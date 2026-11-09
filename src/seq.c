/* seq.c — Psy-Q compatible SEQ + VAB player on top of PSn00bSDK SPU.
 *
 * The file formats and playback model follow Sony Psy-Q's libsnd:
 *   pQES SEQ  -> MIDI-like event stream
 *   pBAV VAB  -> program/tone table + VAG ADPCM body
 *
 * We deliberately do not link Psy-Q's LIBSND.LIB. The project remains a
 * normal PSn00bSDK/CMake build; this is a small native implementation of the
 * documented Psy-Q data model using PSn00bSDK's SPU register/DMA API.
 */

#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxspu.h>
#include "vn.h"

#define SPU_BASE       0x1010
#define SEQ_VOICES     20      /* голоса 0..19 — музыка */
#define SE_VOICES       4      /* голоса 20..23 — короткие звуки */
#define SPU_TOTAL_VOICES 24
#define SE_RATE     22050      /* частота, с которой кодируются эффекты */
#define MAX_CHAN       16
#define MAX_PROGS      128
#define MAX_TONES      (MAX_PROGS * VAB_TONES)
#define MAX_VAGS       256
#define VAB_PROGS      128
#define VAB_TONES      16
#define VAB_PROG_SIZE  16
#define VAB_TONE_SIZE  32
#define VAB_HDR_SIZE   32
#define VAB_INDEX_SIZE 512
#define VAB_PROG_TABLE (VAB_PROGS * VAB_PROG_SIZE)
#define VAB_BUF_SIZE   (288 * 1024)  /* банк вырос: 6 инструментов + 16 ударных */
#define SEQ_BUF_SIZE (128 * 1024)
#define NO_LOOP        0

static u_char vab_buf[VAB_BUF_SIZE] __attribute__((aligned(4)));
static u_char seq_buf[SEQ_BUF_SIZE] __attribute__((aligned(4)));

static const u_char *seq_data;
static u_int seq_len;
static u_int pc;
static u_int loop_pc;
static u_int loop_tempo;
static u_int ppqn;
static u_int tempo_us;
static int playing;
static int next_delta;
static int64_t delta_fp;
static int64_t ticks_per_frame_fp;
static u_char running_status;
static int master_vol = 90;
static int fade_target = 90;
static int fade_frames;
static int fade_stop;
static int loop_enabled = 1;
/* Рабочая громкость, заданная сценарием (bgm volume). К ней возвращаются
   новый трек и плавное появление. master_vol — текущая, она гуляет при
   затуханиях и после bgm fadeout остаётся нулём; если новый трек брал бы
   её, то после первой же концовки музыка больше не звучала бы. */
static int base_vol = SEQ_VOL_NORMAL;

static u_int vab_body_off;
static u_int vab_body_len;
static u_int vab_prog_count;
static u_int vab_vag_count;

static u_char ch_prog[MAX_CHAN];
static u_char ch_vol[MAX_CHAN];
static u_char ch_pan[MAX_CHAN];

typedef struct {
    u_char valid;
    u_char tones;
    u_char vol;
    u_char pan;
} Program;

typedef struct {
    u_char valid;
    u_char vol;
    u_char pan;
    u_char center;
    u_char shift;
    u_char min;
    u_char max;
    u_short adsr1;
    u_short adsr2;
    u_short prog;
    u_short vag;
} Tone;

typedef struct {
    u_char used;
    u_char ch;
    u_char note;
    u_char vel;
    u_short age;
} Voice;

static Program programs[MAX_PROGS];
static Tone tones[MAX_TONES];
static Voice voices[SEQ_VOICES];
static u_short age_counter;

/*
 * Psy-Q compatible SEQ/VAB fallback:
 * The current test VAB contains only program 0.  A SEQ can legitimately
 * request other GM programs, so unresolved programs fall back to program 0.
 * This keeps the sequencer useful while a multi-program VAB is assembled.
 */
static int seq_resolve_program(int requested)
{
    if (requested < 0 || requested >= 128)
        requested = 0;

    /* Current VAB builder exposes the number of programs. */
    if (requested >= vab_prog_count)
        requested = 0;

    return requested;
}

static void voice_update_volumes(void);

static u_short rd16le(const u_char *p)
{
    return (u_short)(p[0] | ((u_short)p[1] << 8));
}

static u_short rd16be(const u_char *p)
{
    return (u_short)(((u_short)p[0] << 8) | p[1]);
}

static u_int rd24be(const u_char *p)
{
    return ((u_int)p[0] << 16) | ((u_int)p[1] << 8) | p[2];
}

static int read_vlq(void)
{
    u_int v = 0;
    int n = 0;
    while (pc < seq_len && n++ < 4) {
        u_char b = seq_data[pc++];
        v = (v << 7) | (b & 0x7f);
        if (!(b & 0x80)) return (int)v;
    }
    playing = 0;
    return 0;
}

static void set_tempo(u_int us)
{
    if (!us) us = 500000;
    tempo_us = us;
    /* 60 Hz main-loop equivalent to Psy-Q SS_TICK60. Fixed point avoids
       cumulative integer truncation. */
    ticks_per_frame_fp = ((int64_t)ppqn * 1000000LL << 16) /
                         ((int64_t)tempo_us * 60LL);
    if (ticks_per_frame_fp < 1) ticks_per_frame_fp = 1;
}

static u_short pitch_from_note(const Tone *t, int note)
{
    int d = note - (int)t->center;
    int oct = d / 12;
    int rem = d % 12;
    static const u_short semi[12] = {
        4096, 4340, 4598, 4871, 5161, 5468,
        5793, 6137, 6502, 6889, 7298, 7732
    };
    u_int p = 4096;

    if (rem < 0) { rem += 12; oct--; }
    p = (p * semi[rem]) >> 12;
    if (oct > 0) p <<= oct;
    else if (oct < 0) p >>= -oct;

    /* Tone shift is a fine pitch adjustment in 1/64 semitone units in the
       Psy-Q tone table. Keep the common zero-shift case exact. */
    if (t->shift) {
        int fine = (int)t->shift - 64;
        if (fine > 0) p += (p * fine) / 768;
        else p -= (p * (-fine)) / 768;
    }

    if (p > 0x3fff) p = 0x3fff;
    if (p < 1) p = 1;
    return (u_short)p;
}

static void voice_off(int v)
{
    if (!voices[v].used) return;
    SpuSetKey(0, 1u << v);
    voices[v].used = 0;
}

static int find_tone(int prog, int note)
{
    int i;
    prog = seq_resolve_program(prog);
    if (prog < 0 || prog >= MAX_PROGS || !programs[prog].valid)
        return -1;
    /* Current converter uses one full-range tone per program. The loop is
       deliberately general so a multi-tone VAB can be dropped in later. */
    for (i = 0; i < VAB_TONES; i++) {
        int ti = prog * VAB_TONES + i;
        if (ti >= (int)vab_prog_count * VAB_TONES) break;
        if (tones[ti].valid) {
            Tone *t = &tones[ti];
            if (note >= t->min && note <= t->max) return ti;
        }
    }
    return -1;
}

static void voice_update_volume(int v)
{
    int ch, ti, prog, vol, l, r;
    Tone *t;
    if (!voices[v].used) return;
    ch = voices[v].ch & 15;
    prog = ch_prog[ch];
    ti = find_tone(prog, voices[v].note);
    if (ti < 0) return;
    t = &tones[ti];
    vol = voices[v].vel * ch_vol[ch] * t->vol * master_vol / (127 * 127 * 127);
    if (vol > 127) vol = 127;
    vol = vol * 0x3fff / 127;
    l = vol * (127 - ch_pan[ch]) / 64;
    r = vol * ch_pan[ch] / 64;
    if (l > 0x3fff) l = 0x3fff;
    if (r > 0x3fff) r = 0x3fff;
    SpuSetVoiceVolume(v, l, r);
}

static void voice_update_volumes(void)
{
    int v;
    for (v = 0; v < SEQ_VOICES; v++) voice_update_volume(v);
}

/*
 * Адрес сэмпла в звуковой памяти по его номеру в банке.
 *
 * Тело VAB — это ADPCM всех сэмплов подряд, а таблица в заголовке хранит
 * длину каждого в восьмибайтовых единицах. Считаем смещение сложением.
 * Возвращает -1, если номера нет.
 */
static int vag_addr(int vag)
{
    const u_char *idx = vab_buf + vab_body_off - VAB_INDEX_SIZE;
    u_int off = 0;
    int i;

    if (vag <= 0 || vag > vab_vag_count) return -1;
    for (i = 1; i < vag; i++)
        off += (u_int)rd16le(idx + i * 2) * 8;
    if (off >= vab_body_len) return -1;
    return (int)(SPU_BASE + off);
}

static void note_on(int ch, int note, int vel)
{
    int v, free = -1, oldest = 0;
    int prog = ch_prog[ch & 15];
    int ti = find_tone(prog, note);
    Tone *t;
    int vol, l, r;

    if (ti < 0 || ti >= MAX_TONES) {
        LOG("seq: no tone prog=%d note=%d\n", prog, note);
        return;
    }
    t = &tones[ti];
    if (!t->valid || t->vag == 0 || t->vag > vab_vag_count) return;

    for (v = 0; v < SEQ_VOICES; v++) {
        if (!voices[v].used) { free = v; break; }
        if (voices[v].age < voices[oldest].age) oldest = v;
    }
    if (free < 0) {
        voice_off(oldest);
        free = oldest;
    }
    v = free;

    vol = vel * ch_vol[ch] * t->vol * master_vol / (127 * 127 * 127);
    if (vol > 127) vol = 127;
    vol = vol * 0x3fff / 127;

    l = vol * (127 - ch_pan[ch]) / 64;
    r = vol * ch_pan[ch] / 64;
    if (l > 0x3fff) l = 0x3fff;
    if (r > 0x3fff) r = 0x3fff;

    {
        int addr = vag_addr(t->vag);
        if (addr < 0) return;
        SpuSetVoiceStartAddr(v, (u_int)addr);
    }

    SpuSetVoiceVolume(v, l, r);
    SpuSetVoicePitch(v, pitch_from_note(t, note));
    /* VAB stores the complete SPU ADSR register words. Write them verbatim. */
    SPU_CH_ADSR1(v) = t->adsr1;
    SPU_CH_ADSR2(v) = t->adsr2;
    SpuSetKey(1, 1u << v);

    voices[v].used = 1;
    voices[v].ch = (u_char)ch;
    voices[v].note = (u_char)note;
    voices[v].vel = (u_char)vel;
    voices[v].age = ++age_counter;
}

static void note_off(int ch, int note)
{
    int v;
    for (v = 0; v < SEQ_VOICES; v++) {
        if (voices[v].used && voices[v].ch == ch && voices[v].note == note) {
            voice_off(v);
            return;
        }
    }
}

static void seq_reset_channels(void)
{
    int i;
    for (i = 0; i < MAX_CHAN; i++) {
        ch_prog[i] = 0;
        ch_vol[i] = 127;
        ch_pan[i] = 64;
    }
}

/*
 * Пока главный код меняет банк или трек, тик обязан молчать: он теперь
 * дёргается ещё и во время чтения с диска, а seq_play как раз читает диск
 * посреди собственной настройки. Без этой защиты секвенсор проигрывал бы
 * наполовину подменённое состояние.
 */
static volatile int seq_lock;

static void seq_stop_unlocked(void);

/*
 * Короткий звук по номеру сэмпла в банке.
 *
 * Играет на голосах ВЫШЕ тех, что занимает музыка: иначе эффект воровал бы
 * голос у мелодии и обрывал ноту. Голоса перебираются по кругу, поэтому
 * несколько эффектов подряд накладываются, а не глушат друг друга.
 */
void seq_play_se(int vag, int vol)
{
    static int next;
    int v, addr, l, r;

    if (!vab_vag_count) return;      /* банк не загружен */

    addr = vag_addr(vag);
    if (addr < 0) {
        LOG("seq: no sample %d for se\n", vag);
        return;
    }

    v = SEQ_VOICES + next;
    if (v >= SPU_TOTAL_VOICES) return;
    next = (next + 1) % SE_VOICES;

    if (vol < 0) vol = 100;
    if (vol > 127) vol = 127;
    /* От рабочей громкости, а не от текущей: во время затухания музыки
       (и после него, когда она равна нулю) эффекты иначе пропадали бы. */
    l = r = vol * base_vol / 127 * 0x3000 / 127;
    if (l > 0x3fff) l = r = 0x3fff;

    SpuSetKey(0, 1u << v);                  /* глушим прошлый на этом голосе */
    SpuSetVoiceVolume(v, (short)l, (short)r);
    SpuSetVoiceStartAddr(v, (u_int)addr);
    SpuSetVoicePitch(v, getSPUSampleRate(SE_RATE));

    /* Огибающая как у обычной ноты: быстрая атака, естественное затухание. */
    SPU_CH_ADSR1(v) = 0x80FF;
    SPU_CH_ADSR2(v) = 0x09CE;
    SpuSetKey(1, 1u << v);
}

void seq_stop(void)
{
    seq_lock = 1;
    seq_stop_unlocked();
    seq_lock = 0;
}

static void seq_stop_unlocked(void)
{
    int v;
    playing = 0;
    fade_frames = 0;
    fade_stop = 0;
    for (v = 0; v < SEQ_VOICES; v++) voice_off(v);
}

static void seq_fade_tick(void)
{
    int diff;
    if (!fade_frames) return;
    diff = fade_target - master_vol;
    if (!diff) {
        fade_frames = 0;
        if (fade_stop) seq_stop();
        return;
    }
    /* Bresenham-like integer ramp: exactly reaches the target on the last
       requested frame without a floating point dependency on the PS1. */
    if (diff > 0) {
        int step = (diff + fade_frames - 1) / fade_frames;
        master_vol += step;
        if (master_vol > fade_target) master_vol = fade_target;
    } else {
        int step = ((-diff) + fade_frames - 1) / fade_frames;
        master_vol -= step;
        if (master_vol < fade_target) master_vol = fade_target;
    }
    fade_frames--;
    voice_update_volumes();
    if (!fade_frames && master_vol == fade_target && fade_stop)
        seq_stop();
}

static void exec_event(void)
{
    u_char status, type, ch, a, b;

    if (pc >= seq_len) {
        seq_stop();
        return;
    }

    status = seq_data[pc++];
    if (status & 0x80) {
        if (status < 0xF0)
            running_status = status;
    } else {
        if (!running_status) { seq_stop(); return; }
        pc--;
        status = running_status;
    }

    type = status & 0xf0;
    ch = status & 0x0f;

    switch (type) {
    case 0x80:
        if (pc + 1 > seq_len) { seq_stop(); return; }
        note_off(ch, seq_data[pc++]);
        if (pc < seq_len) pc++; /* release velocity */
        break;
    case 0x90:
        if (pc + 2 > seq_len) { seq_stop(); return; }
        a = seq_data[pc++]; b = seq_data[pc++];
        if (b) note_on(ch, a, b);
        else note_off(ch, a);
        break;
    case 0xA0:
        if (pc + 2 > seq_len) { seq_stop(); return; }
        pc += 2;
        break;
    case 0xB0:
        if (pc + 2 > seq_len) { seq_stop(); return; }
        a = seq_data[pc++]; b = seq_data[pc++];
        if (a == 7) ch_vol[ch] = b;
        else if (a == 10) ch_pan[ch] = b;
        else if (a == 99 && b == 20) {
            loop_pc = pc;
            loop_tempo = tempo_us;
        } else if (a == 99 && b == 30) {
            if (loop_enabled && loop_pc != NO_LOOP) {
                pc = loop_pc;
                set_tempo(loop_tempo);
                delta_fp = 0;
            }
        }
        break;
    case 0xC0:
        if (pc >= seq_len) { seq_stop(); return; }
        ch_prog[ch] = seq_data[pc++] & 127;
        break;
    case 0xD0:
        if (pc >= seq_len) { seq_stop(); return; }
        pc++;
        break;
    case 0xE0:
        if (pc + 2 > seq_len) { seq_stop(); return; }
        pc += 2;
        break;
    case 0xF0:
        if (status == 0xFF) {
            if (pc >= seq_len) { seq_stop(); return; }
            a = seq_data[pc++];
            if (a == 0x51) {
                if (pc + 3 > seq_len) { seq_stop(); return; }
                set_tempo(rd24be(seq_data + pc));
                pc += 3;
            } else if (a == 0x2f) {
                if (loop_enabled && loop_pc != NO_LOOP) {
                    pc = loop_pc;
                    set_tempo(loop_tempo);
                    delta_fp = 0;
                } else {
                    seq_stop();
                }
            } else if (a == 0x58) {
                /* SS SEQ commonly carries time-signature data. The converter
                   writes four payload bytes. */
                if (pc + 4 > seq_len) { seq_stop(); return; }
                pc += 4;
            } else {
                /* Unknown Sony meta events have no generic length byte.
                   Refuse to guess and stop rather than desynchronizing the
                   stream. */
                seq_stop();
            }
        } else {
            seq_stop();
        }
        break;
    default:
        seq_stop();
        break;
    }
}

int seq_load_bank(const char *path)
{
    int len;
    u_char *b = vab_buf;
    u_int ps, ts, vs;
    u_int tone_off, index_off, body_off;

    /* ts используется только в сообщении о загрузке, а в релизной сборке
       печать исчезает — иначе компилятор справедливо ругается. */
    (void) ts;
    int p, t;

    memset(programs, 0, sizeof(programs));
    memset(tones, 0, sizeof(tones));
    vab_body_off = vab_body_len = vab_prog_count = vab_vag_count = 0;

    seq_lock = 1;
    if (!cd_load_to(path, b, sizeof(vab_buf), &len)) { seq_lock = 0; return -1; }
    if (len < VAB_HDR_SIZE || memcmp(b, "pBAV", 4) != 0) {
        LOG("seq: %s is not a Psy-Q VAB\n", path);
        seq_lock = 0;
        return -1;
    }

    ps = rd16le(b + 18);
    ts = rd16le(b + 20);
    vs = rd16le(b + 22);
    if (ps > VAB_PROGS) ps = VAB_PROGS;
    if (vs > MAX_VAGS - 1) vs = MAX_VAGS - 1;
    if (ps == 0) ps = 1;

    tone_off = VAB_HDR_SIZE + VAB_PROG_TABLE;
    index_off = tone_off + ps * VAB_TONES * VAB_TONE_SIZE;
    body_off = index_off + VAB_INDEX_SIZE;
    if (body_off > (u_int)len) {
        LOG("seq: malformed VAB tables\n");
        seq_lock = 0;
        return -1;
    }

    vab_prog_count = ps;
    vab_vag_count = vs;
    vab_body_off = body_off;
    vab_body_len = (u_int)len - body_off;

    for (p = 0; p < (int)ps; p++) {
        const u_char *pa = b + VAB_HDR_SIZE + p * VAB_PROG_SIZE;
        programs[p].tones = pa[0];
        programs[p].vol = pa[1];
        programs[p].pan = pa[4];
        programs[p].valid = pa[0] != 0;
    }

    for (p = 0; p < (int)ps; p++) {
        for (t = 0; t < VAB_TONES; t++) {
            int ti = p * VAB_TONES + t;
            const u_char *ta = b + tone_off + ti * VAB_TONE_SIZE;
            if (ti >= (int)(ps * VAB_TONES)) break;
            tones[ti].valid = 0;
            if (!programs[p].valid || t >= programs[p].tones) continue;
            tones[ti].valid = 1;
            tones[ti].vol = ta[2];
            tones[ti].pan = ta[3];
            tones[ti].center = ta[4];
            tones[ti].shift = ta[5];
            tones[ti].min = ta[6];
            tones[ti].max = ta[7];
            tones[ti].adsr1 = rd16le(ta + 16);
            tones[ti].adsr2 = rd16le(ta + 18);
            tones[ti].prog = rd16le(ta + 20);
            tones[ti].vag = rd16le(ta + 22);
        }
    }

    SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
    SpuSetTransferStartAddr(SPU_BASE);
    SpuWrite((const uint32_t *)(b + body_off), vab_body_len);
    SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
    SpuSetCommonMasterVolume(0x3000, 0x3000);

    LOG("seq: VAB ok, programs %u, tones %u, VAGs %u, body %u KB\n",
           ps, ts, vs, vab_body_len / 1024);
    seq_lock = 0;
    return 0;
}

int seq_play(const char *path, int loop)
{
    int len;
    u_int tempo;

    seq_lock = 1;                  /* снимается на каждом выходе */
    seq_stop_unlocked();
    if (!cd_load_to(path, seq_buf, sizeof(seq_buf), &len)) {
        seq_lock = 0;
        return -1;
    }
    if (len < 14 || memcmp(seq_buf, "pQES", 4) != 0) {
        LOG("seq: %s is not Psy-Q SEQ\n", path);
        seq_lock = 0;
        return -1;
    }

    if (rd16be(seq_buf + 4) != 1) {
        LOG("seq: unsupported SEQ version %u\n", rd16be(seq_buf + 4));
        seq_lock = 0;
        return -1;
    }

    ppqn = rd16be(seq_buf + 6);
    tempo = rd24be(seq_buf + 8);
    if (!ppqn) ppqn = 480;
    if (!tempo) tempo = 500000;

    seq_data = seq_buf + 13;
    seq_len = (u_int)len - 13;
    pc = 0;
    loop_pc = NO_LOOP;
    loop_tempo = tempo;
    loop_enabled = loop ? 1 : 0;
    running_status = 0;
    master_vol = fade_target = base_vol;
    seq_reset_channels();
    set_tempo(tempo);
    fade_frames = 0;
    fade_stop = 0;

    next_delta = read_vlq();
    delta_fp = (int64_t)next_delta << 16;
    playing = 1;
    seq_lock = 0;
    return 0;
}

void seq_tick(void)
{
    if (seq_lock) return;          /* идёт смена банка или трека */

    seq_fade_tick();
    if (!playing) return;

    delta_fp -= ticks_per_frame_fp;
    while (playing && delta_fp <= 0) {
        exec_event();
        if (!playing) break;
        next_delta = read_vlq();
        delta_fp += (int64_t)next_delta << 16;
    }
}

int seq_playing(void)
{
    return playing;
}

void seq_set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    master_vol = v;
    base_vol = v;
    fade_target = v;
    fade_frames = 0;
    voice_update_volumes();
    fade_stop = 0;
}

int seq_base_volume(void) { return base_vol; }

/* Плавно к громкости v. Трек НЕ останавливается даже на нуле: так
   fade out ... music и fade in ... music возвращают ту же музыку с того
   же места, как и обещает сценарий. */
void seq_fade_to(int v, int frames)
{
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    if (frames < 1) frames = 1;
    fade_target = v;
    fade_frames = frames;
    fade_stop = 0;
}

/* bgm fadeout: увести в тишину и остановить трек. */
void seq_fade_out(int frames)
{
    seq_fade_to(0, frames);
    fade_stop = 1;
}

/* bgm fadein: из тишины к рабочей громкости. */
void seq_fade_in(int frames)
{
    master_vol = 0;
    voice_update_volumes();
    seq_fade_to(base_vol, frames);
}
