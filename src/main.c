/* main.c — точка входа движка */

#include <sys/types.h>
#include <stdio.h>
#include <psxgpu.h>
#include <psxapi.h>
#include <psxetc.h>
#include <psxgte.h>
#include <psxspu.h>
#include "vn.h"

#define SCRIPT_MAX (96 * 1024)

static u_char script_data[SCRIPT_MAX] __attribute__((aligned(4)));

/* Запас на рост шрифта: при ячейке 16x18 атлас занимает ~25 КБ, а CdRead
   пишет целыми секторами по 2048. Раньше тут было 24 КБ, и увеличение
   ячейки с 16 до 18 px молча упирало загрузку в чёрный экран. */
static u_char font_tim[40 * 1024]     __attribute__((aligned(4)));
static u_char font_widths[2048];   /* CD читает секторами, нужен запас */

/*
 * Фатальная ошибка. На консоли терминала нет, поэтому вместо молчаливого
 * зависания мигаем экраном: число вспышек в серии = код ошибки. Так на
 * живом железе видно, ЧТО именно не загрузилось, без последовательного
 * порта и без шрифта (которого может ещё не быть).
 *
 *   1 вспышка  — FONT.TIM
 *   2 вспышки  — FONT.WID
 *   3 вспышки  — CH00.VNS (первая глава сценария)
 */

static void die(const char *msg, int code)
{
    LOG("fatal: %s (code %d)\n", msg, code);

    while (1) {
        int i, f;
        for (i = 0; i < code; i++) {
            for (f = 0; f < 10; f++) {          /* вспышка */
                box_draw(0, 0, SCR_W, SCR_H, 220, 40, 40, -1, Z_DEBUG);
                gfx_flip();
            }
            for (f = 0; f < 10; f++) gfx_flip();  /* пауза между вспышками */
        }
        for (f = 0; f < 60; f++) gfx_flip();      /* пауза между сериями */
    }
}

int main(void)
{
    gfx_init();
    {
        /* Число вспышек = 10 + причина от разборщика файловой системы:
           10 — ФС в порядке, файла нет      11 — сбой позиционирования
           12 — не читается дескриптор тома  13 — диск не ISO9660
           14 — открыта крышка
           Коды начинаются с десяти, чтобы не пересекаться с 1..7 —
           ошибками загрузки файлов ниже: раньше пять вспышек могли
           означать и «открыта крышка», и «FONT.WID не прочитался». */
        int r = cd_init();
        if (r < 0)
            die("CD init", 9 - r);
    }
    pad_init();

    /* Нужны для STR-видео: InitGeom() поднимает GTE (его использует
       VLC-декодер MDEC), SpuInit() — звук. Безвредны, если видео нет. */
    InitGeom();

    SpuInit();


    /* Секвенсор должен идти и во время чтения с диска: иначе на каждой
       загрузке фона музыка замирает, а ноты виснут. В эмуляторе чтение почти
       мгновенное, поэтому на консоли это заметно, а на ПК нет. */
    cd_set_idle(seq_tick);

    /* Пробный файл в корне диска: его читал загрузчик, значит он заведомо
       есть и находится в корневом каталоге. Если не найдётся даже он —
       оглавление диска недоступно нашей библиотеке целиком, и дело не в
       наших подкаталогах. */
    if (!cd_load_to("\\SYSTEM.CNF;1", font_widths, sizeof(font_widths), NULL))
        die("SYSTEM.CNF", 7);

    /* Общий файл картинок: один поиск по каталогу здесь избавляет от
       поиска при каждой загрузке фона или спрайта. Если файла нет,
       движок просто продолжит читать отдельные TIM. */
    cd_pak_open("\\DATA\\ART.PAK;1");

    /* Озвучка не обязательна: без неё движок просто молчит. */
    voice_open("\\DATA\\VOICE.XAS;1", "\\DATA\\VOICE.IDX;1");

    /* Таблица глав нужна сохранениям. Без неё игра работает, но
       сохраняться будет некуда. */
    save_load_chapters("\\DATA\\SCR\\CHAPS.BIN;1");


    /* Карта памяти: драйвер поднимается один раз, заголовок с иконкой
       читается с диска. Без карты игра работает, просто не сохраняется. */
    card_init();
    card_load_header("\\DATA\\SYS\\SAVEHDR.BIN;1");

    /* Галерея читает признаки открытых сцен С КАРТЫ, поэтому идёт после
       заголовка: имя файла на карте берётся именно оттуда. Наоборот —
       и галерея всегда начиналась бы пустой. */
    gal_load_table("\\DATA\\SCR\\GAL.BIN;1");


    if (!cd_load_to("\\DATA\\SYS\\FONT.TIM;1", font_tim, sizeof(font_tim), NULL))
        die("FONT.TIM", cd_last_error() == CD_ERR_READ ? 4 : 1);
    if (!cd_load_to("\\DATA\\SYS\\FONT.WID;1", font_widths, sizeof(font_widths), NULL))
        die("FONT.WID", cd_last_error() == CD_ERR_READ ? 5 : 2);
    font_init((u_long *)font_tim, font_widths);

    /* Банк сэмплов не обязателен: без него движок работает молча. */
    if (seq_load_bank("\\DATA\\BGM\\BANK.VAB;1") < 0)
        LOG("seq: no BANK.VAB, music disabled\n");

    /* Сценарий нарезан на главы CH00.VNS, CH01.VNS, ... — VM подтягивает
       их сама по мере надобности, в памяти живёт ровно одна. */
    if (vm_start(script_data, SCRIPT_MAX) < 0)
        die("CH00.VNS", cd_last_error() == CD_ERR_READ ? 6 : 3);

    while (1) {
        voice_tick();
        seq_tick();              /* 60 Гц — привязан к кадру */
        vm_update();
        vm_render();

        if (vm_finished())
            font_draw("КОНЕЦ", -1, 130, 110, Z_TEXT, 255, 255, 255);

        gfx_flip();
    }
    return 0;
}
