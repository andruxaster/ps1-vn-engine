/* card.c -- сохранение на карту памяти
 *
 * Работает через файловые вызовы BIOS (open/read/write по пути bu00:имя).
 * Низкоуровневые _card_* асинхронные и требуют отдельно открытых событий
 * BIOS: в DuckStation запись через них «проходила», но данные не
 * появлялись. Файловые вызовы держат эту машинерию внутри — ими же
 * пользуется большинство игр.
 *
 * Файл игры на карте один и занимает один блок (8 КБ):
 *
 *   кадры 0..1        служебный заголовок: название и иконка (их
 *                     показывает менеджер карты в BIOS)
 *   кадры 2..13       слоты сохранений, по SLOT_FRAMES кадров на слот
 *   кадр  14          глобальные данные: признаки открытых сцен галереи
 *
 * Кадр — 128 байт; BIOS читает и пишет файлы карты кратно кадру.
 */

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <psxapi.h>
#include "vn.h"

#define FRAME       128

/* Служебный заголовок с иконкой готовит tools/mksaveicon.py.
   Полезных данных здесь 384 байта, но буфер обязан быть кратен сектору:
   CdRead пишет целыми секторами по 2048. */
static u_char hdr_icon[2048] __attribute__((aligned(4)));
static int    hdr_ready;

/* Имя файла на карте лежит в третьем кадре заголовка, а не в коде: так
   смена игры не требует правки исходников. Пустая строка означает, что
   заголовок ещё не загружен. */
static char card_name[24];

const char *card_file_name(void) { return card_name; }

int card_load_header(const char *path)
{
    int len, i;

    hdr_ready = 0;
    card_name[0] = 0;
    if (!cd_load_to(path, hdr_icon, sizeof(hdr_icon), &len)) {
        LOG("card: no %s\n", path);
        return -1;
    }
    if (len < FRAME * 2 || memcmp(hdr_icon, "SC", 2) != 0) {
        LOG("card: %s is not a save header\n", path);
        return -1;
    }
    /* Третий кадр — имя файла. Если его нет (старый заголовок), берём
       запасное, чтобы игра всё же сохранялась. */
    if (len >= FRAME * 2 + 1 && hdr_icon[FRAME * 2]) {
        for (i = 0; i < (int)sizeof(card_name) - 1; i++) {
            card_name[i] = (char)hdr_icon[FRAME * 2 + i];
            if (!card_name[i]) break;
        }
        card_name[sizeof(card_name) - 1] = 0;
    } else {
        strcpy(card_name, "BASLUS-VN000SAVE");
        LOG("card: header has no file name, using default\n");
    }

    hdr_ready = 1;
    LOG("card: save file '%s'\n", card_name);
    return 0;
}

/*
 * Подъём драйвера карты — по требованию, а не при запуске.
 *
 * Игрок может ни разу не сохраниться, а на некоторых платформах
 * (встроенные эмуляторы приставок) подъём драйвера при старте — известное
 * место зависания. Если платформа его не тянет, зависнет сохранение, а не
 * запуск игры.
 */
static int card_driver_up;

static void card_ensure_driver(void)
{
    if (card_driver_up) return;

    InitCARD(1);      /* 1 — опрос геймпада остаётся включённым */
    StartCARD();
    _bu_init();
    card_driver_up = 1;
    LOG("card: driver up\n");
}

void card_init(void)
{
    /* Само поднятие отложено до первого обращения. */
    card_driver_up = 0;
}

/* Последняя причина отказа — чтобы экран сохранений показал её человеку,
   а не общее «не получилось». */
static int last_err;

int card_last_error(void) { return last_err; }

/*
 * Флаги для open(). В psxapi.h их нет: это значения BIOS, а не SDK.
 * При создании файла число блоков передаётся в старших 16 битах.
 */
#define O_RDONLY   0x0001
#define O_WRONLY   0x0002
#define O_CREAT    0x0200

#define CARD_SLOTS  4
/* Запись сохранения — 300 байт (256 флагов плюс служебная часть), то есть
   три кадра на слот. */
#define SLOT_FRAMES 3
#define EXTRA_FRAME (2 + CARD_SLOTS * SLOT_FRAMES)
#define CARD_BUF    (FRAME * (EXTRA_FRAME + 1))

static u_char file_buf[CARD_BUF]   __attribute__((aligned(4)));
static u_char verify_buf[CARD_BUF] __attribute__((aligned(4)));

static void card_path(char *out, int chan, const char *name)
{
    strcpy(out, chan ? "bu10:" : "bu00:");
    strcat(out, name);
}

/* Результаты чтения файла целиком */
#define FETCH_OK      0
#define FETCH_NOFILE -1      /* открыть не удалось */
#define FETCH_BAD    -2      /* файл открылся, но не прочитался или чужой */

static int card_read_into(const char *path, u_char *dst)
{
    int fd, n;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return FETCH_NOFILE;
    n = read(fd, dst, CARD_BUF);
    close(fd);
    return (n >= FRAME * 2 && memcmp(dst, "SC", 2) == 0) ? FETCH_OK : FETCH_BAD;
}

/*
 * Читает файл во внутренний буфер. Открытие пробуется дважды: BIOS
 * отвечает отказом на первое обращение после смены карты, и это не
 * значит, что файла нет.
 */
static int card_fetch(int chan, const char *name)
{
    char path[32];
    int  r;

    card_ensure_driver();
    card_path(path, chan, name);

    r = card_read_into(path, file_buf);
    if (r == FETCH_NOFILE)
        r = card_read_into(path, file_buf);
    return r;
}

/*
 * Пишет буфер целиком и перечитывает для проверки.
 *
 * Файл всегда сначала прочитан (или создаётся впервые), поэтому запись
 * одного слота не трогает остальные слоты и признаки галереи.
 */
static int card_commit(int chan, const char *name, int fd)
{
    char path[32];
    int  n;

    card_path(path, chan, name);
    memcpy(file_buf, hdr_icon, FRAME * 2);    /* иконка всегда свежая */

    if (fd < 0)
        fd = open(path, O_WRONLY);
    if (fd < 0) {
        LOG("card: open('%s') for write failed\n", path);
        last_err = CARD_ERR_WRITE;
        return -1;
    }
    n = write(fd, file_buf, CARD_BUF);
    close(fd);

    if (n != CARD_BUF) {
        LOG("card: wrote %d of %d\n", n, CARD_BUF);
        last_err = CARD_ERR_WRITE;
        return -1;
    }

    /* Проверяем чтением: «записалось» перестаёт быть обещанием. */
    if (card_read_into(path, verify_buf) != FETCH_OK ||
        memcmp(verify_buf, file_buf, CARD_BUF) != 0) {
        LOG("card: verify failed\n");
        last_err = CARD_ERR_VERIFY;
        return -1;
    }

    LOG("card: saved '%s'\n", path);
    return 0;
}

/*
 * Готовит буфер к изменению: читает существующий файл или создаёт новый.
 * Возвращает дескриптор только что созданного файла, -1 если файл уже
 * был (писать будем, открыв заново), -2 при ошибке.
 *
 * Главное здесь — НИКОГДА не принимать сбой чтения за «файла нет». Раньше
 * любой сбой обнулял буфер, и запись одного слота стирала остальные
 * сохранения вместе с галереей.
 */
static int card_prepare(int chan, const char *name)
{
    char path[32];
    int  r, fd;

    last_err = CARD_ERR_NONE;
    if (!hdr_ready) {
        last_err = CARD_ERR_NOHDR;
        return -2;
    }

    r = card_fetch(chan, name);
    if (r == FETCH_OK)
        return -1;
    if (r == FETCH_BAD) {
        last_err = CARD_ERR_READ;
        return -2;
    }

    /* Открыть не удалось. Создание существующего файла BIOS отвергает,
       так что успех здесь и есть доказательство, что файла не было. */
    card_path(path, chan, name);
    fd = open(path, O_CREAT | O_WRONLY | (1 << 16));   /* один блок */
    if (fd >= 0) {
        memset(file_buf, 0, CARD_BUF);
        return fd;
    }

    /* Не создался: либо карты нет / она полна, либо файл всё-таки есть.
       Последняя попытка прочитать — и только потом сдаёмся. */
    if (card_fetch(chan, name) == FETCH_OK)
        return -1;

    last_err = CARD_ERR_NOSPACE;
    return -2;
}

int card_save_slot(int chan, const char *name, int slot,
                   const void *rec, int len)
{
    int fd;

    if (slot < 0 || slot >= CARD_SLOTS || len > FRAME * SLOT_FRAMES) {
        last_err = CARD_ERR_TOOBIG;
        return -1;
    }

    fd = card_prepare(chan, name);
    if (fd == -2)
        return -1;

    memset(file_buf + FRAME * (2 + slot * SLOT_FRAMES), 0,
           FRAME * SLOT_FRAMES);
    memcpy(file_buf + FRAME * (2 + slot * SLOT_FRAMES), rec, len);

    return card_commit(chan, name, fd);
}

/* Читает слот. 0 — прочитан, -1 — пусто или файла нет. */
int card_load_slot(int chan, const char *name, int slot,
                   void *rec, int len)
{
    const u_char *p;

    if (slot < 0 || slot >= CARD_SLOTS || len > FRAME * SLOT_FRAMES)
        return -1;
    if (card_fetch(chan, name) != FETCH_OK)
        return -1;

    p = file_buf + FRAME * (2 + slot * SLOT_FRAMES);
    if (memcmp(p, "VNSV", 4) != 0)          /* слот не занят */
        return -1;

    memcpy(rec, p, len);
    return 0;
}

int card_slot_count(void) { return CARD_SLOTS; }

/* Глобальный кадр: меняется один он, слоты не страдают. */
int card_save_extra(int chan, const char *name, const void *data, int len)
{
    int fd;

    if (len > FRAME) {
        last_err = CARD_ERR_TOOBIG;
        return -1;
    }

    fd = card_prepare(chan, name);
    if (fd == -2)
        return -1;

    memset(file_buf + FRAME * EXTRA_FRAME, 0, FRAME);
    memcpy(file_buf + FRAME * EXTRA_FRAME, data, len);

    return card_commit(chan, name, fd);
}

int card_load_extra(int chan, const char *name, void *data, int len)
{
    if (len > FRAME) return -1;
    if (card_fetch(chan, name) != FETCH_OK) return -1;
    memcpy(data, file_buf + FRAME * EXTRA_FRAME, len);
    return 0;
}
