#!/usr/bin/env python3
"""Готовит служебный заголовок сохранения для карты памяти.

  python3 tools/mksaveicon.py save_icon.png data/SYS/SAVEHDR.BIN --title "MY GAME"

Первые два кадра файла сохранения занимает не наши данные, а служебная
структура, которую читает менеджер карт памяти в BIOS: без неё сохранение
выглядит там мусором.

  кадр 0 (128 байт):
     0  "SC"
     2  флаг числа кадров иконки (0x11 = один)
     3  сколько блоков занимает файл
     4  название, 64 байта
    68  резерв, 28 байт
    96  палитра: 16 цветов по 15 бит
  кадр 1 (128 байт):
     иконка 16x16, по 4 бита на пиксель

Про название: менеджер BIOS ожидает японскую кодировку, и кириллицы там
нет. Поэтому в самой карте имя пишется латиницей — внутри игры, на своём
экране сохранений, никаких ограничений нет.
"""

FRAME_EXTRA = 32     # кадр под имя файла на карте

import argparse
import os
import struct

from PIL import Image


def rgb15(r, g, b):
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)


def main():
    ap = argparse.ArgumentParser(description="иконка и заголовок сохранения")
    ap.add_argument("source")
    ap.add_argument("out")
    ap.add_argument("--title", default="SAVE")
    ap.add_argument("--blocks", type=int, default=1)
    ap.add_argument("--savefile", default="BASLUS-VN000SAVE",
                    help="имя файла на карте, попадает в третий кадр")
    args = ap.parse_args()

    im = Image.open(args.source).convert("RGBA")

    # Прозрачность на карте кодируется цветом 0x0000, поэтому подложим
    # чёрный там, где пусто, и зарезервируем под это первый цвет палитры.
    flat = Image.new("RGBA", im.size, (0, 0, 0, 255))
    flat.paste(im, (0, 0), im)
    small = flat.convert("RGB").resize((16, 16), Image.LANCZOS)

    q = small.quantize(colors=16, method=Image.MEDIANCUT)
    pal = (q.getpalette() or [])[:48]
    pal += [0] * (48 - len(pal))

    hdr = bytearray(128)
    hdr[0:2] = b"SC"
    hdr[2] = 0x11                      # один кадр иконки
    hdr[3] = args.blocks & 0xFF

    title = args.title.encode("ascii", "replace")[:63]
    hdr[4:4 + len(title)] = title

    for i in range(16):
        struct.pack_into("<H", hdr, 96 + i * 2,
                         rgb15(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]))

    # Иконка: два пикселя в байте, младший полубайт — левый пиксель.
    px = q.load()
    icon = bytearray(128)
    for y in range(16):
        for x in range(0, 16, 2):
            icon[y * 8 + x // 2] = (px[x, y] & 15) | ((px[x + 1, y] & 15) << 4)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    # Третий кадр — имя файла на карте. Держим его здесь, а не в коде
    # движка: тогда смена игры не требует правки исходников, и имя не
    # приходится дублировать по нескольким местам.
    name = bytearray(FRAME_EXTRA)
    fn = args.savefile.encode("ascii", "replace")[:19]
    name[0:len(fn)] = fn

    open(args.out, "wb").write(bytes(hdr) + bytes(icon) + bytes(name))

    print(f"{args.out}: заголовок 128 б + иконка 128 б + имя 32 б, "
          f"название '{title.decode()}', файл '{fn.decode()}'")


if __name__ == "__main__":
    main()
