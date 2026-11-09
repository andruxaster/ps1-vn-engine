/* vm.c — интерпретатор байткода новеллы + отрисовка сцены */

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <psxpad.h>
#include "vn.h"

/* ---- опкоды (должны совпадать с tools/build_data.py) ---- */
enum {
    OP_END = 0, OP_BG, OP_CHAR, OP_NAME, OP_TEXT, OP_CLEARBOX,
    OP_WAIT, OP_FADE, OP_BGM, OP_SE, OP_CHOICE, OP_JUMP,
    OP_SET, OP_IFJMP, OP_BOX, OP_STR, OP_SPLASH, OP_MENU, OP_SCENE,
    OP_VOICE, OP_CHAPTER, OP_FX, OP_GALLERY
};

enum { S_RUN, S_TEXT, S_WAIT, S_FADE, S_CHOICE, S_END, S_SPLASH, S_MENU };

/* Раскладка текстового окна */
#define BOX_X    8
#define BOX_Y  158
#define BOX_W  304
#define BOX_H   74
#define TXT_X   20
#define TXT_Y  172
#define NAME_X  20
#define NAME_Y  136
/* Скорость печати в 1/256 символа за кадр: целыми символами медленнее
   одного за кадр (60 симв/с) не сделать, а в новеллах читают заметно
   спокойнее. 128 = полсимвола за кадр = ~30 симв/с. */
#define TYPE_SPEED_Q8 128

static const u_char *code;
static const char   *strs;
static u_int  code_len;
static u_int  pc;
static int    state;

static VN_IMG bg_img;

/* Анимированная сцена: кадры лежат в отдельных слотах VRAM и крутятся
   без обращения к диску. */
static VN_IMG scene_img[SCENE_FRAMES];
static int    scene_frames;    /* 0 = сцена не активна */
static int    scene_cur, scene_timer, scene_delay;
static VN_IMG ch_img[CHAR_SLOTS];
static short  ch_x[CHAR_SLOTS], ch_y[CHAR_SLOTS];
static u_char ch_on[CHAR_SLOTS];

static const char *cur_name;
static const char *cur_text;
static int    text_total, text_shown;
static int    text_accum;      /* накопитель в 1/256 символа */
static int    box_visible = 1;

static int    wait_left;
static int    fade_level, fade_target, fade_step;

static const char *ch_str[MAX_CHOICES];
static u_int  ch_target[MAX_CHOICES];
static int    ch_count, ch_sel;

static u_char flags[MAX_FLAGS];

/* ---- экранные эффекты ----
   Ни один из них не занимает видеопамять: тряска двигает область показа,
   вспышка и тонирование рисуют один прямоугольник, сдвиг смещает фон. */
static int shake_left, shake_power;   /* тряска: сколько кадров и на сколько */
static int flash_left, flash_max;     /* вспышка: затухает сама */
static u_char flash_r, flash_g, flash_b;
static int tint_on;
static u_char tint_r, tint_g, tint_b;
static int slide_left, slide_total, slide_dir;
static int bg_off_x, bg_off_y;        /* смещение фона при сдвиге */

/* Прерванный сдвиг (ожидание пропущено кнопкой) не должен утащить за
   собой следующий фон. */
static void slide_cancel(void)
{
    slide_left = 0;
    bg_off_x = bg_off_y = 0;
}

/* Сбрасывает все экранные эффекты: нужен при загрузке сохранения, иначе
   тонирование, тряска или скрытое окно текста перетекали бы из той
   сцены, где игрок нажал «загрузить». */
static void fx_reset(void)
{
    shake_left = 0;
    gfx_shake(0, 0);
    flash_left = 0;
    tint_on = 0;
    slide_cancel();
    box_visible = 1;
}

/* Простой генератор для тряски: настоящая случайность тут не нужна,
   важнее чтобы дрожание не выглядело периодичным. */
static u_int shake_seed = 12345;
static int shake_rand(int amp)
{
    shake_seed = shake_seed * 1103515245u + 12345u;
    return (int)((shake_seed >> 16) % (u_int)(amp * 2 + 1)) - amp;
}


/* Где мы в сценарии с точки зрения сохранений. */
static u_int  cur_chapter;
static u_int  playtime;        /* кадров развёртки с начала главы */

/* Отладочные сохранение и загрузка: L1 пишет, R1 читает. Настоящий экран
   сохранений появится позже — сейчас нужно убедиться, что карта отвечает
   на живой консоли, а прыжок к главе работает. */
static char   save_msg[96];
static int    save_msg_left;

static void save_note(const char *m)
{
    strncpy(save_msg, m, sizeof(save_msg) - 1);
    save_msg[sizeof(save_msg) - 1] = 0;
    save_msg_left = 180;          /* три секунды на экране */
}

/* ---------------------------------------------------------------- */
/* Экран сохранения и загрузки                                        */

/* Имя вынесено в vn.h: файл общий с галереей. */
#define SAVE_FILE card_file_name()

static int  menu_on;          /* 0 нет, 1 сохранение, 2 загрузка */
static int  menu_pick;
static SaveSlot menu_slots[4];
static u_char   menu_used[4];

/* Перечитывает слоты с карты. Делается при открытии экрана, а не каждый
   кадр: обращение к карте не бесплатное. */
static void menu_refresh(void)
{
    int i, n = card_slot_count();
    if (n > 4) n = 4;

    for (i = 0; i < n; i++) {
        menu_used[i] = (card_load_slot(0, SAVE_FILE, i,
                                       &menu_slots[i],
                                       sizeof(SaveSlot)) == 0);
        /* Данные с карты недоверенные: повреждённая запись без нуля в
           конце названия увела бы sprintf далеко за буфер строки. */
        menu_slots[i].title[SAVE_TITLE - 1] = 0;
    }
}

static void menu_open(int mode)
{
    /* Сохраняться можно только на реплике, ждущей нажатия: все прочие
       состояния переходные, и восстанавливать их незачем. */
    if (mode == 1 && !vm_can_save()) {
        save_note("here you cannot save");
        return;
    }
    menu_on = mode;
    menu_pick = 0;
    menu_refresh();
}

static void menu_confirm(void)
{
    SaveSlot slot;
    int err;

    if (menu_on == 1) {
        if (!save_make(&slot, cur_chapter, flags, MAX_FLAGS, playtime)) {
            save_note("cannot build record");
            return;
        }
        if (card_save_slot(0, SAVE_FILE, menu_pick, &slot, sizeof(slot)) < 0) {
            /* Порядок обязан совпадать с CARD_ERR_* в vn.h */
            static const char *why[] = {
                "?", "no icon file", "no card or card full",
                "card read failed", "card write failed",
                "record too big", "write did not land"
            };
            int e = card_last_error();
            save_note(why[e <= CARD_ERR_VERIFY ? e : 0]);
            return;
        }
        save_note("saved");
        menu_on = 0;
        return;
    }

    if (!menu_used[menu_pick]) {
        save_note("slot is empty");
        return;
    }

    err = save_check(&menu_slots[menu_pick]);
    if (err != SAVE_OK) {
        static const char *why[] = {"", "bad format", "old version",
                                    "other build", "no such chapter"};
        save_note(why[err <= SAVE_ERR_CHAPTER ? err : 1]);
        return;
    }

    if (vm_load_save(&menu_slots[menu_pick]) < 0)
        save_note("jump failed");
    else
        save_note("loaded");
    menu_on = 0;
}

static void asset_load(VN_IMG *dst, int id, int slot);

/* ---- экран галереи ----
   Показывает открытые сцены по одной во весь экран. Миниатюр нет
   намеренно: на 320x240 полноразмерный кадр и так размером с миниатюру,
   а отдельные уменьшенные копии съели бы место и видеопамять. */
static int  gal_on;
static int  gal_pick;
static int  gal_shown = -1;      /* какая картинка сейчас загружена */

static void gal_show(void)
{
    int n, i;

    if (gal_pick == gal_shown) return;      /* уже показана */

    n = gal_frame_count(gal_pick);
    if (n <= 0) return;

    if (n == 1 && gal_delay(gal_pick) == 0) {
        /* Обычный фон. */
        asset_load(&bg_img, gal_frame(gal_pick, 0), SLOT_BG);
        scene_frames = 0;
    } else {
        /* Анимированная сцена: пользуемся той же машинерией, что и в
           игре, — кадры в своих слотах видеопамяти и общий счётчик. */
        for (i = 0; i < n; i++)
            asset_load(&scene_img[i], gal_frame(gal_pick, i),
                       SLOT_SCENE0 + i);
        scene_frames = n;
        scene_delay  = gal_delay(gal_pick);
        if (scene_delay < 1) scene_delay = 8;
        scene_cur = scene_timer = 0;
        bg_img.loaded = 0;
    }
    gal_shown = gal_pick;
}

/* Переходит к следующей ОТКРЫТОЙ сцене в заданную сторону. */
static void gal_step(int dir)
{
    int n = gal_count_total(), i, k;

    for (i = 1; i <= n; i++) {
        k = (gal_pick + dir * i + n * (i + 1)) % n;
        if (gal_is_open(k)) {
            gal_pick = k;
            gal_show();
            return;
        }
    }
}

static void gal_open(void)
{
    int n = gal_count_total(), i;

    if (gal_count_open() == 0) {
        save_note("gallery is empty");
        return;
    }
    for (i = 0; i < n; i++)
        if (gal_is_open(i)) { gal_pick = i; break; }

    gal_on = 1;
    gal_shown = -1;
    gal_show();
}

static void gal_update(u_int pressed)
{
    if (pressed & PAD_LEFT)  gal_step(-1);
    if (pressed & PAD_RIGHT) gal_step(1);
    if (pressed & (PAD_TRIANGLE | PAD_SELECT)) {
        gal_on = 0;
        bg_img.loaded = 0;       /* картинку галереи с экрана убираем */
        scene_frames = 0;        /* и анимацию тоже */
        gal_shown = -1;
    }
}

static void gal_draw(void)
{
    /* Название до 39 байт плюс «256/256  » — в 48 не помещалось. */
    char line[64];

    /* Подпись снизу, чтобы не закрывать саму сцену. */
    box_draw(0, SCR_H - 24, SCR_W, 24, 8, 10, 34, 0, Z_BOX);
    box_draw(0, SCR_H - 24, SCR_W, 24, 8, 10, 34, 0, Z_BOX);

    sprintf(line, "%d/%d  %s", gal_pick + 1, gal_count_total(),
            gal_title(gal_pick));
    font_draw(line, -1, 8, SCR_H - 20, Z_TEXT, 240, 230, 190);
}

static void menu_update(u_int pressed)
{
    int n = card_slot_count();
    if (n > 4) n = 4;

    if (pressed & PAD_UP)   menu_pick = (menu_pick + n - 1) % n;
    if (pressed & PAD_DOWN) menu_pick = (menu_pick + 1) % n;
    if (pressed & PAD_CROSS) menu_confirm();
    if (pressed & (PAD_TRIANGLE | PAD_SELECT)) menu_on = 0;
}

static void menu_draw(void)
{
    char line[64];
    int i, y = 60, n = card_slot_count();
    if (n > 4) n = 4;

    /* Высота под заголовок и четыре строки. Раньше окно было выше из-за
       подсказки внизу, и она налезала на текстовое окно игры. */
    box_draw(24, 40, 272, 116, 8, 10, 34, 0, Z_BOX);
    box_draw(24, 40, 272, 116, 8, 10, 34, 0, Z_BOX);

    font_draw(menu_on == 1 ? "СОХРАНЕНИЕ" : "ЗАГРУЗКА",
              -1, 36, 44, Z_TEXT, 255, 220, 120);

    for (i = 0; i < n; i++) {
        int sel = (i == menu_pick);

        if (menu_used[i]) {
            u_int sec = menu_slots[i].playtime / 60;
            sprintf(line, "%d. %s  %d:%02d", i + 1, menu_slots[i].title,
                    (int)(sec / 60), (int)(sec % 60));
        } else {
            sprintf(line, "%d. пусто", i + 1);
        }

        font_draw(line, -1, 40, y, Z_TEXT,
                  sel ? 255 : 150, sel ? 235 : 150, sel ? 140 : 150);
        y += 22;
    }
}

/* --- отладочный оверлей (Select) --- */
/* Имена нужны только оверлею. Держим потолок ниже общего лимита ассетов:
   1024 имени стоят 24 КБ памяти, дальше номера показываются как есть. */
#define DBG_NAMES_MAX 1024
#define DBG_NAME_LEN  24
static int    dbg_bg_id = -1;
static int    dbg_ch_id[CHAR_SLOTS];
static int    dbg_scene_id = -1;

#if !VN_RELEASE
/* В релизе оверлея нет, а значит не нужны ни таблица имён (24 КБ памяти),
   ни её чтение с диска при старте. */
static int    dbg_on;
static u_char dbg_names[DBG_NAMES_MAX * DBG_NAME_LEN];
static int    dbg_have_names;

static const char *dbg_name(int id)
{
    if (!dbg_have_names || id < 0 || id >= DBG_NAMES_MAX) return "?";
    return (const char *)(dbg_names + id * DBG_NAME_LEN);
}
#endif

/* --- сплэш-экран (юр. текст, титры) --- */
static const char *splash_text;
static int    splash_left;         /* кадров до авто-перехода */
static int    splash_fade;         /* плавное появление/уход, 0..255 */
static int    splash_phase;        /* 0 = появляется, 1 = держится, 2 = уходит */

/* --- титульное меню --- */
#define MENU_MAX 4
static const char *menu_item[MENU_MAX];
static u_int  menu_target[MENU_MAX];
static int    menu_count, menu_sel;
static const char *menu_title;     /* строка-заголовок игры */

/* Читаем побайтово: MIPS не умеет невыровненный доступ к слову */
static u_int rd8 (u_int o) { return code[o]; }
static u_int rd16(u_int o) { return code[o] | (code[o+1] << 8); }
static u_int rd32(u_int o) {
    return code[o] | (code[o+1] << 8) | (code[o+2] << 16) | ((u_int)code[o+3] << 24);
}

static u_int str_len;           /* размер пула строк текущей главы */

/* Смещение за пределами пула даёт пустую строку, а не чтение чужой памяти. */
static const char *str_at(u_int off)
{
    return off < str_len ? strs + off : "";
}

static void asset_load(VN_IMG *dst, int id, int slot)
{
    char path[24];
    u_long *tim;

    /* Сначала общий файл: там картинка читается сразу с нужного сектора,
       без поиска по каталогу. Если его нет, работаем по-старому — так
       сборка без ART.PAK остаётся рабочей. */
    if (cd_pak_ready()) {
        tim = cd_pak_load(id, cd_scratch(), cd_scratch_size(), NULL);
        if (tim) {
            img_load_to(dst, tim, slot);
            return;
        }
        LOG("vm: pak entry %d failed, trying file\n", id);
    }

    sprintf(path, "\\DATA\\ART\\A%04d.TIM;1", id);
    tim = cd_load(path, NULL);
    if (tim) img_load_to(dst, tim, slot);
}

/* ---------------------------------------------------------------- */

/*
 * Сценарий разрезан на главы CH00.VNS, CH01.VNS, ... — целиком он в 2 МБ
 * не помещается. В памяти живёт ровно одна глава; адрес перехода несёт
 * номер главы в старшем байте: (глава << 24) | смещение.
 */
static u_char *sbuf;            /* буфер под главу, даёт main.c */
static int     scap;
static int     cur_chunk = -1;
static int     chunk_count;

static u_int hdr32(const u_char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u_int)p[3] << 24);
}

/* Пул строк лежит на диске «зашумлённым» потоком от xorshift32. Это не
   шифрование — снимается за пару минут, — но текст перестаёт вываливаться
   в strings и читаться случайным просмотром файла. Значение обязано
   совпадать со SCRAMBLE_SEED в tools/build_data.py. */
#define SCRAMBLE_SEED 0x5CA1AB1Eu

static void descramble(u_char *p, int n, u_int seed)
{
    u_int x = seed ? seed : 1;          /* ноль вырождает генератор */
    int i = 0;

    while (i < n) {
        u_int k;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        k = x;
        p[i++] ^= (u_char)k;              if (i >= n) break;
        p[i++] ^= (u_char)(k >> 8);       if (i >= n) break;
        p[i++] ^= (u_char)(k >> 16);      if (i >= n) break;
        p[i++] ^= (u_char)(k >> 24);
    }
}

static int load_chunk(int n)
{
    char  path[24];
    u_int code_off, str_off;
    int   len;

    sprintf(path, "\\DATA\\SCR\\CH%02d.VNS;1", n);
    if (!cd_load_to(path, sbuf, scap, &len)) {
        LOG("vm: chapter %d load failed\n", n);
        return -1;
    }
    if (len < 24 || memcmp(sbuf, "VNS3", 4) != 0) {
        LOG("vm: chapter %d is not VNS3\n", n);
        return -1;
    }

    code_off    = hdr32(sbuf + 4);
    code_len    = hdr32(sbuf + 8);
    str_off     = hdr32(sbuf + 12);
    str_len     = hdr32(sbuf + 16);
    chunk_count = sbuf[22] | (sbuf[23] << 8);

    /* Заголовок проверяем по фактической длине файла: битый или чужой
       файл иначе отправил бы интерпретатор читать память за буфером. */
    if (code_off > (u_int)len || code_len > (u_int)len - code_off ||
        str_off > (u_int)len || str_len > (u_int)len - str_off) {
        LOG("vm: chapter %d header is broken\n", n);
        return -1;
    }

    code = sbuf + code_off;
    strs = (const char *)(sbuf + str_off);

    /* Пул строк расшифровываем на месте, один раз на главу (~7 мс на
       60 КБ — теряется на фоне 200 мс чтения с диска). */
    descramble(sbuf + str_off, (int)str_len, SCRAMBLE_SEED ^ (u_int)n);

    cur_chunk = n;

    /* Все указатели вели в пул прошлой главы — он только что затёрт. */
    cur_text = NULL;
    cur_name = NULL;
    splash_text = NULL;
    menu_title = NULL;
    menu_count = 0;
    ch_count = 0;
    return 0;
}

/* Переход по адресу (глава << 24) | смещение */
static void vm_goto(u_int target)
{
    int   ch  = (int)(target >> 24);
    u_int off = target & 0xFFFFFFu;

    if (ch != cur_chunk && load_chunk(ch) < 0) {
        state = S_END;
        return;
    }
    pc = off;
}

int vm_start(u_char *buf, int cap)
{
    int i;

    sbuf = buf;
    scap = cap;
    cur_chunk = -1;

    if (load_chunk(0) < 0)
        return -1;

    /* Таблица имён нужна только оверлею: нет файла — покажем номера. */
#if !VN_RELEASE
    dbg_have_names = cd_load_to("\\DATA\\SYS\\NAMES.DBG;1", dbg_names,
                                sizeof(dbg_names), NULL) ? 1 : 0;
#endif
    for (i = 0; i < CHAR_SLOTS; i++) dbg_ch_id[i] = -1;

    pc = 0;
    state = S_RUN;

    bg_img.loaded = 0;
    scene_frames = 0;
    for (i = 0; i < CHAR_SLOTS; i++) { ch_img[i].loaded = 0; ch_on[i] = 0; }
    memset(flags, 0, sizeof(flags));
    cur_chapter = 0;
    playtime = 0;
    return 0;
}

int vm_finished(void) { return state == S_END; }

/* Выполняем опкоды, пока не упрёмся в ожидание ввода/таймера */
static void vm_step(void)
{
    while (state == S_RUN) {
        int op;

        /* Код главы кончился — подтягиваем следующую и продолжаем с её начала. */
        if (pc >= code_len) {
            if (cur_chunk + 1 < chunk_count && load_chunk(cur_chunk + 1) == 0) {
                pc = 0;
                continue;
            }
            state = S_END;
            return;
        }

        op = rd8(pc);
        switch (op) {

        case OP_END:
            state = S_END;
            return;

        case OP_BG:
            slide_cancel();            /* новый фон встаёт на место */
            dbg_bg_id = (int)rd16(pc + 1);
            dbg_scene_id = -1;
            asset_load(&bg_img, (int)rd16(pc + 1), SLOT_BG);
            scene_frames = 0;          /* обычный фон гасит анимацию сцены */
            /* Смена локации без затемнения — реплика прошлой сцены тоже
               не должна её пережить. */
            cur_text = NULL;
            cur_name = NULL;
            pc += 3;
            break;

        case OP_SCENE: {
            /* scene: n кадров, задержка, затем n номеров ассетов.
               Все кадры грузятся заранее в свои слоты VRAM и потом
               крутятся без обращения к диску. */
            int n = rd8(pc + 1), i;
            slide_cancel();
            scene_delay = rd8(pc + 2);
            if (n > SCENE_FRAMES) n = SCENE_FRAMES;
            for (i = 0; i < n; i++)
                asset_load(&scene_img[i], (int)rd16(pc + 3 + i * 2),
                           SLOT_SCENE0 + i);
            dbg_scene_id = (int)rd16(pc + 3);
            dbg_bg_id = -1;
            scene_frames = n;
            scene_cur = 0;
            scene_timer = 0;
            cur_text = NULL;           /* как и обычный фон, сцена меняет план */
            cur_name = NULL;
            bg_img.loaded = 0;         /* сцена заменяет статический фон */
            pc += 3 + rd8(pc + 1) * 2;
            break;
        }

        case OP_CHAR: {
            /* Раскладка: слот(1) номер(2) x(2) y(2) = 8 байт.
               Признак «убрать» — 0xFFFF. */
            int slot = rd8(pc + 1);
            int id   = (int)rd16(pc + 2);
            if (slot < CHAR_SLOTS) {
                if (id == 0xFFFF) {
                    ch_on[slot] = 0;
                    dbg_ch_id[slot] = -1;
                } else {
                    dbg_ch_id[slot] = id;
                    asset_load(&ch_img[slot], id, char_vram_slot[slot]);
                    ch_x[slot] = (short)rd16(pc + 4);
                    ch_y[slot] = (short)rd16(pc + 6);
                    ch_on[slot] = 1;
                }
            }
            pc += 8;
            break;
        }

        case OP_NAME:
            cur_name = str_at(rd32(pc + 1));
            if (!*cur_name) cur_name = NULL;
            pc += 5;
            break;

        case OP_TEXT:
            cur_text   = str_at(rd32(pc + 1));
            text_total = font_count(cur_text);
            text_shown = 0;
            text_accum = 0;
            box_visible = 1;
            pc += 5;
            state = S_TEXT;
            return;

        case OP_CLEARBOX:
            cur_text = NULL; cur_name = NULL;
            pc += 1;
            break;

        case OP_WAIT:
            wait_left = rd16(pc + 1);
            pc += 3;
            state = S_WAIT;
            return;

        case OP_FADE: {
            int dir = rd8(pc + 1), sp = rd8(pc + 2);
            fade_target = (dir & 1) ? 255 : 0;
            fade_step   = sp ? sp : 8;

            /* Бит 1 — вести громкость музыки вместе с картинкой. Число
               кадров берём то же, что у затемнения, чтобы звук и
               изображение шли синхронно. */
            if (dir & 2) {
                int frames = 255 / fade_step;
                if (frames < 1) frames = 1;
                seq_fade_to((dir & 1) ? 0 : seq_base_volume(), frames);
            }

            pc += 3;
            state = S_FADE;
            return;
        }

        case OP_BGM: {
            int sub = rd8(pc + 1);
            if (sub == 0) {
                /* play <id> [loop|once] */
                int id = rd8(pc + 2);
                int loop = rd8(pc + 3);
                char path[24];
                sprintf(path, "\\DATA\\BGM\\M%03d.SEQ;1", id);
                seq_play(path, loop);
                pc += 4;
            } else if (sub == 1) {
                seq_stop();
                pc += 2;
            } else if (sub == 2) {
                seq_fade_out((int)rd16(pc + 2));
                pc += 4;
            } else if (sub == 3) {
                seq_fade_in((int)rd16(pc + 2));
                pc += 4;
            } else if (sub == 4) {
                seq_set_volume((int)rd8(pc + 2));
                pc += 3;
            } else {
                LOG("vm: bad BGM subcommand %d\n", sub);
                pc += 2;
            }
            break;
        }

        case OP_CHAPTER:
            /* Отметка главы: движок запоминает, где находится, и это
               значение уходит в сохранение. На картинку не влияет. */
            cur_chapter = rd32(pc + 1);
            playtime = 0;
            pc += 5;
            break;

        case OP_GALLERY:
            /* Отмечаем сцену открытой. Запись на карту произойдёт только
               если это впервые — иначе пауза возникала бы на каждом
               повторном прохождении. */
            gal_unlock((int)rd16(pc + 1));
            pc += 3;
            break;

        case OP_FX: {
            int sub = rd8(pc + 1);

            if (sub == 0) {                    /* тряска */
                shake_power = rd8(pc + 2);
                shake_left  = rd8(pc + 3);
            } else if (sub == 1) {             /* вспышка */
                flash_r = rd8(pc + 2);
                flash_g = rd8(pc + 3);
                flash_b = rd8(pc + 4);
                flash_left = flash_max = rd8(pc + 5);
            } else if (sub == 2) {             /* тонирование */
                tint_r = rd8(pc + 2);
                tint_g = rd8(pc + 3);
                tint_b = rd8(pc + 4);
                tint_on = rd8(pc + 5);
            } else {                           /* сдвиг фона через чёрное */
                slide_dir   = rd8(pc + 2);
                slide_total = slide_left = rd8(pc + 3);
                /* Сдвиг ждёт своего конца, как fade out, который он
                   заменяет. Раньше сценарий шёл дальше сразу, и следующий
                   за ним bg успевал загрузиться и уехать вместо старого. */
                wait_left = slide_total;
                pc += 6;
                state = S_WAIT;
                return;
            }
            pc += 6;
            break;
        }

        case OP_VOICE:
            /* Реплика играет фоном: сценарий не ждёт её конца, иначе текст
               застыл бы. Следующая загрузка с диска оборвёт её сама. */
            voice_play((int)rd16(pc + 1));
            pc += 3;
            break;

        case OP_SE:
            /* Номер сэмпла в банке. Играет на голосах, свободных от
               музыки, поэтому ноту не обрывает. */
            seq_play_se(rd8(pc + 1), -1);
            pc += 2;
            break;

        case OP_CHOICE: {
            int n = rd8(pc + 1), i, k = 0;
            u_int p = pc + 2;
            /* Лишние варианты пропускаем, но pc сдвигаем на ВСЕ: иначе он
               попал бы в середину таблицы и дальше исполнялся мусор. */
            for (i = 0; i < n; i++) {
                if (k < MAX_CHOICES) {
                    ch_str[k]    = str_at(rd32(p));
                    ch_target[k] = rd32(p + 4);
                    k++;
                }
                p += 8;
            }
            ch_count = k;
            pc = p;
            if (!k)                        /* пустой выбор: просто идём дальше */
                break;
            ch_sel   = 0;
            state = S_CHOICE;
            return;
        }

        case OP_JUMP:
            vm_goto(rd32(pc + 1));
            break;

        case OP_SET:
            if (rd8(pc + 1) < MAX_FLAGS) flags[rd8(pc + 1)] = rd8(pc + 2);
            pc += 3;
            break;

        case OP_IFJMP: {
            int f = rd8(pc + 1), v = rd8(pc + 2);
            u_int tgt = rd32(pc + 3);
            pc += 7;
            if (f < MAX_FLAGS && flags[f] == v) vm_goto(tgt);
            break;
        }

        case OP_STR: {
            char path[24];
            int  id      = rd8(pc + 1);
            int  ticks   = rd8(pc + 2);
            int  nframes = rd16(pc + 3);
            int  skip    = rd8(pc + 5);
            pc += 6;

            /* Ролик проигрывается синхронно: для новеллы это нормально,
               параллельно всё равно ничего не происходит. */
            sprintf(path, "\\DATA\\STR\\V%03d.STR;1", id);
            str_play(path, nframes, ticks, skip);
#if !VN_RELEASE
            /* Отчёт ролика — сразу на экран, на десять секунд: если видео
               не пошло на железе, его можно просто сфотографировать. */
            save_note(str_last_report());
            save_msg_left = 600;
#endif

            /* Видео затёрло кадровые буферы, а текстуры фона и персонажей
               лежат выше по VRAM и уцелели — сцена восстановится сама
               на ближайшей перерисовке. Экран пока чёрный. */
            fade_level = 255;
            fade_target = 255;
            break;
        }

        case OP_BOX:
            box_visible = rd8(pc + 1);
            pc += 2;
            break;

        case OP_SPLASH:
            /* splash <строка> <кадров держать>. Появление/уход — по 16 кадров. */
            splash_text = str_at(rd32(pc + 1));
            splash_left = rd16(pc + 5);
            pc += 7;
            splash_fade  = 255;        /* стартуем с чёрного */
            splash_phase = 0;
            state = S_SPLASH;
            return;

        case OP_MENU: {
            /* menu <заголовок> <N> { <строка> <метка> } */
            int n, i;
            u_int p;
            menu_title = str_at(rd32(pc + 1));
            if (!*menu_title) menu_title = NULL;
            n = rd8(pc + 5);
            p = pc + 6;
            menu_count = 0;
            for (i = 0; i < n; i++) {          /* лишние пропускаем, см. CHOICE */
                if (menu_count < MENU_MAX) {
                    menu_item[menu_count]   = str_at(rd32(p));
                    menu_target[menu_count] = rd32(p + 4);
                    menu_count++;
                }
                p += 8;
            }
            if (!menu_count) {
                pc = p;
                break;
            }
            menu_sel   = 0;
            pc = p;
            state = S_MENU;
            return;
        }

        default:
            LOG("vm: bad opcode %d at %d\n", op, (int)pc);
            state = S_END;
            return;
        }
    }
}

/* Продвигает экранные эффекты. Вызывается каждый кадр независимо от
   состояния сценария: иначе тряска замирала бы на время реплики. */
static void fx_tick(void)
{
    if (shake_left > 0) {
        shake_left--;
        gfx_shake(shake_rand(shake_power), shake_rand(shake_power));
        if (shake_left == 0) gfx_shake(0, 0);   /* вернуть на место */
    }
    if (flash_left > 0) flash_left--;

    if (slide_left > 0) {
        int t = slide_total - slide_left;    /* сколько прошло */
        int d = SCR_W * t / slide_total;

        slide_left--;
        switch (slide_dir) {
        case 0: bg_off_x = -d; break;        /* уезжает влево */
        case 1: bg_off_x =  d; break;
        case 2: bg_off_y = -SCR_H * t / slide_total; break;
        default: bg_off_y = SCR_H * t / slide_total; break;
        }
        if (slide_left == 0) {
            /* Доехали: фон целиком за краем. Убираем его совсем и только
               потом сбрасываем смещение — иначе старый фон на миг
               возвращался на место до загрузки нового. */
            bg_img.loaded = 0;
            scene_frames = 0;
            bg_off_x = bg_off_y = 0;
        }
    }
}

void vm_update(void)
{
    playtime++;
    fx_tick();
    u_int pressed = pad_pressed();
    int   confirm = pressed & (PAD_CROSS | PAD_CIRCLE | PAD_START);

    if (save_msg_left > 0) save_msg_left--;

    /* Пока открыт экран сохранений, он забирает ввод целиком: иначе
       крестик пролистывал бы реплики за спиной у меню. */
    /* Галерея забирает ввод целиком, как и экран сохранений. */
    if (gal_on) {
        gal_update(pressed);
        return;
    }

    if (menu_on) {
        menu_update(pressed);
        return;
    }

#if !VN_RELEASE
    if (pressed & PAD_SELECT) dbg_on = !dbg_on;
#endif   /* Select — оверлей */
    if (pressed & PAD_L1) menu_open(1);           /* сохранить */
    if (pressed & PAD_R1) menu_open(2);           /* загрузить */

    switch (state) {

    case S_RUN:
        break;

    case S_TEXT:
        if (text_shown < text_total) {
            text_accum += TYPE_SPEED_Q8;
            text_shown += text_accum >> 8;      /* целые символы наружу */
            text_accum &= 0xFF;                 /* дробный остаток копится */
            if (text_shown > text_total) text_shown = text_total;
            if (confirm) text_shown = text_total;   /* пропустить печать */
        } else if (confirm) {
            state = S_RUN;
        }
        break;

    case S_WAIT:
        if (--wait_left <= 0 || confirm) state = S_RUN;
        break;

    case S_FADE:
        if (fade_level < fade_target) {
            fade_level += fade_step;
            if (fade_level >= fade_target) { fade_level = fade_target; state = S_RUN; }
        } else if (fade_level > fade_target) {
            fade_level -= fade_step;
            if (fade_level <= fade_target) { fade_level = fade_target; state = S_RUN; }
        } else {
            state = S_RUN;
        }
        /* Экран стал полностью чёрным — гасим реплику вместе с картинкой.
           Иначе старый текст переживает затемнение и потом проявляется уже
           поверх НОВОГО фона: зритель видит прошлую реплику в новой сцене.
           Момент выбран так, что смена невидима — экран и так чёрный. */
        if (state == S_RUN && fade_level >= 255) {
            cur_text = NULL;
            cur_name = NULL;
        }
        break;

    case S_CHOICE:
        if (pressed & PAD_UP)   ch_sel = (ch_sel + ch_count - 1) % ch_count;
        if (pressed & PAD_DOWN) ch_sel = (ch_sel + 1) % ch_count;
        if (confirm) { state = S_RUN; vm_goto(ch_target[ch_sel]); }
        break;

    case S_SPLASH:
        /* фаза 0: проявляемся; 1: держимся splash_left кадров; 2: гаснем.
           Крестик/Start перескакивает сразу к затуханию. */
        if (splash_phase == 0) {
            splash_fade -= 16;
            if (splash_fade <= 0) { splash_fade = 0; splash_phase = 1; }
            if (confirm) splash_phase = 2;
        } else if (splash_phase == 1) {
            if (--splash_left <= 0 || confirm) splash_phase = 2;
        } else {
            splash_fade += 16;
            if (splash_fade >= 255) { splash_fade = 255; state = S_RUN; }
        }
        break;

    case S_MENU:
        if (menu_count > 1) {
            if (pressed & PAD_UP)   menu_sel = (menu_sel + menu_count - 1) % menu_count;
            if (pressed & PAD_DOWN) menu_sel = (menu_sel + 1) % menu_count;
        }
        if (confirm) {
            /* Особая цель: пункт «Загрузить» открывает экран сохранений,
               а не прыгает по метке. Титульное меню при этом остаётся на
               экране — если игрок передумает, он вернётся к нему. */
            if (menu_target[menu_sel] == 0xFFFFFFFFu) {
                menu_open(2);
            } else if (menu_target[menu_sel] == 0xFFFFFFFEu) {
                gal_open();
            } else {
                state = S_RUN;
                vm_goto(menu_target[menu_sel]);
            }
        }
        break;

    case S_END:
        return;
    }

    vm_step();
}

/* ---------------------------------------------------------------- */

/* Вспышка и тонирование — по одному прямоугольнику поверх картинки.
   Рисуются под текстом, чтобы реплику не заливало цветом. */
static void fx_draw(void)
{
    if (tint_on)
        box_draw(0, 0, SCR_W, SCR_H, tint_r, tint_g, tint_b, 0, Z_BOX + 1);

    if (flash_left > 0) {
        /* Гаснет к концу: чем меньше осталось, тем прозрачнее. Двойной
           проход в начале даёт плотную вспышку. */
        int n = (flash_left * 2 > flash_max) ? 2 : 1;
        int k;
        for (k = 0; k < n; k++)
            box_draw(0, 0, SCR_W, SCR_H, flash_r, flash_g, flash_b,
                     0, Z_BOX + 1);
    }
}

static int blink;

/* Отладочный оверлей: что сейчас загружено и где мы в сценарии.
   Рисуется на слое Z_DEBUG, то есть поверх затемнения тоже. */
#if !VN_RELEASE
static void dbg_draw(void)
{
    static const char *st_name[] = {
        "RUN", "TEXT", "WAIT", "FADE", "CHOICE", "END", "SPLASH", "MENU"
    };
    char line[64];
    int i, y = 4;

    if (dbg_scene_id >= 0)
        sprintf(line, "SCENE %s x%d k%d", dbg_name(dbg_scene_id),
                scene_frames, scene_cur);
    else
        sprintf(line, "BG %s", dbg_bg_id >= 0 ? dbg_name(dbg_bg_id) : "-");
    font_draw(line, -1, 4, y, Z_DEBUG, 120, 255, 140);
    y += 18;

    for (i = 0; i < CHAR_SLOTS; i++) {
        if (dbg_ch_id[i] < 0) continue;
        sprintf(line, "C%d %s %s%d,%d", i, dbg_name(dbg_ch_id[i]),
                ch_on[i] ? "" : "(off) ", ch_x[i], ch_y[i]);
        font_draw(line, -1, 4, y, Z_DEBUG, 255, 220, 120);
        y += 18;
    }

    sprintf(line, "gl%d pc%d %s%s", cur_chunk, (int)pc,
            st_name[state < 8 ? state : 0], seq_playing() ? " mus" : "");
    font_draw(line, -1, 4, y, Z_DEBUG, 160, 200, 255);
    y += 18;

    /* Сообщение сохранения: на консоли терминала нет, а знать результат
       надо сразу. Держится три секунды. */
    if (save_msg_left > 0) {
        font_draw(save_msg, -1, 4, y, Z_DEBUG, 255, 255, 140);
        y += 18;
    }

    /* Итог последнего ролика прямо на экране: TTY доступен не везде,
       а оверлей видно и в эмуляторе, и на живой консоли. */
    if (str_last_report()[0])
        font_draw(str_last_report(), -1, 4, y, Z_DEBUG, 255, 170, 170);

    /* Подложка добавляется ПОСЛЕДНЕЙ, хотя лежит снизу: в одном слое
       ordering table примитивы обрабатываются в обратном порядке
       добавления. Раньше она шла первой и потому рисовалась поверх
       текста, гася его наполовину. Два прохода — чтобы текст читался
       и на светлом фоне. */
    box_draw(0, 0, SCR_W, y + 22, 0, 0, 0, 0, Z_DEBUG);
    box_draw(0, 0, SCR_W, y + 22, 0, 0, 0, 0, Z_DEBUG);
}
#endif

/* Короткое сообщение (сохранение, загрузка, галерея, отчёт ролика).
   Раньше оно рисовалось только внутри отладочного оверлея: без Select
   игрок не видел ни «saved», ни «slot is empty», а в релизе — вообще
   ничего. Теперь показывается всегда; при включённом оверлее — в нём. */
static void note_draw(void)
{
    int h;

    if (save_msg_left <= 0) return;
#if !VN_RELEASE
    if (dbg_on) return;
#endif
    h = font_lines(save_msg) * 20 + 8;
    font_draw(save_msg, -1, 8, 8, Z_DEBUG, 255, 255, 140);
    /* Подложка добавляется после текста: в слое OT она окажется ниже. */
    box_draw(0, 4, SCR_W, h, 0, 0, 0, 0, Z_DEBUG);
    box_draw(0, 4, SCR_W, h, 0, 0, 0, 0, Z_DEBUG);
}

void vm_render(void)
{
    int i;

    /* Сплэш-экран: чёрный фон, текст по центру, плавное появление/уход.
       Рисуем отдельно от сцены — ни фона, ни персонажей тут нет. */
    if (state == S_SPLASH) {
        int lines = font_lines(splash_text);
        int w = font_strwidth(splash_text);
        int x = (SCR_W - w) / 2;
        int y = (SCR_H - lines * 20) / 2;    /* 20 = LINE_STEP из gfx.c */
        if (x < 0) x = 0;
        font_draw(splash_text, -1, x, y, Z_TEXT, 230, 230, 230);
        fade_draw(splash_fade);
#if !VN_RELEASE
    if (dbg_on) dbg_draw();
#endif
        return;
    }

    if (scene_frames > 0) {
        if (++scene_timer >= scene_delay) {      /* пора менять кадр */
            scene_timer = 0;
            if (++scene_cur >= scene_frames) scene_cur = 0;
        }
        if (scene_img[scene_cur].loaded)
            img_draw(&scene_img[scene_cur], bg_off_x, bg_off_y, Z_BG);
    } else if (bg_img.loaded) {
        img_draw(&bg_img, bg_off_x, bg_off_y, Z_BG);
    }

    for (i = 0; i < CHAR_SLOTS; i++)
        if (ch_on[i]) img_draw(&ch_img[i], ch_x[i], ch_y[i], Z_CHAR);

    /* Титульное меню поверх фона-титульника (его ставит опкод bg до menu). */
    if (state == S_MENU) {
        /* Нижний пункт всегда на одной высоте: при трёх пунктах список
           начинается со 150, четвёртый поднимает его, а не уводит за
           нижний край экрана. */
        int y = 228 - menu_count * 26;
        if (menu_title) {
            int tw = font_strwidth(menu_title);
            font_draw(menu_title, -1, (SCR_W - tw) / 2, 60, Z_TEXT, 255, 220, 140);
        }
        for (i = 0; i < menu_count; i++) {
            const char *it = menu_item[i];
            int iw = font_strwidth(it);
            int ix = (SCR_W - iw) / 2;
            int sel = (i == menu_sel);
            if (sel) {
                box_draw(ix - 14, y - 3, iw + 28, 22, 40, 50, 90, 0, Z_BOX);
                font_draw(">", -1, ix - 12, y, Z_TEXT, 255, 220, 120);
            }
            font_draw(it, -1, ix, y, Z_TEXT,
                      sel ? 255 : 150, sel ? 235 : 150, sel ? 120 : 150);
            y += 26;
        }
        fade_draw(fade_level);
        if (gal_on) gal_draw();
        if (menu_on) menu_draw();      /* загрузка с титульного экрана */
        note_draw();                   /* «gallery is empty» и т.п. */
#if !VN_RELEASE
        if (dbg_on) dbg_draw();
#endif
        return;
    }

    if (state == S_CHOICE) {
        int y = 70;
        for (i = 0; i < ch_count; i++) {
            box_draw(40, y - 4, 240, 24, 0, 0, 40, 0, Z_BOX);
            if (i == ch_sel)
                font_draw(">", -1, 26, y, Z_TEXT, 255, 200, 80);
            font_draw(ch_str[i], -1, 50, y, Z_TEXT,
                      (i == ch_sel) ? 255 : 140,
                      (i == ch_sel) ? 230 : 140,
                      (i == ch_sel) ?  90 : 140);
            y += 30;
        }
    } else if (box_visible && cur_text) {
        /* Окно рисуем двумя проходами: каждый смешивает 50/50, поэтому фон
           остаётся на четверть вместо половины. Так текст читается на любой
           картинке, и при этом не нужна тень под буквами — с мягким
           шрифтом она давала грязный ореол, а не подсветку контура. */
        box_draw(BOX_X, BOX_Y, BOX_W, BOX_H, 10, 12, 40, 0, Z_BOX);
        box_draw(BOX_X, BOX_Y, BOX_W, BOX_H, 10, 12, 40, 0, Z_BOX);

        if (cur_name) {
            /* Ширина плашки по самому имени: раньше было зашито 110 px,
               и длинное имя вылезало за край. */
            int nw = font_strwidth(cur_name) + 24;
            if (nw < 60) nw = 60;
            if (nw > BOX_W - 8) nw = BOX_W - 8;
            box_draw(BOX_X + 4, NAME_Y - 4, nw, 24, 20, 24, 70, 0, Z_BOX);
            box_draw(BOX_X + 4, NAME_Y - 4, nw, 24, 20, 24, 70, 0, Z_BOX);
            font_draw(cur_name, -1, NAME_X, NAME_Y, Z_TEXT, 255, 210, 120);
        }

        font_draw(cur_text, text_shown, TXT_X, TXT_Y, Z_TEXT, 240, 240, 240);

        /* Маркер «жду нажатия»: значок кнопки вместо квадрата.
           Сдвиг на 5 вместо 4 — смена каждые 32 кадра, ~0.9 Гц: спокойное
           мигание, которое не дёргает глаз при чтении. */
        blink++;
        if (state == S_TEXT && text_shown >= text_total && ((blink >> 5) & 1))
            font_draw_glyph(ICON_CROSS,
                            BOX_X + BOX_W - 22, BOX_Y + BOX_H - 22,
                            Z_TEXT, 230, 230, 245);
    }

    /* Эффекты идут ДО затемнения: иначе fade out гасил бы картинку, а
       вспышка и тонирование продолжали светиться поверх черноты. */
    fx_draw();

    if (gal_on) gal_draw();

    fade_draw(fade_level);
    if (menu_on) menu_draw();
    note_draw();
#if !VN_RELEASE
    if (dbg_on) dbg_draw();
#endif
}

/* ---- сохранения ---- */

u_int vm_chapter(void)   { return cur_chapter; }
u_int vm_playtime(void)  { return playtime; }
const u_char *vm_flags(void) { return flags; }

/* Можно ли сохраняться прямо сейчас: только на реплике, ждущей нажатия.
   Все прочие состояния — печать текста, затемнение, выбор — переходные,
   и восстанавливать их незачем. */
int vm_can_save(void)
{
    return cur_chapter != 0 && state == S_TEXT &&
           text_shown >= text_total;
}

/*
 * Применяет сохранение: прыгает на начало главы и восстанавливает флаги.
 * Состояние экрана НЕ восстанавливается — глава выстроит его сама.
 */
int vm_load_save(const SaveSlot *s)
{
    int chunk = save_chapter_chunk(s->chapter);
    int i;

    if (chunk < 0) return -1;

    seq_stop();
    voice_stop();

    for (i = 0; i < MAX_FLAGS && i < SAVE_FLAGS; i++)
        flags[i] = s->flags[i];

    /* Чистим сцену: иначе на новую главу перетекут спрайты и фон от
       прошлой, а она рассчитывает начать с пустого экрана. */
    bg_img.loaded = 0;
    scene_frames = 0;
    for (i = 0; i < CHAR_SLOTS; i++) ch_on[i] = 0;
    cur_text = NULL;
    cur_name = NULL;
    fade_level = 255;
    fade_target = 255;
    fx_reset();
    ch_count = 0;
    gal_on = 0;

    cur_chapter = s->chapter;
    playtime = s->playtime;

    vm_goto(((u_int)chunk << 24));      /* начало главы = начало чанка */
    state = S_RUN;
    return 0;
}
