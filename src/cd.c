/* cd.c — чтение файлов из ISO9660 на диске */

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <psxapi.h>
#include <psxcd.h>
#include "vn.h"

/* Статический буфер вместо malloc: на PS1 куча — лишний источник сюрпризов.
   128 КБ хватает на TIM 320x240 @ 8bpp (~77 КБ). */
#define TMP_SIZE (128 * 1024)
static u_long tmpbuf[TMP_SIZE / 4];

/*
 * Поднятие привода.
 *
 * Раньше здесь был голый CdInit() без проверки, и сразу за ним шло первое
 * чтение. В эмуляторе диск готов мгновенно, а живому приводу нужна
 * раскрутка; ODE и встроенный эмулятор PS3 сообщают о готовности не так,
 * как эмуляторы на ПК. Если попасть в это окно, самый первый CdSearchFile
 * не находит даже файл в корне — и всё выглядит так, будто диска нет.
 *
 * Поэтому: инициализируем с повторами и ждём, пока привод ответит на
 * простую команду.
 */
/*
 * Поднятие привода.
 *
 * Раньше здесь был голый CdInit() без проверки результата, и сразу за ним
 * шло первое чтение. В эмуляторе диск готов мгновенно, а живому приводу
 * нужна раскрутка — попадая в это окно, самый первый поиск не находил даже
 * файл в корне, и всё выглядело как отсутствие диска.
 *
 * Теперь инициализация с повторами, а при неудаче возвращается ТОЧНАЯ
 * причина от разборщика файловой системы: по ней сразу видно, диск ли не
 * раскрутился, не читается ли дескриптор тома или файловая система вовсе
 * не распознана.
 */
/*
 * Поднятие привода.
 *
 * Загрузчик BIOS читает с этого же диска SYSTEM.CNF и MAIN.EXE, то есть
 * файловая система на нём заведомо правильная. Но разборщик PSn00bSDK на
 * ODE и во встроенном эмуляторе PS3 отвечал CdlIsoInvalidFs — «это не
 * ISO9660». Так бывает, когда привод остался в состоянии, которого
 * библиотека не ожидает: не прочитано оглавление диска либо выставлен
 * режим выдачи сырых секторов по 2340 байт вместо обычных 2048, и тогда
 * дескриптор тома читается со сдвигом и не распознаётся.
 *
 * Поэтому приводим привод в известное состояние явно и по шагам.
 */
int cd_init(void)
{
    CdlFILE probe;
    u_char  mode;
    int     attempt, i, err;

    for (attempt = 0; attempt < 6; attempt++) {
        if (!CdInit()) {
            LOG("cd: CdInit failed, attempt %d\n", attempt + 1);
            for (i = 0; i < 30; i++) VSync(0);
            continue;
        }

        /* Разбудить и сбросить признак открытой крышки. */
        CdControlB(CdlNop, 0, 0);

        /* Прочитать оглавление диска: некоторые приводы и эмуляторы
           отдают файловую систему только после этого. */
        CdControlB(CdlReadTOC, 0, 0);
        for (i = 0; i < 30; i++) VSync(0);

        /* Явно обычные секторы по 2048 байт, двойная скорость.
           Ключевое здесь — снятый бит CdlModeSize: при нём привод отдаёт
           сырые 2340 байт, и разборщик файловой системы промахивается. */
        mode = CdlModeSpeed;
        CdControlB(CdlSetmode, &mode, 0);
        for (i = 0; i < 10; i++) VSync(0);

        /* Проверяем тем же путём, которым потом пойдут все загрузки. */
        if (CdSearchFile(&probe, "\\SYSTEM.CNF;1")) {
            char label[40];
            label[0] = 0;
            CdGetVolumeLabel(label);
            LOG("cd: ready on attempt %d, volume '%s', region %d\n",
                   attempt + 1, label, (int)CdGetRegion());
            return 0;
        }

        err = CdIsoError();
        LOG("cd: iso error %d, attempt %d\n", err, attempt + 1);
        for (i = 0; i < 20; i++) VSync(0);
    }

    err = CdIsoError();
    LOG("cd: giving up, iso error %d\n", err);
    return -(err + 1);
}

/*
 * Крючок «пока ждём диск». Чтение сектора с CD занимает сотни миллисекунд, и
 * всё это время главный цикл стоит: музыка замирает, ноты виснут. В эмуляторе
 * чтение почти мгновенное, поэтому проблема видна только на консоли.
 *
 * Теперь вместо глухой блокировки идёт опрос, а между опросами вызывается
 * этот крючок — ровно раз на кадр развёртки, чтобы секвенсор шёл в своём темпе.
 */
static void (*idle_hook)(void);

/* Почему сорвалось последнее чтение: «не найден» и «не прочитался» —
   разные диагнозы, а раньше оба давали одинаковую вспышку. */
static int cd_err;

int cd_last_error(void) { return cd_err; }

void cd_set_idle(void (*fn)(void))
{
    idle_hook = fn;
}

/*
 * Ожидание конца чтения.
 *
 * CD_POLL_READ = 1 — опрос с прокруткой секвенсора: музыка не замирает на
 * загрузках, но поведение зависит от того, что именно возвращает
 * неблокирующий опрос на конкретном приводе.
 *
 * CD_POLL_READ = 0 — блокирующее ожидание, как было изначально. Музыка на
 * время загрузки замолкает, зато чтение ведёт себя предсказуемо. Ставьте
 * ноль, если на железе появляется красный экран: значит опрос где-то
 * отдаёт значение, которое принимается за ошибку.
 */
#define CD_POLL_READ 1

/* Кадров развёртки на одно чтение. Фон в 75 КБ читается около 15 кадров,
   с учётом четырёх попыток берём с большим запасом. */
#define CD_WAIT_LIMIT 600

static int cd_wait_read(void)
{
#if CD_POLL_READ
    u_char status = 0;
    int frames = 0;
    int r;

    /*
     * Документация требует опрашивать привод РАЗ В КАДР, а не вплотную.
     * Плотный цикл не оставлял времени обработчику прерывания привода, и
     * чтение не завершалось никогда — игра вставала намертво. VSync(0)
     * здесь не только задаёт темп, но и отдаёт процессор прерываниям.
     */
    for (;;) {
        r = CdReadSync(1, &status);
        if (r <= 0)
            return r;                /* 0 готово, -1 ошибка, -2 отмена */

        if (idle_hook)
            idle_hook();             /* ровно один такт секвенсора на кадр */

        VSync(0);

        if (++frames > CD_WAIT_LIMIT) {
            LOG("cd: read timed out\n");
            return -1;
        }
    }
#else
    (void) idle_hook;
    return CdReadSync(0, 0);
#endif
}

/* ------------------------------------------------------------------ */
/* Общий файл картинок                                                  */

#define PAK_MAX_INDEX (8 * 1024)     /* хватает на 1023 картинки */

static CdlFILE pak_file;
static int     pak_ready;
static int     pak_count;
static u_char  pak_index[PAK_MAX_INDEX] __attribute__((aligned(4)));

static u_int pak_rd32(const u_char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u_int)p[3] << 24);
}

/*
 * Открывает общий файл и читает его указатель.
 *
 * Смысл в том, чтобы поиск по каталогу ISO9660 случился ОДИН раз. Дальше
 * привод встаёт сразу на нужный сектор: ни чтения каталога, ни лишнего
 * позиционирования. Для каждой картинки это экономит больше времени, чем
 * само её чтение.
 */
int cd_pak_open(const char *path)
{
    u_int idx_bytes, idx_sectors;

    pak_ready = 0;
    if (!CdSearchFile(&pak_file, (char *)path)) {
        LOG("cd: no %s, falling back to separate files\n", path);
        return -1;
    }

    /* Первый сектор: магия и число записей. */
    CdControl(CdlSetloc, (u_char *)&pak_file.pos, 0);
    CdRead(1, (u_long *)pak_index, CdlModeSpeed);
    if (cd_wait_read() < 0) {
        LOG("cd: %s index read failed\n", path);
        return -1;
    }
    if (memcmp(pak_index, "APAK", 4) != 0) {
        LOG("cd: %s is not APAK\n", path);
        return -1;
    }

    pak_count = pak_index[4] | (pak_index[5] << 8);
    idx_bytes = 8 + (u_int)pak_count * 8;
    idx_sectors = (idx_bytes + 2047) / 2048;

    if (idx_sectors * 2048 > PAK_MAX_INDEX) {
        LOG("cd: %s index too big (%d entries)\n", path, pak_count);
        return -1;
    }

    if (idx_sectors > 1) {            /* дочитываем остальные секторы */
        CdControl(CdlSetloc, (u_char *)&pak_file.pos, 0);
        CdRead(idx_sectors, (u_long *)pak_index, CdlModeSpeed);
        if (cd_wait_read() < 0) {
            LOG("cd: %s index read failed\n", path);
            return -1;
        }
    }

    pak_ready = 1;
    LOG("cd: %s ok, %d entries\n", path, pak_count);
    return 0;
}

int cd_pak_ready(void) { return pak_ready; }

/* Тот же временный буфер, что использует cd_load: держать второй такой же
   ради общего файла незачем. */
void *cd_scratch(void)      { return tmpbuf; }
int   cd_scratch_size(void) { return TMP_SIZE; }

/* Читает картинку по номеру прямо с её сектора. */
u_long *cd_pak_load(int index, void *dst, int cap, int *size)
{
    const u_char *e;
    CdlLOC loc;
    u_int sector, bytes;
    int attempt, base;

    if (!pak_ready || index < 0 || index >= pak_count)
        return NULL;

    e = pak_index + 8 + index * 8;
    sector = pak_rd32(e);
    bytes  = pak_rd32(e + 4);

    if ((int)((bytes + 2047) / 2048 * 2048) > cap) {
        LOG("cd: pak entry %d too big\n", index);
        return NULL;
    }

    /* Абсолютный сектор = начало файла плюс смещение из указателя. */
    base = CdPosToInt(&pak_file.pos);

    for (attempt = 0; attempt < 4; attempt++) {
        CdIntToPos(base + (int)sector, &loc);
        CdControl(CdlSetloc, (u_char *)&loc, 0);
        CdRead((int)((bytes + 2047) / 2048), (u_long *)dst, CdlModeSpeed);

        if (cd_wait_read() >= 0) {
            if (size) *size = (int)bytes;
            return (u_long *)dst;
        }
        LOG("cd: pak read error on %d, retry %d\n", index, attempt + 1);
        CdControl(CdlPause, 0, 0);
    }

    cd_err = CD_ERR_READ;
    return NULL;
}

u_long *cd_load_to(const char *path, void *dst, int cap, int *size)
{
    CdlFILE f;
    int sectors, attempt;

    cd_err = CD_ERR_NONE;

    /* Пока идёт реплика, привод занят потоком. Любое чтение оборвёт её на
       полуслове, поэтому глушим сами — иначе поймали бы это как редкий и
       необъяснимый обрыв звука. */
    voice_stop();
    voice_mode_dirty();      /* CdRead ниже выставит свой режим */

    if (!CdSearchFile(&f, (char *)path)) {
        LOG("cd: not found %s\n", path);
        cd_err = CD_ERR_NOTFOUND;
        return NULL;
    }
    sectors = (f.size + 2047) / 2048;
    /* CdRead всегда пишет целыми секторами по 2048 байт */
    if (sectors * 2048 > cap) {
        LOG("cd: %s too big (%d > %d)\n", path, sectors * 2048, cap);
        return NULL;
    }

    /* Раньше результат чтения не проверялся вовсе. На образе и на ODE
       чтение не сбоит, а вот записанная болванка на реальном приводе даёт
       ошибки регулярно: пыль, эксцентриситет, слабый лазер. Одна такая
       ошибка означала молча испорченные данные и зависание дальше по коду. */
    /* Чтение оставлено ровно таким, каким работало: CdRead с собственным
       циклом повторов. Меняется ТОЛЬКО способ ожидания — вместо глухой
       блокировки идёт опрос, чтобы между опросами крутить секвенсор.
       Переход на CdReadRetry пробовался вместе с этим и дал зависание;
       две правки сразу — плохая идея, поэтому она откачена отдельно. */
    for (attempt = 0; attempt < 4; attempt++) {
        CdControl(CdlSetloc, (u_char *)&f.pos, 0);
        CdRead(sectors, (u_long *)dst, CdlModeSpeed);

        if (cd_wait_read() >= 0) {
            if (size) *size = f.size;
            return (u_long *)dst;
        }

        LOG("cd: read error on %s, retry %d\n", path, attempt + 1);
        CdControl(CdlPause, 0, 0);
    }

    LOG("cd: giving up on %s\n", path);
    cd_err = CD_ERR_READ;
    return NULL;
}

u_long *cd_load(const char *path, int *size)
{
    return cd_load_to(path, tmpbuf, TMP_SIZE, size);
}
