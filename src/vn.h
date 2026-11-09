#ifndef VN_H
#define VN_H

#include <sys/types.h>
#include <psxgpu.h>

#define SCR_W       320
#define SCR_H       240
#define OT_LEN      16
#define PACKET_LEN  20480      /* буфер примитивов на кадр */

/* Слои ordering table. 0 = ближе к зрителю, OT_LEN-1 = дальше всех. */
#define Z_DEBUG  0      /* отладочный оверлей — поверх всего, включая затемнение */
#define Z_FADE   1
#define Z_TEXT   2
#define Z_BOX    3
#define Z_CHAR   6
#define Z_BG    12

/*
 * Релизная сборка: cmake -DVN_RELEASE=ON
 *
 * Убирает печать в терминал (на консоли она идёт в последовательный порт
 * и стоит около 4 мс на строку), отладочный оверлей по Select и цветные
 * отметки шагов запуска.
 *
 * Мигание кодом при фатальной ошибке ОСТАЁТСЯ: без него игрок увидит
 * просто чёрный экран, а это худшее из возможного.
 */
#ifndef VN_RELEASE
#define VN_RELEASE 0
#endif

#if VN_RELEASE
#define LOG(...)   ((void)0)
#else
#define LOG(...)   printf(__VA_ARGS__)
#endif

#define CHAR_SLOTS  4
#define SCENE_FRAMES 4
/* Флагов 256 — это потолок формата: номер флага занимает один байт в
   опкодах set и if, больше туда не поместится. Значение флага тоже байт,
   то есть 0..255, а не только «да/нет». */
#define MAX_FLAGS   256
#define MAX_CHOICES 4

/* Слоты VRAM. Раньше координаты запекались внутрь TIM на этапе сборки, и
   спрайт был намертво привязан к одному месту — из-за этого не работали
   несколько выражений лица у персонажа. Теперь место выбирается при
   загрузке, и любая картинка кладётся в любой слот. */
enum {
    SLOT_BG = 0,
    SLOT_CHAR0, SLOT_CHAR1, SLOT_CHAR2,
    SLOT_SCENE0, SLOT_SCENE1, SLOT_SCENE2, SLOT_SCENE3,
    /* Четвёртый персонаж занял область, которая раньше держалась про запас
       под плавную смену выражения. Ту затею откатили, а место так и
       числилось зарезервированным — шесть процентов видеопамяти впустую.
       ВАЖНО: слот идёт последним, а не рядом с CHAR0..CHAR2, потому что
       код адресует их как SLOT_CHAR0 + номер. Поэтому ниже есть таблица
       перевода. */
    SLOT_CHAR3,
    SLOT_COUNT
};

/* Номер слота персонажа -> слот VRAM. Прямая арифметика SLOT_CHAR0 + i
   работает только для первых трёх: четвёртый лежит отдельно. */
extern const int char_vram_slot[CHAR_SLOTS];

typedef struct {
    u_short tpx, tpy;   /* база texture page в координатах VRAM */
    u_short u, v;       /* смещение внутри страницы, в текселях */
    u_short w, h;       /* размер в текселях */
    u_short clut;       /* упакованный getClut() */
    u_char  mode;       /* 0=4bpp, 1=8bpp, 2=16bpp */
    u_char  loaded;
} VN_IMG;

/* ---- gfx.c ---- */
void      gfx_init(void);
void      gfx_flip(void);
/* Сдвиг показываемой области: тряска всего экрана без затрат видеопамяти */
void      gfx_shake(int x, int y);
void      gfx_show_raw(int buf);
void      gfx_clear_all(void);
void      gfx_restore_display(void);
u_long   *gfx_ot(void);
void     *gfx_alloc(int size);

int       img_load(VN_IMG *img, const u_long *tim);
/* Загрузка с явным выбором места в VRAM. Координаты из самого TIM при этом
   игнорируются — так один и тот же спрайт можно положить в любой слот. */
int       img_load_to(VN_IMG *img, const u_long *tim, int slot);
/* Отрисовка с управлением яркостью и режимом смешивания.
   abr < 0 — как обычно; 0 — пополам с фоном; 1 — сложение. */
void      img_draw_ex(const VN_IMG *img, int x, int y, int z,
                      int r, int g, int b, int abr);
void      img_draw(const VN_IMG *img, int x, int y, int z);
/* abr: -1 = непрозрачно, 0 = B/2+F/2, 1 = B+F, 2 = B-F, 3 = B+F/4 */
void      box_draw(int x, int y, int w, int h, int r, int g, int b, int abr, int z);
void      fade_draw(int level);          /* 0..255, 255 = чёрный экран */

int       font_init(const u_long *tim, const u_char *widths);
void      font_draw(const char *s, int maxchars, int x, int y, int z,
                    int r, int g, int b);
int       font_count(const char *s);     /* число кодпоинтов в строке */
int       font_strwidth(const char *s);  /* ширина самой длинной строки, px */
int       font_lines(const char *s);     /* число строк (\n + 1) */

/* Значки в атласе шрифта — идут после символов. Индекс обязан совпадать
   с ICON_CROSS в tools/build_data.py. */
#define ICON_CROSS 161
void      font_draw_glyph(int gi, int x, int y, int z, int r, int g, int b);

/* ---- cd.c ---- */
int       cd_init(void);
u_long   *cd_load(const char *path, int *size);   /* во временный буфер */
u_long   *cd_load_to(const char *path, void *dst, int cap, int *size);
/* Функция, которую надо крутить, пока идёт чтение с диска (иначе музыка
   замирает на каждой загрузке). Вызывается раз на кадр развёртки. */
void      cd_set_idle(void (*fn)(void));

/* Причина последнего сбоя чтения */
#define CD_ERR_NONE     0
#define CD_ERR_NOTFOUND 1
#define CD_ERR_READ     2
int       cd_last_error(void);

/* Общий файл картинок: один поиск по каталогу при старте, дальше чтение
   сразу с нужного сектора. */
/* ---- card.c: карта памяти ---- */
void      card_init(void);
int       card_load_header(const char *path);
/* Слоты внутри одного файла: сохранение в один не трогает остальные. */
int       card_save_slot(int chan, const char *name, int slot,
                         const void *rec, int len);
int       card_load_slot(int chan, const char *name, int slot,
                         void *rec, int len);
int       card_slot_count(void);
/* Глобальные данные, не привязанные к слоту: признаки галереи */
int       card_save_extra(int chan, const char *name, const void *data, int len);
int       card_load_extra(int chan, const char *name, void *data, int len);

/* Причина последнего отказа карты */
enum { CARD_ERR_NONE = 0, CARD_ERR_NOHDR, CARD_ERR_NOSPACE, CARD_ERR_READ,
       CARD_ERR_WRITE, CARD_ERR_TOOBIG, CARD_ERR_VERIFY };
int       card_last_error(void);

/* ---- gallery.c: галерея открытых сцен ---- */
/* Имя файла на карте берётся из заголовка сохранения (SAVEHDR.BIN), а он
   собирается по game.txt. В коде его больше нет: движок универсален, и
   смена игры не должна требовать правки исходников. */
const char *card_file_name(void);

int         gal_load_table(const char *path);
int         gal_count_total(void);
int         gal_count_open(void);
int         gal_is_open(int i);
const char *gal_title(int i);
int         gal_frame_count(int i);
int         gal_frame(int i, int k);
int         gal_delay(int i);
int         gal_unlock(int i);

/* ---- save.c: сохранения ---- */
/* Версия поднята: запись выросла с 64 флагов до 256, и старые сохранения
   стали несовместимы. Проверка версии отвергнет их с внятной причиной,
   а не прочитает как мусор. */
#define SAVE_VERSION 2
#define SAVE_FLAGS   256
#define SAVE_TITLE   24

/* Запись сохранения. Снимка экрана в ней нет намеренно: глава выстраивает
   сцену сама, поэтому хватает её номера и флагов сюжета. */
typedef struct {
    char    magic[4];          /* "VNSV" */
    u_short version;
    u_short pad;
    u_int   build;             /* отпечаток сборки: чужое не подойдёт */
    u_int   chapter;           /* хеш названия главы */
    u_int   playtime;          /* кадров развёртки с начала игры */
    char    title[SAVE_TITLE]; /* для списка сохранений */
    u_char  flags[SAVE_FLAGS];
} SaveSlot;

enum { SAVE_OK = 0, SAVE_ERR_FORMAT, SAVE_ERR_VERSION,
       SAVE_ERR_BUILD, SAVE_ERR_CHAPTER };

int         save_load_chapters(const char *path);
int         save_chapter_count(void);
const char *save_chapter_title(int i);
int         save_make(SaveSlot *s, u_int chapter_hash, const u_char *flags,
                      int nflags, u_int playtime);
int         save_check(const SaveSlot *s);
int         save_chapter_chunk(u_int hash);

/* Со стороны интерпретатора */
u_int          vm_chapter(void);
u_int          vm_playtime(void);
const u_char  *vm_flags(void);
int            vm_can_save(void);
int            vm_load_save(const SaveSlot *s);

/* ---- voice.c: озвучка из общего файла XA ---- */
int       voice_open(const char *xas, const char *idx);
int       voice_play(int index);
void      voice_stop(void);
void      voice_tick(void);        /* раз в кадр из главного цикла */
int       voice_playing(void);
/* Сообщить, что режим привода сбит чужим чтением: следующая реплика
   выставит свой заново. */
void      voice_mode_dirty(void);

int       cd_pak_open(const char *path);
int       cd_pak_ready(void);
u_long   *cd_pak_load(int index, void *dst, int cap, int *size);
void     *cd_scratch(void);       /* общий временный буфер чтения */
int       cd_scratch_size(void);

/* ---- pad.c ---- */
void      pad_init(void);
u_int     pad_now(void);
u_int     pad_pressed(void);

/* ---- str.c ---- */
/* nframes берётся из скомпилированного скрипта: build_data.py считает кадры,
   просканировав .str. Возвращает 1, если ролик пропустили. */
int       str_play(const char *path, int nframes, int ticks, int skippable);
/* Итог последнего ролика: показать в отладочном оверлее, а не только в TTY */
const char *str_last_report(void);

/* ---- seq.c: музыка (Psy-Q SEQ/VAB через PSn00bSDK SPU) ---- */
int       seq_load_bank(const char *path);
int       seq_play(const char *path, int loop);
void      seq_stop(void);
void      seq_tick(void);          /* раз в кадр из главного цикла */
int       seq_playing(void);
void      seq_set_volume(int v);   /* 0..127 */
void      seq_fade_to(int v, int frames);   /* трек не останавливает */
void      seq_fade_out(int frames);          /* в тишину и стоп */
void      seq_fade_in(int frames);           /* из тишины к рабочей */
int       seq_base_volume(void);             /* заданная bgm volume */
/* Короткий звук из банка: играет на голосах, свободных от музыки. */
void      seq_play_se(int vag, int vol);
/* Рабочая громкость музыки: к ней возвращается fade in ... music.
   Значение совпадает с начальным master_vol в src/seq.c. */
#define SEQ_VOL_NORMAL 90

/* ---- vm.c ---- */
/* buf/cap — буфер, куда VM грузит главы CH00.VNS, CH01.VNS, ... */
int       vm_start(u_char *buf, int cap);
void      vm_update(void);
void      vm_render(void);
int       vm_finished(void);

#endif
