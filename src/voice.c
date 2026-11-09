/* voice.c -- озвучка репликами из общего файла XA
 *
 * Все реплики лежат в одном VOICE.XAS вперемешку по каналам: поток
 * 18900 Гц моно занимает лишь каждый шестнадцатый сектор, и в свободные
 * слоты положены другие реплики. Чтобы проиграть нужную, встаём на начало
 * её пачки и ставим фильтр привода на её канал — остальные каналы
 * контроллер просто отбросит.
 *
 * Декодирует железо, процессор не участвует. Музыка живёт в SPU и на
 * привод не опирается, поэтому голос и музыка звучат одновременно.
 *
 * Плата за это: пока идёт реплика, диск занят. Любая загрузка картинки
 * оборвёт её, поэтому cd_load_to() глушит голос сам.
 */

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <psxcd.h>
#include <psxgpu.h>
#include "vn.h"

/* Указатель целиком живёт в памяти: так реплика стартует без лишнего
   обращения к диску. 96 КБ хватает на 12286 реплик — для полной озвучки
   новеллы с запасом. */
#define VOICE_IDX_MAX (96 * 1024)
#define VOICE_ENTRY   8
#define VOICE_HDR     12

static CdlFILE xas_file;
static int     voice_ready;
static int     voice_count;
static int     voice_lanes;
static int     voice_fps_q8;   /* кадров развёртки на сектор, 8.8 */
static u_char  voice_idx[VOICE_IDX_MAX] __attribute__((aligned(4)));

static int     playing;
static int     frames_left;

/*
 * Режим выставляется перед КАЖДОЙ репликой.
 *
 * Раньше здесь стоял флаг «режим уже наш», чтобы не тратить лишнюю команду
 * приводу. Затея оказалась неверной: режим меняют четыре разных места —
 * чтение файла, чтение картинки из общего файла, видео и мы сами, — а
 * помнил об этом только один флаг. Стоило пропустить одно место (загрузку
 * картинок), и после первой же смены фона привод оставался в чужом режиме:
 * читал на двойной скорости вместо одинарной, а аудиосекторы уходили в
 * буфер данных вместо звука.
 *
 * Одна команда на реплику дешевле, чем класс ошибок, который проявляется
 * не сразу и выглядит как «первые две реплики звучат, дальше тишина».
 */
static void voice_apply_mode(void)
{
    /* RT — звук уходит в SPU сам; SF — включает фильтр по файлу и каналу.
       Бит скорости снят намеренно: поток свёрстан под одинарную. */
    u_char mode = CdlModeRT | CdlModeSF;
    CdControlB(CdlSetmode, &mode, 0);
}

void voice_mode_dirty(void) { }   /* оставлено ради совместимости вызовов */

static u_int rd32(const u_char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u_int)p[3] << 24);
}
static u_int rd16(const u_char *p) { return p[0] | (p[1] << 8); }

/*
 * Открывает файл озвучки и читает указатель. Поиск по каталогу случается
 * один раз — дальше привод встаёт сразу на нужный сектор.
 */
int voice_open(const char *xas, const char *idx)
{
    int len;

    voice_ready = 0;

    if (!CdSearchFile(&xas_file, (char *)xas)) {
        LOG("voice: no %s\n", xas);
        return -1;
    }
    if (!cd_load_to(idx, voice_idx, sizeof(voice_idx), &len)) {
        LOG("voice: no %s\n", idx);
        return -1;
    }
    if (len < VOICE_HDR || memcmp(voice_idx, "VIDX", 4) != 0) {
        LOG("voice: %s is not VIDX\n", idx);
        return -1;
    }

    voice_count = (int)rd16(voice_idx + 4);
    voice_lanes = (int)rd16(voice_idx + 6);
    voice_fps_q8 = (int)rd16(voice_idx + 8);

    if (VOICE_HDR + voice_count * VOICE_ENTRY > len) {
        LOG("voice: index too big (%d lines)\n", voice_count);
        return -1;
    }

    voice_ready = 1;
    LOG("voice: ok, %d lines, %d lanes\n", voice_count, voice_lanes);
    return 0;
}

void voice_stop(void)
{
    if (!playing) return;

    /* Пауза обязательна: без неё привод продолжает лить поток дальше по
       диску, попадая в чужие пачки. В логе это выглядит как бесконечная
       вереница секторов после конца реплики. */
    CdControlB(CdlPause, 0, 0);
    playing = 0;
    frames_left = 0;
}

int voice_playing(void) { return playing; }

/*
 * Запускает реплику по номеру.
 *
 * Фильтр обязателен: без него контроллер отдал бы в звук ВСЕ каналы пачки
 * разом, то есть все реплики одновременно.
 */
int voice_play(int index)
{
    const u_char *e;
    CdlFILTER filter;
    CdlLOC    loc;
    u_int     group, lane, frames;

    if (!voice_ready || index < 0 || index >= voice_count)
        return -1;

    voice_stop();

    e      = voice_idx + VOICE_HDR + index * VOICE_ENTRY;
    group  = rd32(e);
    lane   = e[4];
    /* Длительность считаем из числа секторов, а не храним для каждой
       реплики: при тысячах реплик это заметная экономия памяти. */
    frames = (rd16(e + 6) * (u_int)voice_fps_q8) >> 8;
    frames += 30;                 /* небольшой запас на раскрутку */

    /* Режим ставится перед каждой репликой — см. voice_apply_mode(). */
    voice_apply_mode();

    filter.file = 0;
    filter.chan = (u_char)lane;
    CdControlB(CdlSetfilter, (u_char *)&filter, 0);

    CdIntToPos(CdPosToInt(&xas_file.pos) + (int)group, &loc);
    CdControlB(CdlReadS, (u_char *)&loc, 0);

    playing = 1;
    frames_left = (int)frames;

    LOG("voice: %d -> group %u lane %u, %d frames\n",
           index, group, lane, frames_left);
    return 0;
}

/*
 * Вызывается раз в кадр. Реплика кончается по счётчику из указателя:
 * длину мы знаем точно, а гадать по состоянию привода незачем.
 */
void voice_tick(void)
{
    if (!playing) return;

    if (--frames_left <= 0)
        voice_stop();
}
