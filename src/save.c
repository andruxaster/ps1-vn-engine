/* save.c -- сохранение и загрузка
 *
 * Сохраняемся только на границе главы. Это сознательное упрощение: глава
 * обязана выстроить сцену с нуля (компилятор это проверяет), поэтому для
 * восстановления достаточно номера главы и флагов сюжета. Не нужно
 * сериализовать ни фон, ни спрайты, ни указатели в пул строк.
 *
 * Глава обозначается ХЕШЕМ названия, а не порядковым номером: вставка новой
 * главы в середину сценария не ломает старые сохранения. Ломает только
 * переименование — и это честно видно игроку как «сохранение не подходит».
 */

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include "vn.h"

#define CHAP_MAX      64
#define CHAP_ENTRY    32
#define CHAP_NAME     24

typedef struct {
    u_int  hash;
    u_short chunk;
    char   title[CHAP_NAME];
} Chapter;

static Chapter chapters[CHAP_MAX];
static int     chapter_count;
static u_int   build_stamp;      /* отпечаток сборки, см. save_build_stamp */

static u_char  chap_raw[CHAP_MAX * CHAP_ENTRY + 8] __attribute__((aligned(4)));

static u_int rd32(const u_char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u_int)p[3] << 24);
}
static u_int rd16(const u_char *p) { return p[0] | (p[1] << 8); }

/*
 * Отпечаток сборки. Считается по таблице глав: если сценарий пересобран с
 * другими главами, старое сохранение отвергается, а не уводит игрока не
 * туда. Во время разработки это будет срабатывать часто, и это правильно.
 */
static u_int fnv(const u_char *p, int n)
{
    u_int h = 2166136261u;
    int i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

int save_load_chapters(const char *path)
{
    int len, i;

    chapter_count = 0;
    if (!cd_load_to(path, chap_raw, sizeof(chap_raw), &len)) {
        LOG("save: no %s\n", path);
        return -1;
    }
    if (len < 8 || memcmp(chap_raw, "CHAP", 4) != 0) {
        LOG("save: %s is not CHAP\n", path);
        return -1;
    }

    chapter_count = (int)rd16(chap_raw + 4);
    if (chapter_count > CHAP_MAX) {
        LOG("save: too many chapters (%d)\n", chapter_count);
        chapter_count = CHAP_MAX;
    }

    for (i = 0; i < chapter_count; i++) {
        const u_char *e = chap_raw + 8 + i * CHAP_ENTRY;
        chapters[i].hash  = rd32(e);
        chapters[i].chunk = (u_short)rd16(e + 4);
        memcpy(chapters[i].title, e + 8, CHAP_NAME);
        chapters[i].title[CHAP_NAME - 1] = 0;
    }

    build_stamp = fnv(chap_raw, 8 + chapter_count * CHAP_ENTRY);
    LOG("save: %d chapters, build %08x\n", chapter_count, build_stamp);
    return 0;
}

int save_chapter_count(void) { return chapter_count; }

const char *save_chapter_title(int i)
{
    if (i < 0 || i >= chapter_count) return "";
    return chapters[i].title;
}

/* Заполняет запись текущим состоянием. Возвращает 0, если сохраняться
   сейчас нельзя — например, глава ещё не начиналась. */
int save_make(SaveSlot *s, u_int chapter_hash, const u_char *flags,
              int nflags, u_int playtime)
{
    int i;

    if (!s || !chapter_hash)
        return 0;

    memset(s, 0, sizeof(*s));
    memcpy(s->magic, "VNSV", 4);
    s->version = SAVE_VERSION;
    s->build   = build_stamp;
    s->chapter = chapter_hash;
    s->playtime = playtime;

    if (nflags > SAVE_FLAGS) nflags = SAVE_FLAGS;
    for (i = 0; i < nflags; i++) s->flags[i] = flags[i];

    for (i = 0; i < chapter_count; i++) {
        if (chapters[i].hash == chapter_hash) {
            memcpy(s->title, chapters[i].title, CHAP_NAME);
            break;
        }
    }
    return 1;
}

/*
 * Проверяет запись перед применением. Отдельная функция, чтобы экран
 * сохранений мог показать причину, а не просто «не грузится».
 */
int save_check(const SaveSlot *s)
{
    int i;

    if (!s || memcmp(s->magic, "VNSV", 4) != 0)
        return SAVE_ERR_FORMAT;
    if (s->version != SAVE_VERSION)
        return SAVE_ERR_VERSION;
    if (s->build != build_stamp)
        return SAVE_ERR_BUILD;

    for (i = 0; i < chapter_count; i++)
        if (chapters[i].hash == s->chapter)
            return SAVE_OK;

    return SAVE_ERR_CHAPTER;
}

/* Номер чанка для главы из записи, или -1. */
int save_chapter_chunk(u_int hash)
{
    int i;
    for (i = 0; i < chapter_count; i++)
        if (chapters[i].hash == hash)
            return (int)chapters[i].chunk;
    return -1;
}
