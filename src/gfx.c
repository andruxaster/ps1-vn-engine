/* gfx.c — вывод графики: двойной буфер, TIM-спрайты, битмап-шрифт */

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>
#include <psxetc.h>
#include "vn.h"

typedef struct {
    DISPENV disp;
    DRAWENV draw;
    u_long  ot[OT_LEN];
    char    pri[PACKET_LEN];
} DBUF;

static DBUF  db[2];
static int   db_act;
static char *nextpri;

/* -------------------------------------------------------------- */

void gfx_init(void)
{
    int i;

    ResetGraph(0);

    /* Буфер 0: рисуем в (0,0), показываем (0,240). Буфер 1 — наоборот. */
    SetDefDrawEnv(&db[0].draw, 0,     0, SCR_W, SCR_H);
    SetDefDispEnv(&db[0].disp, 0, SCR_H, SCR_W, SCR_H);
    SetDefDrawEnv(&db[1].draw, 0, SCR_H, SCR_W, SCR_H);
    SetDefDispEnv(&db[1].disp, 0,     0, SCR_W, SCR_H);

    for (i = 0; i < 2; i++) {
        setRGB0(&db[i].draw, 0, 0, 0);
        db[i].draw.isbg = 1;    /* заливать буфер перед отрисовкой */
        db[i].draw.dtd  = 1;    /* дизеринг: сглаживает градиенты в 15 бит */
        ClearOTagR(db[i].ot, OT_LEN);
    }

    db_act  = 0;
    nextpri = db[0].pri;

    PutDrawEnv(&db[0].draw);
    PutDispEnv(&db[0].disp);
    SetDispMask(1);
}

u_long *gfx_ot(void) { return db[db_act].ot; }

/* Залить оба кадровых буфера чёрным. В PSn00bSDK нет ClearImage() из PsyQ,
   но у наших DRAWENV стоит isbg=1 — при PutDrawEnv буфер сам заливается
   фоновым цветом (он у нас чёрный), так что отдельная функция не нужна. */
void gfx_clear_all(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        PutDrawEnv(&db[i].draw);
        DrawSync(0);
    }
    PutDrawEnv(&db[db_act].draw);   /* вернуть текущий контекст рисования */
}

/* Показать кадровый буфер напрямую, минуя OT — нужно проигрывателю STR,
   который пишет декодированные кадры прямо в VRAM через LoadImage. */
void gfx_show_raw(int buf)
{
    static DISPENV d;
    SetDefDispEnv(&d, 0, buf ? SCR_H : 0, SCR_W, SCR_H);
    PutDispEnv(&d);
    SetDispMask(1);
}

/* Вернуть обычный режим двойной буферизации после видео */
void gfx_restore_display(void)
{
    db_act  = 0;
    nextpri = db[0].pri;
    ClearOTagR(db[0].ot, OT_LEN);
    ClearOTagR(db[1].ot, OT_LEN);
    PutDrawEnv(&db[0].draw);
    PutDispEnv(&db[0].disp);
    SetDispMask(1);
}

void *gfx_alloc(int size)
{
    char *p = nextpri;
    size = (size + 3) & ~3;
    /* Переполнение: примитив просто не рисуется. Раньше здесь отдавалось
       начало буфера, то есть поверх уже связанных в OT примитивов, — это
       рвало список, и GPU уходил по мусорному указателю. */
    if ((p + size) > (db[db_act].pri + PACKET_LEN))
        return NULL;
    nextpri += size;
    return p;
}

/* Смещение показываемой области. Двигается ВЕСЬ экран разом — фон,
   персонажи и текст, — потому что меняется не отрисовка, а то, какой
   участок видеопамяти выводится. Стоит ноль: это два поля в структуре. */
static int shake_x, shake_y;

void gfx_shake(int x, int y)
{
    shake_x = x;
    shake_y = y;
}

void gfx_flip(void)
{
    DISPENV d;

    DrawSync(0);
    VSync(0);

    d = db[db_act].disp;
    if (shake_x | shake_y) {
        d.disp.x += shake_x;
        d.disp.y += shake_y;
    }
    PutDispEnv(&d);
    PutDrawEnv(&db[db_act].draw);
    SetDispMask(1);
    DrawOTag(db[db_act].ot + OT_LEN - 1);

    db_act ^= 1;
    nextpri = db[db_act].pri;
    ClearOTagR(db[db_act].ot, OT_LEN);
}

/* -------------------------------------------------------------- */

static int px_per_hword(int mode)
{
    return (mode == 0) ? 4 : (mode == 1) ? 2 : 1;
}

/* Таблица мест в VRAM. Порядок обязан совпадать с enum в vn.h.
   Кадровые буферы занимают x0-319 y0-479, палитры лежат в y480+, поэтому
   всё остальное распределено по свободным областям. */
static const struct { u_short x, y, cx, cy; } vram_slot[SLOT_COUNT] = {
    { 320,   0,   0, 480 },   /* фон          320x240 8bpp */
    { 512,   0,   0, 481 },   /* персонаж 0   256x256 8bpp */
    { 640,   0,   0, 482 },   /* персонаж 1 */
    { 768,   0,   0, 483 },   /* персонаж 2 */
    { 384, 256,   0, 496 },   /* кадр сцены 0 */
    { 544, 256,   0, 497 },   /* кадр сцены 1 */
    { 704, 256,   0, 498 },   /* кадр сцены 2 */
    { 864, 256,   0, 499 },   /* кадр сцены 3 */
    { 896,   0,   0, 485 },   /* персонаж 3 */
};

static int img_load_common(VN_IMG *img, const u_long *tim, int slot)
{
    TIM_IMAGE t;
    int ppw;

    img->loaded = 0;
    if (GetTimInfo((u_long *)tim, &t) < 0)
        return -1;

    /* Место назначения выбирается здесь, а не на этапе сборки: координаты,
       записанные в самом TIM, при slot >= 0 просто перекрываются. Благодаря
       этому один и тот же спрайт можно положить в любой слот — именно так
       работают несколько выражений лица у одного персонажа. */
    if (slot >= 0 && slot < SLOT_COUNT) {
        t.prect->x = vram_slot[slot].x;
        t.prect->y = vram_slot[slot].y;
        if (t.mode & 0x8) {
            t.crect->x = vram_slot[slot].cx;
            t.crect->y = vram_slot[slot].cy;
        }
        if ((t.prect->x + t.prect->w) > 1024 ||
            (t.prect->y + t.prect->h) > 512) {
            LOG("img: slot %d too small for %dx%d\n",
                   slot, t.prect->w, t.prect->h);
            return -1;
        }
    }

    LoadImage(t.prect, t.paddr);
    DrawSync(0);
    if (t.mode & 0x8) {                 /* есть CLUT */
        LoadImage(t.crect, t.caddr);
        DrawSync(0);
    }

    img->mode = t.mode & 3;
    ppw = px_per_hword(img->mode);

    /* База texture page кратна 64 полусловам по X и 256 строкам по Y */
    img->tpx  = t.prect->x & ~0x3F;
    img->tpy  = t.prect->y & ~0xFF;
    img->u    = (t.prect->x - img->tpx) * ppw;
    img->v    = (t.prect->y - img->tpy);
    img->w    = t.prect->w * ppw;       /* prect->w задан в полусловах */
    img->h    = t.prect->h;
    img->clut = (t.mode & 0x8) ? getClut(t.crect->x, t.crect->y) : 0;
    img->loaded = 1;
    return 0;
}

/* Координаты берутся из самого TIM — так грузится шрифт */
int img_load(VN_IMG *img, const u_long *tim)
{
    return img_load_common(img, tim, -1);
}

int img_load_to(VN_IMG *img, const u_long *tim, int slot)
{
    return img_load_common(img, tim, slot);
}

/*
 * U/V у GPU 8-битные, поэтому картинку шире/выше 256 текселей приходится
 * резать на куски и для каждого выставлять свою texture page.
 */
/* Первые три слота идут подряд, четвёртый занял бывшую запасную область
   в конце таблицы. */
const int char_vram_slot[CHAR_SLOTS] = {
    SLOT_CHAR0, SLOT_CHAR1, SLOT_CHAR2, SLOT_CHAR3
};

void img_draw(const VN_IMG *img, int x, int y, int z)
{
    img_draw_ex(img, x, y, z, 128, 128, 128, 0);
}

void img_draw_ex(const VN_IMG *img, int x, int y, int z,
                 int r, int g, int b, int abr)
{
    int ppw, sx, sy;

    if (!img || !img->loaded) return;
    ppw = px_per_hword(img->mode);

    for (sy = 0; sy < img->h; ) {
        int v  = img->v + sy;
        int py = img->tpy + (v & ~255);
        int vv = v & 255;
        int hh = 256 - vv;
        if (hh > img->h - sy) hh = img->h - sy;

        for (sx = 0; sx < img->w; ) {
            int u  = img->u + sx;
            int px = img->tpx + (u >> 8) * (256 / ppw);
            int uu = u & 255;
            int ww = 256 - uu;
            SPRT     *s;
            DR_TPAGE *tp;

            if (ww > img->w - sx) ww = img->w - sx;

            s  = (SPRT *)gfx_alloc(sizeof(SPRT));
            tp = (DR_TPAGE *)gfx_alloc(sizeof(DR_TPAGE));
            if (!s || !tp) return;
            setSprt(s);
            setXY0(s, x + sx, y + sy);
            setWH (s, ww, hh);
            setUV0(s, uu, vv);
            s->clut = img->clut;
            setRGB0(s, r, g, b);                /* 128 = нейтральная яркость */
            /* Полупрозрачность включаем всегда: смешиваются только те
               тексели, у которых в палитре стоит бит STP. Для обычных
               картинок таких нет, и они рисуются как раньше — зато у
               спрайтов персонажей мягкий край волос не превращается
               в гребёнку. */
            if (abr >= 0) setSemiTrans(s, 1);
            addPrim(gfx_ot() + z, s);

            /* tpage добавляем ПОСЛЕ спрайта: addPrim кладёт в начало списка,
               значит GPU обработает его раньше самого спрайта */
            setDrawTPage(tp, 0, 1,
                         getTPage(img->mode, abr >= 0 ? abr : 0, px, py));
            addPrim(gfx_ot() + z, tp);

            sx += ww;
        }
        sy += hh;
    }
}

void box_draw(int x, int y, int w, int h, int r, int g, int b, int abr, int z)
{
    TILE *t = (TILE *)gfx_alloc(sizeof(TILE));
    if (!t) return;
    setTile(t);
    setXY0(t, x, y);
    setWH (t, w, h);
    setRGB0(t, r, g, b);

    if (abr >= 0) {
        DR_TPAGE *tp;
        setSemiTrans(t, 1);
        addPrim(gfx_ot() + z, t);
        tp = (DR_TPAGE *)gfx_alloc(sizeof(DR_TPAGE));
        if (!tp) return;
        setDrawTPage(tp, 0, 1, getTPage(0, abr, 0, 0));
        addPrim(gfx_ot() + z, tp);
    } else {
        addPrim(gfx_ot() + z, t);
    }
}

/* Затемнение: режим B-F вычитает из кадра ровный серый — честный fade to black */
void fade_draw(int level)
{
    if (level <= 0) return;
    if (level > 255) level = 255;
    box_draw(0, 0, SCR_W, SCR_H, level, level, level, 2, Z_FADE);
}

/* ---------------------- шрифт ---------------------------------- */

#define GLYPH_W      16
/* Высота ячейки обязана вмещать ascent+descent шрифта целиком.
   При 16 px хвосты букв (р, у, ф, y, q) вылезали в ячейку этажом ниже,
   и над Б, В, К, Н, Ё появлялась «случайная» точка. Значение должно
   совпадать с GLYPH_H в tools/build_data.py. */
#define GLYPH_H      18
#define GLYPH_COLS   16
#define LINE_STEP    20
/* 161 символ + значок кнопки в последней ячейке (см. ICON_CROSS
   в tools/build_data.py — значения обязаны совпадать) */
#define FONT_GLYPHS  162

static VN_IMG font_img;
static u_char font_w[FONT_GLYPHS];

int font_init(const u_long *tim, const u_char *widths)
{
    if (img_load(&font_img, tim) < 0) return -1;
    memcpy(font_w, widths, FONT_GLYPHS);
    return 0;
}

/* UTF-8 -> кодпоинт */
static const char *u8next(const char *p, int *cp)
{
    unsigned char c = (unsigned char)*p++;
    int v, n;

    if      (c < 0x80) { v = c;        n = 0; }
    else if (c < 0xE0) { v = c & 0x1F; n = 1; }
    else if (c < 0xF0) { v = c & 0x0F; n = 2; }
    else               { v = c & 0x07; n = 3; }

    while (n-- > 0 && (*p & 0xC0) == 0x80)
        v = (v << 6) | (*p++ & 0x3F);

    *cp = v;
    return p;
}

/* Раскладка атласа: ASCII 0x20..0x7E, затем А-я, затем Ё и ё */
static int cp2glyph(int cp)
{
    if (cp >= 0x20 && cp <= 0x7E)   return cp - 0x20;            /*   0..94  */
    if (cp >= 0x410 && cp <= 0x44F) return 95 + (cp - 0x410);    /*  95..158 */
    if (cp == 0x401)                return 159;                  /* Ё */
    if (cp == 0x451)                return 160;                  /* ё */
    return 0;                                                    /* пробел */
}

int font_count(const char *s)
{
    int n = 0, cp;
    while (*s) { s = u8next(s, &cp); n++; }
    return n;
}

/* Ширина самой длинной строки (до \n) в пикселях — для центрирования. */
int font_strwidth(const char *s)
{
    int cur = 0, max = 0, cp;
    while (*s) {
        s = u8next(s, &cp);
        if (cp == '\n') { if (cur > max) max = cur; cur = 0; continue; }
        cur += font_w[cp2glyph(cp)];
    }
    if (cur > max) max = cur;
    return max;
}

/* Число строк (\n + 1). */
int font_lines(const char *s)
{
    int n = 1, cp;
    while (*s) { s = u8next(s, &cp); if (cp == '\n') n++; }
    return n;
}

/* Нарисовать одну ячейку атласа по её индексу. Нужно для значков
   (кнопка «крестик» и т.п.), у которых нет кодпоинта. */
void font_draw_glyph(int gi, int x, int y, int z, int r, int g, int b)
{
    SPRT     *sp;
    DR_TPAGE *tp;

    if (gi < 0 || gi >= FONT_GLYPHS || !font_img.loaded) return;

    sp = (SPRT *)gfx_alloc(sizeof(SPRT));
    tp = (DR_TPAGE *)gfx_alloc(sizeof(DR_TPAGE));
    if (!sp || !tp) return;
    setSprt(sp);
    setXY0(sp, x, y);
    setWH (sp, GLYPH_W, GLYPH_H);
    setUV0(sp, font_img.u + (gi % GLYPH_COLS) * GLYPH_W,
               font_img.v + (gi / GLYPH_COLS) * GLYPH_H);
    sp->clut = font_img.clut;
    setRGB0(sp, r, g, b);
    addPrim(gfx_ot() + z, sp);

    setDrawTPage(tp, 0, 1,
                 getTPage(font_img.mode, 0, font_img.tpx, font_img.tpy));
    addPrim(gfx_ot() + z, tp);
}

void font_draw(const char *s, int maxchars, int x, int y, int z,
               int r, int g, int b)
{
    const char *p = s;
    int cx = x, cy = y, drawn = 0, cp, gi;

    while (*p && (maxchars < 0 || drawn < maxchars)) {
        p = u8next(p, &cp);
        drawn++;

        if (cp == '\n') { cx = x; cy += LINE_STEP; continue; }

        gi = cp2glyph(cp);
        if (cp != ' ') {
            SPRT *sp = (SPRT *)gfx_alloc(sizeof(SPRT));
            if (!sp) break;
            setSprt(sp);
            setXY0(sp, cx, cy);
            setWH (sp, GLYPH_W, GLYPH_H);
            setUV0(sp, font_img.u + (gi % GLYPH_COLS) * GLYPH_W,
                       font_img.v + (gi / GLYPH_COLS) * GLYPH_H);
            sp->clut = font_img.clut;
            setRGB0(sp, r, g, b);
            addPrim(gfx_ot() + z, sp);
        }
        cx += font_w[gi];
    }

    /* Весь атлас влезает в одну texture page — хватает одного DR_TPAGE */
    if (drawn) {
        DR_TPAGE *tp = (DR_TPAGE *)gfx_alloc(sizeof(DR_TPAGE));
        if (!tp) return;
        setDrawTPage(tp, 0, 1,
                     getTPage(font_img.mode, 0, font_img.tpx, font_img.tpy));
        addPrim(gfx_ot() + z, tp);
    }
}
