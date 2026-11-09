/* gallery.c -- галерея открытых сцен
 *
 * Признак «сцена открыта» — один бит. Все биты лежат в отдельном кадре
 * файла на карте памяти, а не в слоте сохранения: галерея копится между
 * прохождениями, и новая игра не должна её стирать.
 *
 * Запись на карту происходит только тогда, когда бит ДЕЙСТВИТЕЛЬНО был
 * поднят впервые. Значит на каждую сцену приходится ровно одна запись за
 * всю жизнь файла, а не по записи на каждый показ.
 */

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include "vn.h"

#define GAL_MAX     256          /* столько сцен помещается в таблицу в ОЗУ */
#define GAL_ENTRY   52
#define GAL_TITLE   40
#define GAL_BITS    128          /* байт признаков = 1024 сцены */

typedef struct {
    u_short frame[4];        /* кадры: один для фона, до четырёх для сцены */
    u_char  count;
    u_char  delay;           /* кадров развёртки на кадр анимации, 0 = статика */
    char    title[GAL_TITLE];
} GalEntry;

static GalEntry gal[GAL_MAX];
static int      gal_count;
static u_char   gal_bits[GAL_BITS];
static int      gal_ready;
static int      gal_bits_read;   /* признаки с карты ещё не прочитаны */

/* Читает признаки с карты при первой надобности. Карты может не быть —
   тогда галерея просто пуста, и это не ошибка. */
static void gal_need_bits(void)
{
    if (gal_bits_read) return;
    gal_bits_read = 1;
    card_load_extra(0, card_file_name(), gal_bits, sizeof(gal_bits));
}

static u_char   gal_raw[GAL_MAX * GAL_ENTRY + 8] __attribute__((aligned(4)));

static u_int rd16(const u_char *p) { return p[0] | (p[1] << 8); }

int gal_load_table(const char *path)
{
    int len, i, k;

    gal_ready = 0;
    gal_count = 0;
    memset(gal_bits, 0, sizeof(gal_bits));

    if (!cd_load_to(path, gal_raw, sizeof(gal_raw), &len)) {
        LOG("gal: no %s\n", path);
        return -1;
    }
    if (len < 8 || memcmp(gal_raw, "GALL", 4) != 0) {
        LOG("gal: %s is not GALL\n", path);
        return -1;
    }

    gal_count = (int)rd16(gal_raw + 4);
    if (gal_count > GAL_MAX) {
        LOG("gal: too many scenes (%d), keeping %d\n", gal_count, GAL_MAX);
        gal_count = GAL_MAX;
    }

    for (i = 0; i < gal_count; i++) {
        const u_char *e = gal_raw + 8 + i * GAL_ENTRY;
        for (k = 0; k < 4; k++)
            gal[i].frame[k] = (u_short)rd16(e + k * 2);
        gal[i].count = e[8];
        gal[i].delay = e[9];
        if (gal[i].count > 4) gal[i].count = 4;
        memcpy(gal[i].title, e + 12, GAL_TITLE);
        gal[i].title[GAL_TITLE - 1] = 0;
    }

    /* Признаки НЕ читаем здесь: это обращение к карте, а значит подъём
       её драйвера при запуске. На некоторых платформах он подвешивает
       игру, и терять запуск ради галереи не стоит. Прочитаем при первом
       реальном обращении. */
    gal_bits_read = 0;

    gal_ready = 1;
    LOG("gal: %d scenes\n", gal_count);
    return 0;
}

int gal_count_total(void) { return gal_count; }

int gal_is_open(int i)
{
    if (i < 0 || i >= gal_count) return 0;
    gal_need_bits();
    return (gal_bits[i >> 3] >> (i & 7)) & 1;
}

int gal_count_open(void)
{
    int i, n = 0;
    for (i = 0; i < gal_count; i++) n += gal_is_open(i);
    return n;
}

const char *gal_title(int i)
{
    if (i < 0 || i >= gal_count) return "";
    return gal[i].title;
}

/* Кадры сцены: сколько их, какие и с какой задержкой меняются. */
int gal_frame_count(int i)
{
    if (i < 0 || i >= gal_count) return 0;
    return gal[i].count;
}

int gal_frame(int i, int k)
{
    if (i < 0 || i >= gal_count || k < 0 || k >= gal[i].count) return -1;
    return (int)gal[i].frame[k];
}

int gal_delay(int i)
{
    if (i < 0 || i >= gal_count) return 0;
    return gal[i].delay;
}

/*
 * Отмечает сцену открытой. Возвращает 1, если это случилось впервые —
 * тогда и только тогда идёт запись на карту.
 */
int gal_unlock(int i)
{
    if (!gal_ready || i < 0 || i >= gal_count) return 0;
    gal_need_bits();
    if (gal_is_open(i)) return 0;            /* уже было, писать незачем */

    gal_bits[i >> 3] |= (u_char)(1 << (i & 7));

    if (card_save_extra(0, card_file_name(), gal_bits, sizeof(gal_bits)) < 0) {
        /* Карты нет или не пишется. Бит держим в памяти: в этом сеансе
           галерея работает, просто не переживёт выключение. */
        LOG("gal: unlock %d not saved\n", i);
        return 1;
    }
    LOG("gal: unlocked %d (%s)\n", i, gal[i].title);
    return 1;
}
