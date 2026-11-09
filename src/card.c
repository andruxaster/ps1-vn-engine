/* card.c -- сохранение на карту памяти
 *
 * Используется низкоуровневый набор _card_* из psxapi.h: он объявлен явно,
 * в отличие от флагов файловых вызовов BIOS, значения которых пришлось бы
 * угадывать. Взамен файловую систему карты приходится вести самим — она
 * простая и хорошо документирована.
 *
 * Устройство карты:
 *   кадр 128 байт, всего 1024 кадра = 128 КБ
 *   кадр 0        заголовок каталога
 *   кадры 1..15   по записи на каждый блок
 *   блок N        кадры N*64 .. N*64+63, то есть 8 КБ
 *
 * Наш файл занимает один блок. Первые два кадра блока — служебный
 * заголовок с названием и иконкой (его показывает менеджер BIOS), дальше
 * наши слоты сохранений.
 */

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <psxapi.h>
#include "vn.h"

#define FRAME       128
#define BLOCK_FRAMES 64
#define DIR_FREE    0xA0
#define DIR_FIRST   0x51
#define LINK_LAST   0xFFFF

/* Служебный заголовок с иконкой готовит tools/mksaveicon.py.
   Полезных данных здесь 256 байт, но буфер обязан быть кратен сектору:
   CdRead пишет целыми секторами по 2048, и на буфере «точно по размеру
   файла» чтение отказывается. */
static u_char hdr_icon[2048] __attribute__((aligned(4)));
static int    hdr_ready;

static u_char frame[FRAME] __attribute__((aligned(4)));

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
 * Готовит драйвер карты. Порядок важен: сначала общая инициализация,
 * затем запуск, затем _bu_init. Аргумент InitCARD — оставить ли включённым
 * опрос геймпада; нам он нужен, поэтому единица.
 */
/*
 * Подъём драйвера карты — по требованию, а не при запуске.
 *
 * Раньше он поднимался в main() вместе со всем остальным. Но игрок может
 * ни разу не сохраниться, а на некоторых платформах (встроенные эмуляторы
 * приставок) подъём драйвера при старте — известное место зависания:
 * карта эмулируется иначе, и вызовы BIOS ведут себя не так.
 *
 * Теперь драйвер поднимается при первом реальном обращении к карте. Если
 * платформа его не тянет, зависнет сохранение, а не запуск игры.
 */
static int card_driver_up;

static void card_ensure_driver(void)
{
    if (card_driver_up) return;

    InitCARD(1);
    StartCARD();
    _bu_init();
    card_driver_up = 1;
    LOG("card: driver up\n");
}

void card_init(void)
{
    /* Оставлено для совместимости вызова из main(): само поднятие
       отложено до первого обращения. */
    card_driver_up = 0;
}

/*
 * Проверяет, что карта в гнезде и отвечает. Каждая операция карты
 * асинхронная, поэтому после запроса надо дождаться завершения.
 */
/* Последняя причина отказа — чтобы вызывающий мог показать её человеку,
   а не общее «не получилось». */
static int last_err;

int card_last_error(void) { return last_err; }

int card_present(int chan)
{
    int st;

    card_ensure_driver();

    _card_load(chan);
    if (_card_wait(chan) < 0) {
        last_err = CARD_ERR_NOCARD;
        return 0;
    }

    /* _card_info запрашивает сведения о карте. Часть последовательностей
       без него не считает карту готовой. */
    _card_info(chan);
    if (_card_wait(chan) < 0) {
        last_err = CARD_ERR_NOCARD;
        return 0;
    }

    st = _card_status(chan);
    LOG("card: status %d\n", st);
    if (st < 0) {
        last_err = CARD_ERR_STATUS;
        return 0;
    }
    return 1;
}

/* Контрольная сумма каталога и наших кадров — простое исключающее ИЛИ. */
static u_char xor_sum(const u_char *p, int n)
{
    u_char s = 0;
    int i;
    for (i = 0; i < n; i++) s ^= p[i];
    return s;
}

/*
 * Записывает наши слоты в блок карты и заводит на него запись каталога.
 *
 * Пишем всегда в блок 1: игра одна, файл один, а внутри него уже несколько
 * слотов. Так не приходится разбирать занятость блоков и искать свободный.
 */
/*
 * Готовит каталог карты, если он невалиден.
 *
 * Форматируем ТОЛЬКО когда заголовок не читается как "MC": на исправной
 * карте с чужими сохранениями ничего не трогаем. Иначе первая же запись
 * стёрла бы соседние игры.
 */
static int card_format_if_needed(int chan)
{
    int i;

    _card_read(chan, 0, frame);
    if (_card_wait(chan) >= 0 && frame[0] == 'M' && frame[1] == 'C')
        return 0;                      /* каталог в порядке */

    LOG("card: directory invalid, formatting\n");

    /* Кадр 0 — заголовок каталога. */
    memset(frame, 0, FRAME);
    frame[0] = 'M';
    frame[1] = 'C';
    frame[FRAME - 1] = xor_sum(frame, FRAME - 1);
    _card_write(chan, 0, frame);
    if (_card_wait(chan) < 0) return -1;

    /* Кадры 1..15 — записи блоков, все свободны. */
    for (i = 1; i <= 15; i++) {
        memset(frame, 0, FRAME);
        frame[0] = DIR_FREE;
        frame[8] = (u_char)(LINK_LAST & 0xFF);
        frame[9] = (u_char)(LINK_LAST >> 8);
        frame[FRAME - 1] = xor_sum(frame, FRAME - 1);
        _card_write(chan, i, frame);
        if (_card_wait(chan) < 0) return -1;
    }

    /* Кадры 16..35 — список сбойных секторов, записи пустые. */
    for (i = 16; i <= 35; i++) {
        memset(frame, 0xFF, FRAME);
        _card_write(chan, i, frame);
        if (_card_wait(chan) < 0) return -1;
    }

    return 0;
}

/*
 * Флаги для open(). В psxapi.h их нет: это значения BIOS, а не SDK.
 * Взяты из документации; если запись не пойдёт, править надо здесь.
 * При создании файла число блоков передаётся в старших 16 битах.
 */
#define O_RDONLY   0x0001
#define O_WRONLY   0x0002
#define O_CREAT    0x0200

/* Слотов в файле. Каждый занимает кадр в 128 байт, запись сохранения —
   108, так что помещается с запасом. Файл на карте всегда один: слоты
   лежат внутри него, поэтому сохранение в один не трогает остальные. */
#define CARD_SLOTS 4
/* Запись сохранения переросла один кадр: 256 флагов плюс служебная часть
   дают 300 байт. Считаем размер слота в кадрах, а не полагаем его равным
   одному — иначе слоты налезли бы друг на друга. */
#define SLOT_FRAMES 3
/* Ещё один кадр в конце — под глобальные данные, которые не относятся к
   слотам: признаки открытых сцен галереи. Они копятся между
   прохождениями, поэтому лежат отдельно от сохранений. */
#define EXTRA_FRAME (2 + CARD_SLOTS * SLOT_FRAMES)
#define CARD_BUF    (FRAME * (EXTRA_FRAME + 1))

static u_char file_buf[CARD_BUF] __attribute__((aligned(4)));

/*
 * Запись через файловый интерфейс BIOS.
 *
 * Низкоуровневые _card_* оказались неработоспособны: они асинхронные и
 * требуют отдельно открытых событий, чего мы не делали. Файловые вызовы
 * держат всю эту машинерию внутри — и именно ими пользуется большинство
 * игр.
 */
static void card_path(char *out, int chan, const char *name)
{
    strcpy(out, chan ? "bu10:" : "bu00:");
    strcat(out, name);
}

/* Читает файл целиком во внутренний буфер. 0 — прочитан, -1 — нет файла. */
static int card_fetch(int chan, const char *name)
{
    char path[32];
    int  fd, n;

    card_ensure_driver();

    card_path(path, chan, name);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, file_buf, CARD_BUF);
    close(fd);
    return (n >= FRAME * 2 && memcmp(file_buf, "SC", 2) == 0) ? 0 : -1;
}

static int card_write_file(int chan, const char *name,
                           const void *data, int len)
{
    char path[32];
    int  fd, n;

    card_ensure_driver();

    if (len > CARD_BUF - FRAME * 2) {
        last_err = CARD_ERR_TOOBIG;
        return -1;
    }

    card_path(path, chan, name);
    memcpy(file_buf, hdr_icon, FRAME * 2);
    memcpy(file_buf + FRAME * 2, data, len);

    /* Сначала пробуем открыть существующий файл: пересоздание стёрло бы
       остальные слоты, которые лежат в том же файле. */
    fd = open(path, O_WRONLY);
    if (fd < 0)
        fd = open(path, O_CREAT | O_WRONLY | (1 << 16));   /* один блок */

    if (fd < 0) {
        LOG("card: open('%s') failed\n", path);
        last_err = CARD_ERR_WRITE;
        return -1;
    }

    n = write(fd, file_buf, CARD_BUF);
    close(fd);

    if (n != CARD_BUF) {
        LOG("card: wrote %d of %d\n", n, CARD_BUF);
        last_err = CARD_ERR_VERIFY;
        return -1;
    }

    LOG("card: saved '%s' via file api\n", path);
    return 0;
}

static int card_read_file(int chan, void *data, int len)
{
    char path[32];
    int  fd, n;

    card_path(path, chan, card_name);

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    n = read(fd, file_buf, CARD_BUF);
    close(fd);

    if (n < FRAME * 2 + len || memcmp(file_buf, "SC", 2) != 0)
        return -1;

    memcpy(data, file_buf + FRAME * 2, len);
    return 0;
}

/*
 * Сохраняет запись в слот, не трогая остальные: файл сначала читается
 * целиком, меняется один кадр, потом пишется обратно.
 */
int card_save_slot(int chan, const char *name, int slot,
                   const void *rec, int len)
{
    if (slot < 0 || slot >= CARD_SLOTS || len > FRAME * SLOT_FRAMES)
        return -1;

    if (card_fetch(chan, name) < 0)
        memset(file_buf, 0, CARD_BUF);      /* файла ещё нет */

    memset(file_buf + FRAME * (2 + slot * SLOT_FRAMES), 0,
           FRAME * SLOT_FRAMES);
    memcpy(file_buf + FRAME * (2 + slot * SLOT_FRAMES), rec, len);

    return card_write(chan, name, file_buf + FRAME * 2,
                      CARD_BUF - FRAME * 2);
}

/* Читает слот. 0 — прочитан, -1 — пусто или файла нет. */
int card_load_slot(int chan, const char *name, int slot,
                   void *rec, int len)
{
    const u_char *p;

    if (slot < 0 || slot >= CARD_SLOTS || len > FRAME * SLOT_FRAMES)
        return -1;
    if (card_fetch(chan, name) < 0)
        return -1;

    p = file_buf + FRAME * (2 + slot * SLOT_FRAMES);
    if (memcmp(p, "VNSV", 4) != 0)          /* слот не занят */
        return -1;

    memcpy(rec, p, len);
    return 0;
}

int card_slot_count(void) { return CARD_SLOTS; }

/* Глобальный кадр: читается и пишется целиком, слоты при этом не
   страдают — файл сначала вычитывается, меняется один кадр, пишется
   обратно. */
int card_save_extra(int chan, const char *name, const void *data, int len)
{
    if (len > FRAME) return -1;

    if (card_fetch(chan, name) < 0)
        memset(file_buf, 0, CARD_BUF);

    memset(file_buf + FRAME * EXTRA_FRAME, 0, FRAME);
    memcpy(file_buf + FRAME * EXTRA_FRAME, data, len);

    return card_write(chan, name, file_buf + FRAME * 2,
                      CARD_BUF - FRAME * 2);
}

int card_load_extra(int chan, const char *name, void *data, int len)
{
    if (len > FRAME) return -1;
    if (card_fetch(chan, name) < 0) return -1;
    memcpy(data, file_buf + FRAME * EXTRA_FRAME, len);
    return 0;
}

int card_write(int chan, const char *name, const void *data, int len)
{
    int i, frames;

    last_err = CARD_ERR_NONE;

    if (!hdr_ready) {
        last_err = CARD_ERR_NOHDR;
        return -1;
    }
    if (len > (BLOCK_FRAMES - 2) * FRAME) {
        last_err = CARD_ERR_TOOBIG;
        return -1;
    }
    /* Основной путь — файловый интерфейс. Низкоуровневый оставлен ниже
       на случай, если где-то окажется наоборот. */
    if (card_write_file(chan, name, data, len) == 0)
        return 0;

    LOG("card: file api failed, trying low level\n");

    if (!card_present(chan))
        return -1;

    if (card_format_if_needed(chan) < 0) {
        last_err = CARD_ERR_DIR;
        return -1;
    }

    /* Два служебных кадра: заголовок с названием и сама иконка. */
    for (i = 0; i < 2; i++) {
        memcpy(frame, hdr_icon + i * FRAME, FRAME);
        /* Возврат _card_write в заголовке не описан, поэтому судим только
           по _card_wait — а само значение печатаем для сведения. */
        LOG("card: hdr frame %d -> %d\n", i,
               _card_write(chan, 1 * BLOCK_FRAMES + i, frame));
        if (_card_wait(chan) < 0) {
            LOG("card: wait failed at header frame %d\n", i);
            last_err = CARD_ERR_WRITE;
            return -1;
        }
    }

    /* Данные, по кадру за раз. */
    frames = (len + FRAME - 1) / FRAME;
    for (i = 0; i < frames; i++) {
        int chunk = len - i * FRAME;
        if (chunk > FRAME) chunk = FRAME;

        memset(frame, 0, FRAME);
        memcpy(frame, (const u_char *)data + i * FRAME, chunk);
        _card_write(chan, 1 * BLOCK_FRAMES + 2 + i, frame);
        if (_card_wait(chan) < 0) {
            LOG("card: wait failed at data frame %d\n", i);
            last_err = CARD_ERR_WRITE;
            return -1;
        }
    }

    /* Запись каталога для блока 1: без неё файл на карте не появится. */
    memset(frame, 0, FRAME);
    frame[0] = DIR_FIRST;                      /* первый и единственный блок */
    frame[4] = (u_char)(8192 & 0xFF);          /* размер файла — весь блок */
    frame[5] = (u_char)((8192 >> 8) & 0xFF);
    frame[8] = (u_char)(LINK_LAST & 0xFF);     /* продолжения нет */
    frame[9] = (u_char)(LINK_LAST >> 8);

    for (i = 0; i < 19 && name[i]; i++)
        frame[10 + i] = (u_char)name[i];

    frame[FRAME - 1] = xor_sum(frame, FRAME - 1);

    _card_write(chan, 1, frame);
    if (_card_wait(chan) < 0) {
        LOG("card: directory write failed\n");
        last_err = CARD_ERR_DIR;
        return -1;
    }

    /* Проверяем чтением: _card_wait сообщает лишь о завершении операции,
       а не о том, что данные действительно легли. Дешёвая проверка, зато
       «записалось» перестаёт быть обещанием. */
    memset(frame, 0, FRAME);
    _card_read(chan, 1 * BLOCK_FRAMES, frame);
    if (_card_wait(chan) < 0 || memcmp(frame, "SC", 2) != 0) {
        LOG("card: verify failed, data did not land\n");
        last_err = CARD_ERR_VERIFY;
        return -1;
    }

    LOG("card: saved '%s', %d frames\n", name, frames + 2);
    return 0;
}

/* Читает наши данные из блока 1. Служебные кадры пропускаются. */
int card_read(int chan, void *data, int len)
{
    int i, frames;

    if (card_read_file(chan, data, len) == 0)
        return 0;

    if (!card_present(chan))
        return -1;

    /* Проверяем, что в блоке лежит именно наш файл. */
    _card_read(chan, 1 * BLOCK_FRAMES, frame);
    if (_card_wait(chan) < 0)
        return -1;
    if (memcmp(frame, "SC", 2) != 0) {
        LOG("card: block 1 is not a save file\n");
        return -1;
    }

    frames = (len + FRAME - 1) / FRAME;
    for (i = 0; i < frames; i++) {
        int chunk = len - i * FRAME;
        if (chunk > FRAME) chunk = FRAME;

        _card_read(chan, 1 * BLOCK_FRAMES + 2 + i, frame);
        if (_card_wait(chan) < 0) {
            LOG("card: read failed at frame %d\n", i);
            return -1;
        }
        memcpy((u_char *)data + i * FRAME, frame, chunk);
    }
    return 0;
}
