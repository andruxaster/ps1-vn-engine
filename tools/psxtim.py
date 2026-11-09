"""Конвертер PNG -> TIM (формат текстур PlayStation 1).

Цвет в VRAM: 16 бит, BGR555 + бит STP.
  bit15 = STP, 14..10 = B, 9..5 = G, 4..0 = R
В палитровых режимах значение 0x0000 означает «прозрачный пиксель»,
поэтому индекс 0 всегда резервируем под прозрачность.
"""

import struct
from PIL import Image


def rgb555(r, g, b, stp=0):
    return (stp << 15) | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3)


def _pack_clut(palette, n):
    out = []
    for i in range(n):
        if i < len(palette):
            r, g, b, a = palette[i]
        else:
            r = g = b = a = 0
        if a < 128 or i == 0:
            out.append(0x0000)          # прозрачный
        else:
            v = rgb555(r, g, b)
            if v == 0:
                v = rgb555(8, 8, 8)     # чистый чёрный «спасаем» от прозрачности
            out.append(v)
    return out


def bleed_edges(im, passes=4):
    """«Растекание» цвета в прозрачные области.

    В обычном PNG у полностью прозрачных пикселей цвет случайный (часто
    чёрный или белый). При уменьшении картинки он подмешивается в соседние
    полупрозрачные пиксели, и по краю волос появляется тёмная или светлая
    кайма. Лечится тем, что перед ресайзом прозрачные пиксели заливаются
    цветом ближайших непрозрачных.
    """
    im = im.convert("RGBA")
    w, h = im.size
    px = im.load()
    for _ in range(passes):
        changed = 0
        for y in range(h):
            for x in range(w):
                if px[x, y][3] > 8:
                    continue
                acc, n = [0, 0, 0], 0
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < w and 0 <= ny < h and px[nx, ny][3] > 8:
                        c = px[nx, ny]
                        acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2]
                        n += 1
                if n:
                    px[x, y] = (acc[0] // n, acc[1] // n, acc[2] // n, px[x, y][3])
                    changed += 1
        if not changed:
            break
    return im


def resize_rgba(im, size):
    """Уменьшение с предумноженной альфой.

    Если ресайзить RGBA «в лоб», цвет прозрачных пикселей участвует в
    усреднении наравне с видимыми — края темнеют. Предумножение на альфу
    убирает этот вклад, после чего альфу возвращаем делением.
    """
    im = im.convert("RGBA")
    w, h = im.size
    px = im.load()
    pm = Image.new("RGBA", (w, h))
    q = pm.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            q[x, y] = (r * a // 255, g * a // 255, b * a // 255, a)

    pm = pm.resize(size, Image.LANCZOS)
    out = Image.new("RGBA", size)
    o, s = out.load(), pm.load()
    for y in range(size[1]):
        for x in range(size[0]):
            r, g, b, a = s[x, y]
            if a:
                o[x, y] = (min(255, r * 255 // a), min(255, g * 255 // a),
                           min(255, b * 255 // a), a)
            else:
                o[x, y] = (0, 0, 0, 0)
    return out


# Порядковая матрица 4x4 — размывает переход между уровнями прозрачности
_BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def png_to_tim(src, vx, vy, bpp=8, cx=None, cy=None, transparent_index0=True,
               alpha="cut", fit=None, dither_alpha=True):
    """Возвращает bytes с содержимым .TIM.

    alpha:
      "cut" — жёсткий порог: пиксель либо виден, либо нет (как было).
      "stp" — три уровня: прозрачный / полупрозрачный / непрозрачный.
              Использует бит STP в палитре, который железо смешивает с
              фоном 50/50. Спрайт при этом рисуется полупрозрачным
              примитивом. Только для 8bpp — в 16 цветах места нет.

    fit — (w, h): вписать картинку в размер с правильной альфой.
    """
    im = Image.open(src).convert("RGBA")
    if fit and im.size != tuple(fit):
        im = resize_rgba(bleed_edges(im), tuple(fit))

    if bpp == 16:
        w, h = im.size
        px = im.load()
        data = bytearray()
        for y in range(h):
            for x in range(w):
                r, g, b, a = px[x, y]
                data += struct.pack("<H", 0 if a < 128 else rgb555(r, g, b, 1))
        body = struct.pack("<HHHH", vx, vy, w, h) + bytes(data)
        return struct.pack("<II", 0x10, 0x02) + struct.pack("<I", len(body) + 4) + body

    use_stp = (alpha == "stp" and bpp == 8)
    ncol = 16 if bpp == 4 else 256

    alpha_ch = im.getchannel("A")
    w, h = im.size
    apx = alpha_ch.load()
    rgb = im.convert("RGB")

    if use_stp:
        # Палитру делим НЕ пополам: полупрозрачных пикселей на краю обычно
        # пара процентов, и отдавать им половину цветов расточительно —
        # тело спрайта теряет в цвете больше, чем выигрывает край.
        # Поэтому квантуем две области раздельно и по их реальному весу.
        edge = [(x, y) for y in range(h) for x in range(w)
                if 8 <= apx[x, y] < 200]
        n_edge = max(8, min(64, len(edge) * ncol // max(1, w * h) + 16))
        n_body = ncol - 1 - n_edge

        body_q = rgb.quantize(colors=n_body, method=Image.MEDIANCUT)
        body_pal = (body_q.getpalette() or [])
        body_pal = (body_pal + [0] * (n_body * 3))[: n_body * 3]

        if edge:                       # отдельная палитра только для края
            strip = Image.new("RGB", (len(edge), 1))
            sp = strip.load()
            rp = rgb.load()
            for i, (x, y) in enumerate(edge):
                sp[i, 0] = rp[x, y]
            edge_q = strip.quantize(colors=n_edge, method=Image.MEDIANCUT)
            edge_pal = (edge_q.getpalette() or [])
            emap = edge_q.load()
            edge_idx = {(x, y): emap[i, 0] for i, (x, y) in enumerate(edge)}
        else:
            edge_pal, edge_idx = [], {}
        edge_pal = (edge_pal + [0] * (n_edge * 3))[: n_edge * 3]

        palette = [(0, 0, 0, 0)]
        for i in range(n_body):
            palette.append((body_pal[i * 3], body_pal[i * 3 + 1],
                            body_pal[i * 3 + 2], 255))
        for i in range(n_edge):
            palette.append((edge_pal[i * 3], edge_pal[i * 3 + 1],
                            edge_pal[i * 3 + 2], 255))

        bpx = body_q.load()
        idx = [[0] * w for _ in range(h)]
        for y in range(h):
            for x in range(w):
                a = apx[x, y]
                if a < 8:
                    continue
                if a >= 200:
                    idx[y][x] = bpx[x, y] + 1
                else:
                    # дизеринг решает, станет пиксель полупрозрачным или пропадёт
                    keep = (a - 8) * 255 // 192 > _BAYER[y & 3][x & 3] * 16
                    if keep:
                        idx[y][x] = 1 + n_body + edge_idx.get((x, y), 0)
        stp_lo, stp_hi = 1 + n_body, 1 + n_body + n_edge
    else:
        nbase = ncol - 1
        q = rgb.quantize(colors=nbase, method=Image.MEDIANCUT)
        pal = (q.getpalette() or [])
        pal = (pal + [0] * (nbase * 3))[: nbase * 3]
        palette = [(0, 0, 0, 0)]
        for i in range(nbase):
            palette.append((pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2], 255))
        qpx = q.load()
        idx = [[0] * w for _ in range(h)]
        for y in range(h):
            for x in range(w):
                if not transparent_index0:
                    idx[y][x] = qpx[x, y] + 1
                else:
                    idx[y][x] = 0 if apx[x, y] < 128 else qpx[x, y] + 1
        stp_lo = stp_hi = 0

    data = bytearray()
    if bpp == 4:
        assert w % 4 == 0, "ширина 4bpp-картинки должна делиться на 4"
        for y in range(h):
            for x in range(0, w, 2):
                data.append((idx[y][x] & 15) | ((idx[y][x + 1] & 15) << 4))
    else:
        assert w % 2 == 0, "ширина 8bpp-картинки должна быть чётной"
        for y in range(h):
            for x in range(w):
                data.append(idx[y][x] & 255)

    hw = w // (4 if bpp == 4 else 2)     # ширина в полусловах VRAM

    clut = _pack_clut(palette, ncol)
    if use_stp:                     # краевым цветам — бит полупрозрачности
        for e in range(stp_lo, min(stp_hi, ncol)):
            if clut[e]:
                clut[e] |= 0x8000
    cblk = struct.pack("<HHHH", cx or 0, cy or 0, ncol, 1) + \
        b"".join(struct.pack("<H", c) for c in clut)
    iblk = struct.pack("<HHHH", vx, vy, hw, h) + bytes(data)

    flags = (0 if bpp == 4 else 1) | 0x08
    return (struct.pack("<II", 0x10, flags) +
            struct.pack("<I", len(cblk) + 4) + cblk +
            struct.pack("<I", len(iblk) + 4) + iblk)


def raw_indexed_to_tim(idx, w, h, palette, vx, vy, bpp, cx, cy):
    """Тот же TIM, но из готового массива индексов (нужно для шрифта)."""
    data = bytearray()
    if bpp == 4:
        for y in range(h):
            for x in range(0, w, 2):
                data.append((idx[y][x] & 15) | ((idx[y][x + 1] & 15) << 4))
        hw = w // 4
    else:
        for y in range(h):
            for x in range(w):
                data.append(idx[y][x] & 255)
        hw = w // 2

    ncol = 16 if bpp == 4 else 256
    cblk = struct.pack("<HHHH", cx, cy, ncol, 1) + \
        b"".join(struct.pack("<H", c) for c in _pack_clut(palette, ncol))
    iblk = struct.pack("<HHHH", vx, vy, hw, h) + bytes(data)
    flags = (0 if bpp == 4 else 1) | 0x08
    return (struct.pack("<II", 0x10, flags) +
            struct.pack("<I", len(cblk) + 4) + cblk +
            struct.pack("<I", len(iblk) + 4) + iblk)
