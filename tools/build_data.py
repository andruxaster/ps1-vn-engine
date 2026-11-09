#!/usr/bin/env python3
"""Сборка данных для VN-движка.

  python3 tools/build_data.py

Делает три вещи:
  1. Растеризует TTF в атлас FONT.TIM + таблицу ширин FONT.WID
  2. Конвертирует картинки из assets.txt в A000.TIM, A001.TIM, ...
  3. Компилирует script/main.vns в SCRIPT.VNS (байткод + пул строк)

Результат кладётся в data/.
"""

import argparse
import os
import re
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psxtim import png_to_tim, raw_indexed_to_tim   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "data")

# Раскладка каталогов на диске. Имена короткие и заглавные: ISO9660 первого
# уровня допускает восемь символов, а длинные имена часть приводов читает
# иначе, чем эмуляторы. Плоская папка на несколько сотен файлов неудобна и
# для человека, и для инструментов.
DIR_ART = "ART"     # картинки: A0000.TIM ...
DIR_SCR = "SCR"     # главы сценария: CH00.VNS ...
DIR_BGM = "BGM"     # музыка: BANK.VAB, M000.SEQ ...
DIR_STR = "STR"     # видео: V000.STR ...
DIR_SYS = "SYS"     # шрифт и служебное
SUBDIRS = (DIR_ART, DIR_SCR, DIR_BGM, DIR_STR, DIR_SYS)


PAK_MAGIC = b"APAK"
SECTOR = 2048


def _crop_cache(orig, im, name):
    """Сохраняет обрезанную картинку во временный файл: конвертер TIM
    работает с путями, а не с объектами."""
    d = os.path.join(OUT, "_crop")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, name.replace("/", "_") + ".png")
    im.save(p)
    return p


def build_pak(items_written):
    """Складывает картинки в один файл с указателем.

    Зачем: при загрузке каждой картинки движок сейчас ищет её по каталогу
    ISO9660 — это чтение каталога плюс два позиционирования головки, что
    почти удваивает время загрузки. В общем файле указатель читается один
    раз при старте, а дальше привод встаёт сразу на нужный сектор.

    Раскладка:
        сектор 0..N  указатель: "APAK", число записей, затем по 8 байт на
                     запись — номер сектора от начала файла и размер
        далее        сами картинки, каждая с границы сектора
    """
    if not items_written:
        return
    n = len(items_written)
    index_bytes = 8 + n * 8
    index_sectors = (index_bytes + SECTOR - 1) // SECTOR

    body = bytearray()
    entries = []
    for path in items_written:
        data = open(path, "rb").read()
        entries.append((index_sectors + len(body) // SECTOR, len(data)))
        body += data
        while len(body) % SECTOR:          # каждая картинка с новой границы
            body.append(0)

    index = bytearray(index_sectors * SECTOR)
    index[0:4] = PAK_MAGIC
    struct.pack_into("<HH", index, 4, n, 0)
    for i, (sec, size) in enumerate(entries):
        struct.pack_into("<II", index, 8 + i * 8, sec, size)

    out = os.path.join(OUT, "ART.PAK")
    open(out, "wb").write(bytes(index) + bytes(body))
    print(f"ART.PAK   картинок {n}, указатель {index_sectors} сект., "
          f"всего {(len(index) + len(body)) / 1024 / 1024:.1f} МБ")


def out_path(subdir, name):
    """Путь к файлу образа с созданием каталога."""
    d = os.path.join(OUT, subdir)
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, name)

# Шрифт для растеризации. По умолчанию DejaVu Sans (свободная лицензия,
# есть в большинстве дистрибутивов: пакет fonts-dejavu-core). Свой шрифт —
# через переменную окружения VN_FONT.
TTF = os.environ.get("VN_FONT",
                     "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
FONT_PX = 13

# ---------------------------------------------------------------- VRAM
# Кадровые буферы занимают (0,0)-(319,479). Всё остальное — текстуры.
# Место в VRAM теперь выбирает движок (см. vram_slot[] в src/gfx.c), поэтому
# Высота спрайта персонажа.
#
# Экран 240 строк, текстовое окно начинается со 158-й — значит персонажа
# видно ровно 158 строк. Прежние 256 не помещались никак: часть срезал
# верхний край, часть закрывало окно. Обрезаем снизу (там всё равно окно)
# и ставим персонажа в y=0 — тогда сверху не срезается ничего.
#
# Заодно это возвращает почти 8% видеопамяти: четыре слота по 128x176
# вместо 128x256.
# Высота спрайта персонажа.
#
# Одно время здесь стояло 176 — из расчёта, что нижняя часть скрыта
# текстовым окном. Расчёт был неверным: окно ПОЛУПРОЗРАЧНОЕ, сквозь него
# фигура видна, и обрезка отрезала нужное — у высоких спрайтов до десяти
# тысяч непрозрачных пикселей.
#
# 256 — полный размер исходника. Обрезки нет вовсе, а значит нет и риска
# потерять видимое при отрицательном y, когда автор поднимает фигуру.
# Стоит это ровно ничего: слоты персонажей в видеопамяти изначально
# рассчитаны на 256 строк.
# Кадров у анимированной сцены. ОБЯЗАНО совпадать со SCENE_FRAMES в
# src/vn.h: столько слотов отведено в видеопамяти. Столько же кадров
# помещается в запись галереи.
SCENE_MAX = 4

CHAR_H = 256

# конвертеру нужны только предельные размеры и глубина цвета.
KIND = {
    #  тип       bpp   максимальный размер
    "bg":    dict(bpp=8, size=(320, 240)),
    "char":  dict(bpp=8, size=(256, CHAR_H)),
    "scene": dict(bpp=8, size=(320, 240)),
}
FONT_VRAM = dict(vx=320, vy=256, bpp=4, cx=0, cy=484)

TEXT_MAX_W = 280        # ширина строки в текстовом окне, пикселей
TEXT_MAX_LINES = 3

# --------------------------------------------------------------- шрифт
# ВНИМАНИЕ: GLYPH_H должен вмещать ascent+descent шрифта целиком.
# У DejaVuSans 13px это 13+4=17, поэтому ячейка 18, а не 16: при 16 хвосты
# букв (р, у, ф, y, q, g) вылезали на 1 px в ячейку этажом ниже, и над
# буквами Б, В, К, Н, Ё в тексте появлялась «случайная» точка.
GLYPH_W = 16
GLYPH_H = 18
COLS = 16
NCHARS = 161                 # собственно символы
ICON_CROSS = 161             # значок кнопки «крестик» — маркер «жми дальше»
NGLYPH = 162                 # всего ячеек в атласе


def draw_cross_icon():
    """Значок кнопки: кружок с диагональным крестом внутри.

    Рисуется процедурно, а не берётся из картинки: на 16x18 всё равно
    нужна своя геометрия, зато значок живёт в атласе шрифта, красится тем
    же тоном и не требует отдельного места в VRAM.
    """
    cell = Image.new("L", (GLYPH_W, GLYPH_H), 0)
    d = ImageDraw.Draw(cell)
    cx, cy, r = 7.5, 9.5, 6.5
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=255, width=1)
    # Крест рисуем целыми пикселями от центра: на сетке 16x18 дробные
    # координаты дают разрыв посередине.
    for k in range(-3, 4):
        cell.putpixel((7 + k, 9 + k), 255)
        cell.putpixel((7 + k, 10 - k), 255)
    return cell


def glyph_chars():
    """Порядок глифов должен совпадать с cp2glyph() в gfx.c."""
    chars = [chr(c) for c in range(0x20, 0x7F)]          # 0..94
    chars += [chr(c) for c in range(0x410, 0x450)]       # 95..158
    chars += ["\u0401", "\u0451"]                        # Ё, ё
    assert len(chars) == NCHARS
    return chars


def render_glyph(font, ch, baseline):
    """Рисует один глиф в СВОЙ холст размером с ячейку.

    Изоляция принципиальна: PIL спокойно выводит пиксели за границы, и при
    отрисовке прямо в общий атлас они утекают к соседям. Здесь всё, что не
    влезло, просто обрезается холстом и испортить соседа не может.
    """
    cell = Image.new("L", (GLYPH_W, GLYPH_H), 0)
    ImageDraw.Draw(cell).text((1, baseline), ch, fill=255, font=font, anchor="ls")
    return cell


def build_font():
    chars = glyph_chars()
    rows = (NGLYPH + COLS - 1) // COLS
    W, H = COLS * GLYPH_W, rows * GLYPH_H

    if not os.path.exists(TTF):
        raise SystemExit(f"нет шрифта {TTF}: установите fonts-dejavu-core "
                         f"или укажите свой через VN_FONT=/путь/к/шрифту.ttf")
    font = ImageFont.truetype(TTF, FONT_PX)
    asc, desc = font.getmetrics()
    if asc + desc > GLYPH_H:
        raise SystemExit(
            f"шрифт {FONT_PX}px не влезает в ячейку {GLYPH_H}px "
            f"(нужно {asc + desc}px). Поднимите GLYPH_H или уменьшите FONT_PX.")
    baseline = 1 + asc                      # 1 px отступ сверху

    img = Image.new("L", (W, H), 0)
    widths = []
    for i, ch in enumerate(chars):
        gx, gy = (i % COLS) * GLYPH_W, (i // COLS) * GLYPH_H
        img.paste(render_glyph(font, ch, baseline), (gx, gy))
        adv = int(round(font.getlength(ch)))
        widths.append(max(2, min(GLYPH_W, adv)))

    # значок кнопки идёт последней ячейкой, сразу за символами
    ix, iy = (ICON_CROSS % COLS) * GLYPH_W, (ICON_CROSS // COLS) * GLYPH_H
    img.paste(draw_cross_icon(), (ix, iy))
    widths.append(GLYPH_W)

    # проверяем, что ни один глиф не залез к соседям: ячейка атласа обязана
    # совпадать с изолированной отрисовкой того же символа
    px = img.load()
    for i, ch in enumerate(chars):
        gx, gy = (i % COLS) * GLYPH_W, (i // COLS) * GLYPH_H
        ref = render_glyph(font, ch, baseline).load()
        for yy in range(GLYPH_H):
            for xx in range(GLYPH_W):
                if px[gx + xx, gy + yy] != ref[xx, yy]:
                    raise SystemExit(
                        f"глиф '{ch}' (#{i}) не совпал с изолированной "
                        f"отрисовкой в точке ({xx},{yy}) — подтёк в атласе")

    # L (0..255) -> 4bpp: индекс 0 прозрачный, 1..15 — градации серого
    idx = [[0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            v = px[x, y]
            idx[y][x] = 0 if v < 24 else max(1, min(15, v * 15 // 255))

    palette = [(0, 0, 0, 0)]
    for i in range(1, 16):
        c = i * 255 // 15
        palette.append((c, c, c, 255))

    tim = raw_indexed_to_tim(idx, W, H, palette,
                             FONT_VRAM["vx"], FONT_VRAM["vy"], 4,
                             FONT_VRAM["cx"], FONT_VRAM["cy"])
    open(out_path(DIR_SYS, "FONT.TIM"), "wb").write(tim)
    open(out_path(DIR_SYS, "FONT.WID"), "wb").write(bytes(widths))
    print(f"FONT.TIM  {W}x{H} 4bpp, {NGLYPH} глифов, ячейка {GLYPH_W}x{GLYPH_H}")
    return widths


def text_width(s, widths):
    chars = glyph_chars()
    lut = {c: widths[i] for i, c in enumerate(chars)}
    return sum(lut.get(c, widths[0]) for c in s)


# -------------------------------------------------------------- assets
def discover_assets():
    """Обходит art/ и раскладывает картинки по типам.

        art/bg/*.png            -> имя = имя файла        ("room")
        art/char/<кто>/*.png    -> имя = "кто/выражение"   ("aya/smile")
        art/scene/<что>/*.png   -> имя = "что", кадры по алфавиту

    Раньше был плоский assets.txt, где у каждой картинки приходилось
    указывать слот VRAM. Теперь место выбирает движок при загрузке,
    поэтому от манифеста остаётся только структура папок.

    Возвращает (ids, scenes): имя -> номер ассета и имя сцены -> список
    номеров её кадров.
    """
    art = os.path.join(ROOT, "art")
    ids, scenes, items = {}, {}, []
    _art_files = []

    # Чистим ассеты от прошлой сборки. Без этого при смене схемы имён
    # (A000 -> A0000) или удалении картинки старые файлы остаются в data/
    # и продолжают попадать в образ, раздувая его дубликатами.
    for f in os.listdir(OUT):
        if re.fullmatch(r"A\d+\.TIM", f):
            os.remove(os.path.join(OUT, f))

    bgdir = os.path.join(art, "bg")
    if os.path.isdir(bgdir):
        for f in sorted(os.listdir(bgdir)):
            if f.lower().endswith(".png"):
                items.append((os.path.splitext(f)[0], os.path.join(bgdir, f), "bg"))

    chdir = os.path.join(art, "char")
    if os.path.isdir(chdir):
        for who in sorted(os.listdir(chdir)):
            wd = os.path.join(chdir, who)
            if not os.path.isdir(wd):
                continue
            for f in sorted(os.listdir(wd)):
                if f.lower().endswith(".png"):
                    name = f"{who}/{os.path.splitext(f)[0]}"
                    items.append((name, os.path.join(wd, f), "char"))

    scdir = os.path.join(art, "scene")
    scene_frames = {}
    if os.path.isdir(scdir):
        for what in sorted(os.listdir(scdir)):
            wd = os.path.join(scdir, what)
            if not os.path.isdir(wd):
                continue
            frames = [f for f in sorted(os.listdir(wd)) if f.lower().endswith(".png")]
            if not frames:
                continue
            if len(frames) > SCENE_MAX:
                print(f"  ! сцена '{what}': кадров {len(frames)}, "
                      f"в VRAM влезает только {SCENE_MAX} — лишние пропущены")
                frames = frames[:SCENE_MAX]
            scene_frames[what] = []
            for i, f in enumerate(frames):
                name = f"{what}#{i}"
                items.append((name, os.path.join(wd, f), "scene"))
                scene_frames[what].append(name)

    if not items:
        print("art/ пуст — картинок не будет")
        return ids, scenes

    for n, (name, path, kind) in enumerate(items):
        cfg = KIND[kind]
        im = Image.open(path)
        maxw, maxh = cfg["size"]
        fit = None

        # Персонажа обрезаем, а не вписываем: масштабирование уменьшило бы
        # фигуру, а нам нужно сохранить её размер и убрать лишь то, что
        # всё равно скрыто текстовым окном.
        if kind == "char" and im.size[1] > maxh:
            im = im.convert("RGBA")
            cut = im.crop((0, maxh, im.size[0], im.size[1]))
            a = cut.split()[3]
            lost = sum(1 for v in a.getdata() if v > 8)
            im = im.crop((0, 0, im.size[0], maxh))
            path = _crop_cache(path, im, name)
            if lost > cut.size[0] * cut.size[1] // 20:
                print(f"  ! {name}: обрезано снизу {lost} непрозрачных "
                      f"пикселей — проверьте, не пропало ли нужное")

        if im.size[0] > maxw or im.size[1] > maxh:
            # Вписываем сами, с предумноженной альфой: ресайз «в лоб»
            # затемняет края, и потом они рвутся при квантовании.
            k = min(maxw / im.size[0], maxh / im.size[1])
            fw = max(2, int(im.size[0] * k) & ~1)      # ширина 8bpp — чётная
            fh = max(1, int(im.size[1] * k))
            fit = (fw, fh)
            print(f"  {name}: {im.size[0]}x{im.size[1]} -> {fw}x{fh}")

        # У персонажей мягкий край волос: даём им третий уровень
        # прозрачности через бит STP, фоны в нём не нуждаются.
        amode = "stp" if kind == "char" else "cut"
        tim = png_to_tim(path, 0, 0, cfg["bpp"], 0, 0, alpha=amode, fit=fit)
        _art_files.append(out_path(DIR_ART, f"A{n:04d}.TIM"))
        open(_art_files[-1], "wb").write(tim)
        ids[name] = n
        shown = fit or im.size
        print(f"A{n:04d}.TIM  {name:<20} {kind:<5} "
              f"{shown[0]}x{shown[1]}  {len(tim)//1024} КБ"
              f"{'  (мягкий край)' if amode == 'stp' else ''}")

    # Таблица имён для отладочного оверлея: по 24 байта на ассет, индекс
    # совпадает с номером в A%03d.TIM. Файл нужен только для отладки —
    # если его нет на диске, движок просто покажет номера.
    tbl = bytearray()
    for n in range(len(items)):
        nm = items[n][0].encode("utf-8")[:23]
        tbl += nm + bytes(24 - len(nm))
    # Таблица имён нужна только отладочному оверлею.
    #
    # ВНИМАНИЕ: здесь нельзя выходить из функции — ниже идёт сборка общего
    # файла картинок и разбор сцен, а функция возвращает данные об ассетах.
    names_path = out_path(DIR_SYS, "NAMES.DBG")
    if RELEASE:
        if os.path.exists(names_path):
            os.remove(names_path)
        print("NAMES.DBG  пропущено (релиз)")
    else:
        open(names_path, "wb").write(bytes(tbl))
        print(f"NAMES.DBG  имён {len(items)} ({len(tbl)} байт, "
              f"только для отладки)")

    build_pak(_art_files)

    for what, names in scene_frames.items():
        scenes[what] = [ids[n] for n in names]
        print(f"  сцена '{what}': кадров {len(names)}")

    return ids, scenes


# --------------------------------------------------------------- видео
STR_SECTOR = 2048       # ровно столько отдаёт CdGetSector в режиме RT
STR_ID = 0x0160
STR_TYPE = 0x8001


def _iter_user_data(data):
    """Отдаёт 2048 «пользовательских» байт каждого сектора — ровно то, что
    видит str.c через CdGetSector в режиме RT (железо снимает CD-обвес).

    Поддерживает три раскладки файла:
      2352 — полный CD-сектор (strcd): sync12+header4+subheader8+data2048+EDC/ECC
      2336 — Mode2 без sync/header (str): subheader8 + data2048 + ...
      2048 — «сырые» пользовательские данные (старый синтетический формат)
    Для 2352/2336 попутно отдаёт submode-байт, чтобы отличать видео от аудио.
    """
    # Только по кратности размера раскладку не угадать: 2352 и 2336 делят
    # один и тот же размер каждые 147 секторов, и такой ролик разбирался
    # как сырые 2352 — находился один «кадр», и движок обрывал видео на
    # первом же кадре. Сырой сектор узнаётся по синхропоследовательности.
    if len(data) % 2352 == 0 and data[:12] == b"\x00" + b"\xff" * 10 + b"\x00":
        step, sub_off, data_off = 2352, 0x12, 0x18
    elif len(data) % 2336 == 0:
        step, sub_off, data_off = 2336, 0x02, 0x08
    else:
        step, sub_off, data_off = 2048, None, 0

    pos = 0
    while pos + step <= len(data):
        submode = data[pos + sub_off] if sub_off is not None else None
        yield submode, data[pos + data_off: pos + data_off + 2048]
        pos += step


def scan_str(path):
    """Возвращает (кадров, ширина, высота).

    Кадр = сектор с корректной STR-сигнатурой (0x0160/0x8001) и sec_no == 0.
    Аудиосектора (submode с битом Form2 = 0x20) пропускаются — как это делает
    железо и сам str.c по несовпадению сигнатуры.
    """
    data = open(path, "rb").read()

    frames = 0
    w = h = 0
    saw_signature = False

    for submode, body in _iter_user_data(data):
        if submode is not None and (submode & 0x20):   # Form2 = аудио
            continue
        if len(body) < 8:
            continue
        sid, stype, sec_no, sec_cnt = struct.unpack("<HHHH", body[:8])
        if sid != STR_ID or stype != STR_TYPE:
            continue
        saw_signature = True
        if sec_no == 0:
            frames += 1
            # psxavenc: +8 frame_no, +12 frame_size, +16 width, +18 height
            if len(body) >= 20:
                ww, hh = struct.unpack("<HH", body[16:20])
                if 16 <= ww <= 640 and 16 <= hh <= 480:
                    w, h = ww, hh

    if not saw_signature:
        raise SystemExit(
            f"не удалось разобрать STR '{path}': не найдено ни одного сектора "
            f"с сигнатурой 0x0160/0x8001.\n"
            f"Запустите  python3 tools/probe_str.py {path}  и пришлите вывод.")
    if frames == 0:
        raise SystemExit(
            f"STR '{path}': сигнатура есть, но нет сектора sec_no==0 — "
            f"файл повреждён или обрезан.")

    return frames, w, h


def build_music():
    """music.txt:  <имя>  <файл.mid>  [legacy_tick_column]  [loop]"""
    # Remove assets from the old private VSQ/VSB1 pipeline so stale files can
    # never leak into iso.xml after switching to the Psy-Q-compatible formats.
    for old in ("BANK.SPU",):
        p = os.path.join(OUT, old)
        if os.path.exists(p):
            os.remove(p)
    for f in list(os.listdir(OUT)):
        if re.fullmatch(r"M\d{3}\.(VSQ|SEQ)", f):
            os.remove(os.path.join(OUT, f))
    path = os.path.join(ROOT, "music.txt")
    songs = {}
    if not os.path.exists(path):
        return songs

    import subprocess
    n = 0
    for line in open(path, encoding="utf-8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        parts = line.split()
        name, src = parts[0], parts[1]
        # The Sony SEQ stores PPQN + tempo in the file; there is no fixed
        # player tick rate to bake into the asset. Keep the old optional
        # third column for backwards-compatible music.txt files.
        loop = "loop" in parts[3:] or (len(parts) > 2 and parts[2].lower() == "loop")

        srcp = os.path.join(ROOT, src)
        if not os.path.exists(srcp):
            raise SystemExit(f"MIDI {src} не найден — положите файл "
                             f"или уберите строку из music.txt")

        out = out_path(DIR_BGM, f"M{n:03d}.SEQ")
        cmd = [sys.executable, os.path.join(ROOT, "tools", "mkseq.py"), srcp, out]
        if loop:
            cmd.append("--loop")
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"mkseq упал на {src}:\n{r.stderr}")
        head = r.stdout.strip().splitlines()
        print(f"M{n:03d}.SEQ  {name:<12} {head[1].strip() if len(head) > 1 else ''}")
        songs[name] = n
        n += 1
    return songs


def build_voice():
    """voice.txt: <имя> <файл.wav>. Собирает общий файл озвучки."""
    import subprocess
    manifest = os.path.join(ROOT, "voice.txt")
    if not os.path.exists(manifest):
        return {}

    xas = os.path.join("data", "VOICE.XAS")
    idx = os.path.join("data", "VOICE.IDX")
    r = subprocess.run(
        [sys.executable, os.path.join(ROOT, "tools", "mkxas.py"),
         "voice.txt", xas, idx],
        capture_output=True, text=True, cwd=ROOT)
    if r.returncode:
        raise SystemExit(f"mkxas упал:\n{r.stderr}")
    for line in r.stdout.strip().splitlines():
        print("  " + line if not line.startswith(" ") else line)

    names, n = {}, 0
    for line in open(manifest, encoding="utf-8"):
        line = line.split("#")[0].strip()
        if line:
            names[line.split()[0]] = n
            n += 1
    return names


def build_bank():
    """Собирает BANK.VAB. Если инструментов нет — синтезирует пробный тон,
    чтобы музыка звучала сразу."""
    import subprocess
    out = out_path(DIR_BGM, "BANK.VAB")
    cmd = [sys.executable, os.path.join(ROOT, "tools", "mkvab.py"), out]
    wavs = os.path.join(ROOT, "instruments")
    if os.path.isdir(wavs):
        for f in sorted(os.listdir(wavs)):
            if f.lower().endswith(".wav"):
                cmd += ["--wav", os.path.join(wavs, f)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"mkvab упал:\n{r.stderr}")
    print("BANK.VAB  " + r.stdout.strip().splitlines()[-1].split(": ", 1)[-1])

    # Номера сэмплов для команды se: сборщик банка печатает их строкой
    # «sfx: имя=номер,...». Так сценарий ссылается на звук по имени, а в
    # байткод попадает номер.
    sfx = {}
    for line in r.stdout.splitlines():
        line = line.strip()
        if line.startswith("sfx: "):
            for part in line[5:].split(","):
                k, _, v = part.partition("=")
                if v.isdigit():
                    sfx[k.strip()] = int(v)
    if sfx:
        print(f"  звуков по именам: "
              + ", ".join(f"{k}={v}" for k, v in sfx.items()))
    return sfx


def build_videos():
    """videos.txt:  <имя>  <файл.str>"""
    path = os.path.join(ROOT, "videos.txt")
    vids, order = {}, []
    if not os.path.exists(path):
        return vids, order

    n = 0
    for line in open(path, encoding="utf-8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        name, src = line.split()
        srcp = os.path.join(ROOT, src)
        if not os.path.exists(srcp):
            print(f"  ! видео {src} не найдено — пропущено, но на него ссылается сценарий")
            vids[name] = (n, 0, 0, 0)
            order.append(src)
            n += 1
            continue
        frames, w, h = scan_str(srcp)
        vids[name] = (n, frames, w, h)
        order.append(src)
        secs = frames / 15.0
        print(f"V{n:03d}.STR  {name:<12} {frames} кадров {w}x{h} (~{secs:.1f}s @15fps)")
        n += 1
    return vids, order


def make_placeholder(path, name, kind, size):
    w, h = size
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def centered(txt, cy, fill, px=20):
        try:
            f = ImageFont.truetype(TTF, px)
        except Exception:
            f = None
        tw = int(d.textlength(txt, font=f)) if f else len(txt) * 6
        d.text(((w - tw) // 2, cy), txt, fill=fill, font=f)

    if name == "logo":
        # тёмный фон + рамка + подпись по центру: узнаваемый «логотип-заглушка»
        d.rectangle([0, 0, w - 1, h - 1], fill=(12, 14, 22, 255))
        d.rectangle([w // 2 - 70, h // 2 - 40, w // 2 + 70, h // 2 + 40],
                    outline=(120, 140, 200, 255), width=2)
        centered("YOUR LOGO", h // 2 - 12, (200, 210, 240, 255), 22)
        centered("[placeholder]", h // 2 + 16, (90, 100, 140, 255), 12)
    elif name == "title":
        for y in range(h):                       # мягкий вертикальный градиент
            t = y / h
            d.line([(0, y), (w, y)],
                   fill=(int(20 + 30 * t), int(24 + 40 * t), int(48 + 60 * t), 255))
        # текст НЕ рисуем: заголовок выводит сам движок поверх фона
    elif kind == "bg":
        for y in range(h):
            t = y / h
            d.line([(0, y), (w, y)],
                   fill=(int(30 + 90 * t), int(40 + 60 * t), int(90 - 30 * t), 255))
        d.text((10, 10), name, fill=(255, 255, 255, 255))
    else:
        d.ellipse([w // 4, 20, w * 3 // 4, h - 20], fill=(220, 190, 170, 255))
        d.text((10, 10), name, fill=(255, 255, 255, 255))

    img.save(path)


# -------------------------------------------------------------- скрипт
# Команды языка сценария. Нужен, чтобы отличать команду от реплики при
# отрезании концевых комментариев.
KNOWN_CMDS = {
    "gallery",
    "shake", "flash", "tint", "slide",
    "bg", "bgm", "box", "chapter", "char", "choice", "clear", "end", "fade",
    "goto", "if", "menu", "name", "scene", "se", "set", "splash", "str",
    "voice", "wait",
}

OPS = dict(END=0, BG=1, CHAR=2, NAME=3, TEXT=4, CLEARBOX=5, WAIT=6,
           FADE=7, BGM=8, SE=9, CHOICE=10, JUMP=11, SET=12, IFJMP=13, BOX=14,
           STR=15, SPLASH=16, MENU=17, SCENE=18, VOICE=19, CHAPTER=20, FX=21, GALLERY=22)


# Пул строк «зашумляется» потоком от xorshift32. Это не шифрование:
# любой, кто откроет файл в hex-редакторе, снимет его за пару минут.
# Задача скромнее — чтобы текст не вываливался в strings, не попадал
# в поиск по файлам и не читался случайно. Значение обязано совпадать
# со SCRAMBLE_SEED в src/vm.c.
SCRAMBLE_SEED = 0x5CA1AB1E


def keystream(seed, n):
    """xorshift32: три сдвига и три XOR на каждые четыре байта."""
    x = seed & 0xFFFFFFFF or 1          # ноль вырождает генератор
    out = bytearray()
    while len(out) < n:
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        x &= 0xFFFFFFFF
        out += x.to_bytes(4, "little")
    return bytes(out[:n])


def scramble(data, chunk_id):
    ks = keystream(SCRAMBLE_SEED ^ chunk_id, len(data))
    return bytes(a ^ b for a, b in zip(data, ks))


CHUNK_DEFAULT = 64 * 1024      # целевой размер главы; буфер в main.c — 96 КБ


class Compiler:
    def __init__(self, assets, widths, videos, chunk_size=CHUNK_DEFAULT,
                 scenes=None, music=None, voices=None, sfx=None):
        self.chunk_size = chunk_size
        self.scenes = scenes or {}
        self.voices = voices or {}
        self.sfx = sfx or {}   # имя звука -> номер сэмпла в банке     # имя реплики -> номер в VOICE.IDX
        self.chapters = []             # точки сохранения
        self.gallery = []              # сцены галереи
        self.gallery_seen = {}
        self.force_cuts = []           # где обязан начаться новый чанк
        self._chapter_open = None      # глава, ещё не выстроившая сцену
        self.assets = assets
        self.widths = widths
        self.videos = videos
        self.music = music or {}
        self.code = bytearray()
        self.strs = bytearray(b"\x00")
        self.pool = {"": 0}
        self.labels = {}
        self.fixups = []        # (позиция в code, имя метки)

    def s(self, text):
        if text in self.pool:
            return self.pool[text]
        off = len(self.strs)
        self.strs += text.encode("utf-8") + b"\x00"
        self.pool[text] = off
        return off

    def asset(self, name):
        if name not in self.assets:
            raise SystemExit(f"неизвестный ресурс '{name}'")
        return self.assets[name]

    def video(self, name):
        if name not in self.videos:
            raise SystemExit(f"неизвестное видео '{name}' (нет в videos.txt)")
        return self.videos[name]      # (id, frames, w, h)

    def label_ref(self, name):
        self.fixups.append((len(self.code), name))
        self.code += b"\0\0\0\0"

    def wrap_pages(self, text):
        """Реплика -> список страниц по TEXT_MAX_LINES строк.

        Длинная реплика не обрезается и не вылезает за окно, а делится на
        несколько страниц; между ними игрок нажимает кнопку, имя говорящего
        остаётся. Делим по границе строк, а не посреди слова.
        """
        lines = self.wrap(text, warn=False).split("\n")
        return ["\n".join(lines[i:i + TEXT_MAX_LINES])
                for i in range(0, len(lines), TEXT_MAX_LINES)]

    def wrap(self, text, warn=True):
        """Переносы считаем на ПК — на PS1 это лишние такты."""
        out, line = [], ""
        for word in text.split():
            probe = (line + " " + word) if line else word
            if text_width(probe, self.widths) > TEXT_MAX_W and line:
                out.append(line)
                line = word
            else:
                line = probe
        if line:
            out.append(line)
        if warn and len(out) > TEXT_MAX_LINES:
            print(f"  ! реплика не влезает в {TEXT_MAX_LINES} строки: {text[:40]}...")
        return "\n".join(out)

    def compile(self, path):
        lines = open(path, encoding="utf-8").read().splitlines()
        i = 0
        while i < len(lines):
            raw = lines[i]
            i += 1
            line = raw.strip()
            if not line or line.startswith("#"):
                continue

            if line.startswith("@"):
                self.labels[line[1:].strip()] = len(self.code)
                continue

            m = re.match(r"^(\w+)\b\s*(.*)$", line)
            cmd = m.group(1).lower() if m else ""
            arg = m.group(2).strip() if m else ""

            # Комментарий в конце строки отрезаем ТОЛЬКО у известных команд.
            # У реплик этого делать нельзя: решётка — обычный символ, и
            # строка «Стоимость # 5» потеряла бы половину текста.
            if cmd in KNOWN_CMDS and "#" in arg:
                arg = arg.split("#")[0].strip()

            if cmd in ("bg", "scene"):
                self._chapter_open = None

            if cmd == "bg":
                self.code += bytes([OPS["BG"]]) + struct.pack("<H", self.asset(arg))
            elif cmd == "char":
                p = arg.split()
                slot = int(p[0])
                if p[1] == "hide":
                    self.code += bytes([OPS["CHAR"], slot]) + \
                        struct.pack("<Hhh", 0xFFFF, 0, 0)
                else:
                    x = int(p[2]) if len(p) > 2 else 0
                    y = int(p[3]) if len(p) > 3 else 0
                    self.code += bytes([OPS["CHAR"], slot]) + \
                        struct.pack("<Hhh", self.asset(p[1]), x, y)
            elif cmd == "name":
                self.code += bytes([OPS["NAME"]]) + struct.pack("<I", self.s(arg))
            elif cmd == "clear":
                self.code += bytes([OPS["CLEARBOX"]])
            elif cmd in ("shake", "flash", "tint", "slide"):
                # Все экранные эффекты живут в одном опкоде с подкомандой:
                # так не плодятся номера, а размер остаётся постоянным.
                #   shake 0.5s [сила]      тряска всего экрана
                #   flash 10 [цвет]        вспышка
                #   tint 60 40 20          тонирование, "tint off" убрать
                #   slide left 0.5s        сдвиг фона через чёрное
                p = arg.split()

                def dur(v, default=20):
                    if not v:
                        return default
                    n = int(round(float(v[:-1]) * 60)) if v.endswith("s") \
                        else int(v)
                    return max(1, min(255, n))

                if cmd == "shake":
                    frames = dur(p[0] if p else None, 30)
                    power = int(p[1]) if len(p) > 1 else 3
                    self.code += bytes([OPS["FX"], 0, power & 255,
                                        frames, 0, 0])
                elif cmd == "flash":
                    frames = dur(p[0] if p else None, 8)
                    col = {"white": (255, 255, 255), "red": (255, 60, 60),
                           "blue": (80, 120, 255)}.get(
                               p[1] if len(p) > 1 else "white",
                               (255, 255, 255))
                    self.code += bytes([OPS["FX"], 1, col[0] >> 1, col[1] >> 1,
                                        col[2] >> 1, frames])
                elif cmd == "tint":
                    if p and p[0] == "off":
                        self.code += bytes([OPS["FX"], 2, 0, 0, 0, 0])
                    else:
                        if len(p) < 3:
                            raise SystemExit("tint: нужны три числа или off")
                        self.code += bytes([OPS["FX"], 2,
                                            int(p[0]) & 255, int(p[1]) & 255,
                                            int(p[2]) & 255, 1])
                else:                      # slide
                    dirs = {"left": 0, "right": 1, "up": 2, "down": 3}
                    if not p or p[0] not in dirs:
                        raise SystemExit("slide: нужно left, right, up или down")
                    frames = dur(p[1] if len(p) > 1 else None, 30)
                    self.code += bytes([OPS["FX"], 3, dirs[p[0]],
                                        frames, 0, 0])
            elif cmd == "wait":
                # wait 90    — кадры развёртки (60 в секунде)
                # wait 1.5s  — секунды, так понятнее и труднее ошибиться
                a = arg.strip()
                if a.endswith("s"):
                    frames = int(round(float(a[:-1]) * 60))
                else:
                    frames = int(a)
                if frames > 65535:
                    raise SystemExit(f"wait {a}: слишком долго")
                self.code += bytes([OPS["WAIT"]]) + struct.pack("<H", frames)
            elif cmd == "fade":
                # fade out 24 [music] — с ключевым словом music громкость
                # уходит вместе с картинкой и возвращается на fade in.
                # Бит 1 кладётся в поле направления: размер опкода не растёт.
                # fade out 30      — за 30 кадров развёртки
                # fade out 0.5s     — за полсекунды
                # fade out 30 music — вместе с громкостью музыки
                #
                # Раньше число означало ШАГ яркости за кадр: чем больше,
                # тем быстрее, а «1» давало четыре с лишним секунды.
                # Это сбивало с толку, поэтому теперь это длительность.
                p = arg.split()
                d = 1 if p[0] == "out" else 0
                frames = 20
                for extra in p[1:]:
                    if extra == "music":
                        d |= 2
                    elif extra.endswith("s"):
                        frames = int(round(float(extra[:-1]) * 60))
                    else:
                        frames = int(extra)

                if frames < 1:
                    frames = 1
                step = 255 // frames
                if step < 1:
                    step = 1
                if step > 255:
                    step = 255
                self.code += bytes([OPS["FADE"], d, step])
            elif cmd == "box":
                self.code += bytes([OPS["BOX"], 0 if arg == "off" else 1])
            elif cmd == "str":
                # str <имя> [fps N] [noskip]
                p = arg.split()
                vid, frames, w, h = self.video(p[0])
                fps = 15
                skip = 1
                i2 = 1
                while i2 < len(p):
                    if p[i2] == "fps" and i2 + 1 < len(p):
                        fps = int(p[i2 + 1]); i2 += 2
                    elif p[i2] == "noskip":
                        skip = 0; i2 += 1
                    else:
                        raise SystemExit(f"str: непонятный параметр '{p[i2]}'")
                ticks = max(1, round(60 / fps))     # кадров NTSC на один кадр видео
                if frames > 0xFFFF:
                    raise SystemExit(f"str: слишком длинный ролик {p[0]} ({frames} кадров)")
                self.code += bytes([OPS["STR"], vid, ticks]) + \
                    struct.pack("<H", frames) + bytes([skip])
            elif cmd == "scene":
                # scene <имя> [задержка в кадрах]
                p = arg.split()
                if p[0] not in self.scenes:
                    raise SystemExit(f"неизвестная сцена '{p[0]}' "
                                     f"(нужна папка art/scene/{p[0]}/)")
                frames = self.scenes[p[0]]
                delay = int(p[1]) if len(p) > 1 else 8
                self.code += bytes([OPS["SCENE"], len(frames),
                                    max(1, min(255, delay))])
                for fr in frames:
                    self.code += struct.pack("<H", fr)
            elif cmd == "bgm":
                # Нормальный BGM-интерфейс:
                #   bgm play <name> [loop|once]
                #   bgm stop
                #   bgm fadeout <frames>
                #   bgm fadein <frames>
                #   bgm volume <0..127>
                p = arg.split()
                if not p:
                    raise SystemExit("bgm: требуется play/stop/fadeout/fadein/volume")
                sub = p[0].lower()
                if sub == "play":
                    if len(p) < 2 or p[1] not in self.music:
                        raise SystemExit(f"bgm play: неизвестный трек '{p[1] if len(p) > 1 else ''}'")
                    loop = 1 if len(p) < 3 or p[2].lower() == "loop" else 0
                    if len(p) >= 3 and p[2].lower() not in ("loop", "once"):
                        raise SystemExit("bgm play: ожидается loop или once")
                    self.code += bytes([OPS["BGM"], 0, self.music[p[1]], loop])
                elif sub == "stop":
                    self.code += bytes([OPS["BGM"], 1])
                elif sub in ("fadeout", "fadein"):
                    if len(p) != 2:
                        raise SystemExit(f"bgm {sub}: требуется число кадров")
                    frames = max(1, min(0xFFFF, int(p[1])))
                    mode = 2 if sub == "fadeout" else 3
                    self.code += bytes([OPS["BGM"], mode]) + struct.pack("<H", frames)
                elif sub == "volume":
                    if len(p) != 2:
                        raise SystemExit("bgm volume: требуется 0..127")
                    vol = max(0, min(127, int(p[1])))
                    self.code += bytes([OPS["BGM"], 4, vol])
                else:
                    raise SystemExit(f"bgm: неизвестная команда '{sub}'")
            elif cmd == "gallery":
                # gallery "Название" картинка
                #
                # Отмечает сцену открытой в галерее. Признак хранится
                # ГЛОБАЛЬНО, отдельно от слотов сохранения: галерея копится
                # между прохождениями, иначе новая игра стирала бы её.
                # gallery "Название" <фон или сцена> [задержка]
                #
                # Имя ищется сперва среди сцен, потом среди обычных
                # картинок: анимированную сцену в галерее хочется смотреть
                # так же, как в игре, а не одним застывшим кадром.
                m2 = re.match(r'^"([^"]*)"\s+(\S+)(?:\s+(\d+))?$', arg)
                if not m2:
                    raise SystemExit('gallery: нужно "Название" и имя '
                                     'картинки или сцены')
                title, what = m2.group(1), m2.group(2)
                delay = int(m2.group(3)) if m2.group(3) else 8

                if what in self.scenes:
                    # Кадров тут заведомо не больше четырёх: обход папок
                    # обрезает их раньше, при разборе art/scene/.
                    frames = list(self.scenes[what])
                else:
                    frames = [self.asset(what)]
                    delay = 0

                key = (title, what)
                if key in self.gallery_seen:
                    idx = self.gallery_seen[key]
                else:
                    idx = len(self.gallery)
                    if idx > 1023:
                        raise SystemExit("gallery: больше 1024 сцен не влезет "
                                         "в признаки на карте памяти")
                    self.gallery.append((frames, delay, title))
                    self.gallery_seen[key] = idx

                self.code += bytes([OPS["GALLERY"]]) + struct.pack("<H", idx)
            elif cmd == "chapter":
                # chapter "Название" — точка сохранения.
                #
                # Глава ОБЯЗАНА выстроить сцену с нуля: затемнение, фон,
                # спрайты. Тогда для загрузки достаточно номера главы и
                # флагов сюжета — снимок экрана не нужен.
                #
                # Номер главы берётся из хеша названия, а не из порядка:
                # так вставка новой главы в середину не ломает старые
                # сохранения.
                title = arg.strip().strip('"')
                if not title:
                    raise SystemExit("chapter без названия")
                h = 2166136261
                for ch in title.encode("utf-8"):
                    h = ((h ^ ch) * 16777619) & 0xFFFFFFFF
                if any(c["hash"] == h for c in self.chapters):
                    raise SystemExit(f"две главы с одинаковым названием: {title}")
                self.chapters.append(dict(hash=h, title=title,
                                          at=len(self.code)))
                self.force_cuts.append(len(self.code))
                # Опкод отмечает главу в потоке: движок по нему узнаёт,
                # где находится, и это значение кладётся в сохранение.
                self.code += bytes([OPS["CHAPTER"]]) + struct.pack("<I", h)
                self._chapter_open = title
            elif cmd == "voice":
                # voice <имя из voice.txt> — реплика из общего файла XA.
                # Номер двухбайтовый: озвучки планируется много.
                if arg not in self.voices:
                    raise SystemExit(f"неизвестная реплика '{arg}' "
                                     f"(нужна строка в voice.txt)")
                self.code += bytes([OPS["VOICE"]]) + \
                    struct.pack("<H", self.voices[arg])
            elif cmd == "se":
                # se <имя из instruments.txt> либо se <номер сэмпла>
                a = arg.strip()
                if a in self.sfx:
                    idx = self.sfx[a]
                elif a.isdigit():
                    idx = int(a)
                else:
                    raise SystemExit(
                        f"неизвестный звук '{a}' — нужна строка "
                        f"«sfx {a} <сэмпл>» в instruments.txt")
                if not 0 <= idx <= 255:
                    raise SystemExit(f"se {a}: номер сэмпла {idx} вне 0..255")
                self.code += bytes([OPS["SE"], idx])
            elif cmd in ("set", "if"):
                p = arg.split()

                # Номер флага и значение занимают по байту в опкоде.
                # Без явной проверки выход за предел давал невнятную
                # ошибку про диапазон байта, а не про флаг.
                def flag_num(v, what):
                    n = int(v)
                    if not 0 <= n <= 255:
                        raise SystemExit(
                            f"{cmd}: {what} {n} вне диапазона 0..255")
                    return n

                fl = flag_num(p[0], "номер флага")
                val = flag_num(p[1], "значение")

                if cmd == "set":
                    self.code += bytes([OPS["SET"], fl, val])
                else:                    # if <флаг> <знач> goto <метка>
                    self.code += bytes([OPS["IFJMP"], fl, val])
                    self.label_ref(p[3])
            elif cmd == "goto":
                self.code += bytes([OPS["JUMP"]])
                self.label_ref(arg)
            elif cmd == "end":
                self.code += bytes([OPS["END"]])
            elif cmd == "choice":
                opts = []
                while i < len(lines):
                    o = lines[i].strip()
                    i += 1
                    if o == "" or o.startswith("#"):
                        continue
                    if o.lower() == "endchoice":
                        break
                    mm = re.match(r'^"(.*)"\s*->\s*(\w+)$', o)
                    if not mm:
                        raise SystemExit(f"плохая строка выбора: {o}")
                    opts.append((mm.group(1), mm.group(2)))
                if not 1 <= len(opts) <= 4:
                    raise SystemExit("choice: нужно от 1 до 4 вариантов")
                self.code += bytes([OPS["CHOICE"], len(opts)])
                for txt, lbl in opts:
                    self.code += struct.pack("<I", self.s(txt))
                    # В выборе особых слов НЕТ: load и gallery здесь —
                    # обычные метки. Особые цели понимает только меню, и
                    # если пустить их сюда, движок прыгнет по
                    # несуществующему адресу.
                    self.label_ref(lbl)
            elif cmd == "splash":
                # splash <секунды>
                #   строка 1
                #   строка 2
                # endsplash
                secs = float(arg) if arg else 3.0
                body = []
                while i < len(lines):
                    o = lines[i]
                    i += 1
                    if o.strip().lower() == "endsplash":
                        break
                    body.append(o.rstrip())
                # убираем возможные пустые хвостовые строки
                while body and body[0].strip() == "":
                    body.pop(0)
                while body and body[-1].strip() == "":
                    body.pop()
                text = "\n".join(body)
                hold = max(1, min(0xFFFF, round(secs * 60)))   # секунды -> кадры NTSC
                self.code += bytes([OPS["SPLASH"]]) + \
                    struct.pack("<I", self.s(text)) + struct.pack("<H", hold)
            elif cmd == "menu":
                # menu "Заголовок"
                #   "СТАРТ" -> game_start
                # endmenu
                mt = re.match(r'^"(.*)"$', arg)
                title = mt.group(1) if mt else arg
                opts = []
                while i < len(lines):
                    o = lines[i].strip()
                    i += 1
                    if o == "" or o.startswith("#"):
                        continue
                    if o.lower() == "endmenu":
                        break
                    mm = re.match(r'^"(.*)"\s*->\s*(\w+)$', o)
                    if not mm:
                        raise SystemExit(f"плохая строка меню: {o}")
                    opts.append((mm.group(1), mm.group(2)))
                if not 1 <= len(opts) <= 4:
                    raise SystemExit("menu: нужно от 1 до 4 пунктов")
                self.code += bytes([OPS["MENU"]]) + struct.pack("<I", self.s(title)) + \
                    bytes([len(opts)])
                for txt, lbl in opts:
                    self.code += struct.pack("<I", self.s(txt))
                    if lbl == "gallery":
                        # Второе особое слово: открыть галерею.
                        self.code += struct.pack("<I", 0xFFFFFFFE)
                    elif lbl == "load":
                        # Особая цель: вместо перехода по метке движок
                        # откроет экран загрузки. Адреса 0xFFFFFFFF в
                        # сценарии быть не может — старший байт хранит
                        # номер главы, а их не больше сотни.
                        self.code += struct.pack("<I", 0xFFFFFFFF)
                    else:
                        self.label_ref(lbl)
            else:
                # всё, что не команда — реплика
                # Глава обязана выстроить сцену до первой реплики: при
                # загрузке движок начнёт ровно отсюда, и без фона игрок
                # увидит чёрный экран с текстом.
                if getattr(self, "_chapter_open", None):
                    raise SystemExit(
                        f"глава '{self._chapter_open}': реплика раньше, чем "
                        f"bg или scene. Глава должна выстраивать сцену с нуля.")

                pages = self.wrap_pages(line)
                if len(pages) > 1:
                    print(f"  реплика разбита на {len(pages)} страницы: "
                          f"{line[:40]}...")
                for page in pages:
                    self.code += bytes([OPS["TEXT"]]) + \
                        struct.pack("<I", self.s(page))

        for _, name in self.fixups:
            if name not in self.labels:
                raise SystemExit(f"неизвестная метка '{name}'")
        return self.link()

    # ------------------------------------------------------------------
    # Линковка: режем код на чанки и превращаем адреса в (чанк, смещение)
    # ------------------------------------------------------------------

    # Длины опкодов с фиксированным размером
    _FIXED = {0: 1, 1: 3, 2: 8, 19: 3, 20: 5, 21: 6, 22: 3, 3: 5, 4: 5, 5: 1, 6: 3, 7: 3, 8: 2, 9: 2,
              11: 5, 12: 3, 13: 7, 14: 2, 15: 6, 16: 7}
    # Опкоды, после которых приятно рвать главу: экран и так гаснет/чистится
    _SEAM = {5, 7, 1, 15}          # CLEARBOX, FADE, BG, STR

    def _instr_size(self, off):
        op = self.code[off]
        if op == 8:                                   # BGM: subcommand-dependent
            sub = self.code[off + 1]
            return {0: 4, 1: 2, 2: 4, 3: 4, 4: 3}.get(sub, 2)
        if op == 10:                                  # CHOICE
            return 2 + 8 * self.code[off + 1]
        if op == 17:                                  # MENU
            return 6 + 8 * self.code[off + 5]
        if op == 18:                          # SCENE: 3 + по 2 байта на кадр
            return 3 + self.code[off + 1] * 2
        if op not in self._FIXED:
            raise SystemExit(f"линковка: неизвестный опкод {op} на {off}")
        return self._FIXED[op]

    def _str_operands(self, off):
        """Позиции 4-байтовых операндов-строк внутри инструкции."""
        op = self.code[off]
        if op in (3, 4, 16):                          # NAME, TEXT, SPLASH
            return [off + 1]
        if op == 10:                                  # CHOICE
            return [off + 2 + 8 * k for k in range(self.code[off + 1])]
        if op == 17:                                  # MENU
            return [off + 1] + [off + 6 + 8 * k
                                for k in range(self.code[off + 5])]
        return []

    def _boundaries(self):
        """Список смещений начала каждой инструкции + общий размер."""
        out, off = [], 0
        while off < len(self.code):
            out.append(off)
            off += self._instr_size(off)
        if off != len(self.code):
            raise SystemExit("линковка: код разъехался по границам инструкций")
        return out

    def _split_points(self, bounds):
        """Границы чанков.

        Считаем и код, и пул строк: в новелле текст занимает почти весь объём,
        так что резать по одному коду бессмысленно. Строка, использованная в
        двух главах, дублируется в обеих — это дешевле, чем общий пул.
        Рвём по возможности на «шве» (после fade/clear/bg/str), где на экране
        и так пауза и загрузка незаметна.
        """
        rev = {off: txt for txt, off in self.pool.items()}
        index_of = {off: i for i, off in enumerate(bounds)}

        cuts = [0]
        forced = sorted(set(self.force_cuts))
        start = 0
        used, str_bytes, code_bytes = set(), 1, 0
        seam = None
        i = 0

        while i < len(bounds):
            off = bounds[i]
            isz = self._instr_size(off)

            fresh, add = [], 0
            for sp in self._str_operands(off):
                g = struct.unpack("<I", self.code[sp:sp + 4])[0]
                t = rev[g]
                if t not in used and t not in fresh:
                    fresh.append(t)
                    add += len(t.encode("utf-8")) + 1

            total = 24 + code_bytes + str_bytes + isz + add
            if code_bytes and total > self.chunk_size:
                cut = bounds[seam] if seam is not None and bounds[seam] > start else off
                cuts.append(cut)
                start = cut
                used, str_bytes, code_bytes = set(), 1, 0
                seam = None
                i = index_of[cut]
                continue

            code_bytes += isz
            str_bytes += add
            used.update(fresh)
            # Метка главы рвёт чанк независимо от размера: глава должна
            # начинаться ровно с начала файла, иначе загрузка по номеру
            # главы попадёт в середину сцены.
            if off in forced and off != cuts[-1]:
                cuts.append(off)
                start = off
                used, str_bytes, code_bytes = set(), 1, 0
                seam = None
                i += 1
                continue

            if self.code[off] in self._SEAM and i + 1 < len(bounds):
                seam = i + 1
            i += 1

        cuts.append(len(self.code))
        return cuts

    def link(self):
        bounds = self._boundaries()
        cuts = self._split_points(bounds)
        nchunks = len(cuts) - 1
        if nchunks > 100:
            raise SystemExit(f"слишком много чанков ({nchunks}), поднимите --chunk-size")

        # глобальное смещение -> (чанк, локальное смещение)
        def locate(g):
            for c in range(nchunks):
                if cuts[c] <= g < cuts[c + 1]:
                    return c, g - cuts[c]
            return nchunks - 1, g - cuts[nchunks - 1]

        # Таблица глав: по хешу названия -> где начинается. Сохранение
        # хранит только хеш, поэтому перестановка глав в сценарии старые
        # сохранения не ломает.
        self.chapter_table = []
        for c in self.chapters:
            ch, off = locate(c["at"])
            if off != 0:
                raise SystemExit(
                    f"глава '{c['title']}' начинается не с начала чанка "
                    f"(смещение {off}) — принудительный разрыв не сработал")
            self.chapter_table.append((c["hash"], ch, c["title"]))

        rev_pool = {off: txt for txt, off in self.pool.items()}

        blobs = []
        for c in range(nchunks):
            lo, hi = cuts[c], cuts[c + 1]
            body = bytearray(self.code[lo:hi])

            # свой пул строк на чанк: строка кладётся туда, где её используют
            pool, strs = {"": 0}, bytearray(b"\x00")

            def local_str(txt):
                if txt in pool:
                    return pool[txt]
                off = len(strs)
                strs.extend(txt.encode("utf-8") + b"\x00")
                pool[txt] = off
                return off

            off = lo
            while off < hi:
                for sp in self._str_operands(off):
                    g = struct.unpack("<I", self.code[sp:sp + 4])[0]
                    if g not in rev_pool:
                        raise SystemExit(f"линковка: битая ссылка на строку {g}")
                    body[sp - lo:sp - lo + 4] = struct.pack("<I", local_str(rev_pool[g]))
                off += self._instr_size(off)

            # адреса переходов: старший байт — номер чанка
            for pos, name in self.fixups:
                if lo <= pos < hi:
                    tc, toff = locate(self.labels[name])
                    if toff > 0xFFFFFF:
                        raise SystemExit("линковка: чанк больше 16 МБ")
                    body[pos - lo:pos - lo + 4] = struct.pack("<I", (tc << 24) | toff)

            header = 24
            strs = scramble(bytes(strs), c)      # пул строк наружу не отдаём
            blobs.append(b"VNS3" +
                         struct.pack("<IIII", header, len(body),
                                     header + len(body), len(strs)) +
                         struct.pack("<HH", c, nchunks) +
                         bytes(body) + bytes(strs))
        return blobs


def update_iso_xml(video_order=None):
    """Переписывает список файлов в iso.xml между маркерами.

    Раньше каждый новый ассет приходилось дописывать руками, и при этом
    номера A000.TIM разъезжались при вставке картинки в середину списка.
    Теперь блок генерируется по фактическому содержимому data/ и video/.
    """
    path = os.path.join(ROOT, "iso.xml")
    if not os.path.exists(path):
        return
    src = open(path, encoding="utf-8").read()
    b, e = "<!-- AUTO-DATA-BEGIN", "<!-- AUTO-DATA-END -->"
    if b not in src or e not in src:
        print("  ! в iso.xml нет маркеров AUTO-DATA — список не обновлён")
        return

    lines = []

    def add(name, kind, rel, depth):
        pad = "\t" * (4 + depth)
        lines.append(f'{pad}<file name="{name}" type="{kind}" '
                     f'source="${{PROJECT_SOURCE_DIR}}/{rel}" />')

    def open_dir(name, depth):
        lines.append("\t" * (4 + depth) + f'<dir name="{name}">')

    def close_dir(depth):
        lines.append("\t" * (4 + depth) + "</dir>")

    # Каждый вид данных — в своём каталоге. Плоский DATA на сотни файлов
    # неудобен, а некоторые инструменты работы с образами на нём тормозят.
    # Общий файл картинок лежит прямо в DATA, рядом с каталогами.
    pak = os.path.join(OUT, "ART.PAK")
    if os.path.exists(pak):
        add("ART.PAK", "data", "data/ART.PAK", 0)

    # Озвучка укладывается типом "xa": секторы Form 2, как у видео.
    if os.path.exists(os.path.join(OUT, "VOICE.XAS")):
        add("VOICE.XAS", "xa", "data/VOICE.XAS", 0)
        add("VOICE.IDX", "data", "data/VOICE.IDX", 0)

    for sub in SUBDIRS:
        if sub == DIR_STR:          # видео берётся из video/, не из data/
            continue
        if sub == DIR_ART and os.path.exists(pak):
            # Картинки уже лежат в общем файле. Отдельные TIM остаются на
            # диске компьютера для отладки, но в образ не идут — иначе они
            # занимали бы место дважды.
            continue
        d = os.path.join(OUT, sub)
        if not os.path.isdir(d):
            continue
        files = sorted(os.listdir(d))
        if not files:
            continue
        open_dir(sub, 0)
        for f in files:
            add(f, "data", f"data/{sub}/{f}", 1)
        close_dir(0)

    # Видео лежит отдельно от data/ и нумеруется ТОЛЬКО по videos.txt:
    # обход каталога по алфавиту когда-то разошёлся с номерами компилятора,
    # и сценарий просил один ролик, а проигрывался другой.
    if video_order:
        open_dir(DIR_STR, 0)
        for n, rel in enumerate(video_order):
            # STR укладывается особым типом: mkpsxiso пересоберёт EDC/ECC
            add(f"V{n:03d}.STR", "str", rel.replace("\\", "/"), 1)
        close_dir(0)

    head = src[:src.index(b)]
    tail = src[src.index(e) + len(e):]

    # Метка тома берётся из game.txt: разные игры на движке не должны
    # выходить с одинаковой меткой.
    info = read_game_info()
    head = re.sub(r'volume\s*=\s*"[^"]*"', f'volume      = "{info["volume"]}"', head)

    # Файл лицензии Sony (licensea.dat) в репозитории не распространяется.
    # Без него образ работает в эмуляторах и на консолях с модчипом или
    # ODE; для штатной загрузки на немодифицированной приставке его нужно
    # положить в корень проекта самому.
    lic = os.path.exists(os.path.join(ROOT, "licensea.dat"))
    on = '<license file="${PROJECT_SOURCE_DIR}/licensea.dat" />'
    off = '<!-- <license file="${PROJECT_SOURCE_DIR}/licensea.dat" /> -->'
    if lic:
        head = head.replace(off, on)
    else:
        if on in head and off not in head:
            head = head.replace(on, off)
        print("  ! нет licensea.dat: образ без лицензионного блока "
              "(эмуляторы и консоли с модчипом/ODE)")
    block = (b + ": список генерируется build_data.py.\n"
             "\t\t\t\t     Руками не править — правки затрутся. -->\n"
             + "\n".join(lines) + "\n\t\t\t\t" + e)
    open(path, "w", encoding="utf-8").write(head + block + tail)
    print(f"iso.xml: перечислено файлов {len(lines)}")


def read_game_info():
    """game.txt: название и имя файла на карте.

    Единственный источник этих значений. Раньше название лежало в сборке,
    а имя файла — тремя копиями в исходниках движка; смена игры требовала
    править их все и легко приводила к рассинхрону.
    """
    info = dict(title="VN GAME", savefile="BASLUS-VN000SAVE", volume="VNGAME")
    path = os.path.join(ROOT, "game.txt")
    if not os.path.exists(path):
        return info
    for line in open(path, encoding="utf-8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        key, _, val = line.partition(" ")
        val = val.strip()
        if key in info and val:
            info[key] = val

    # Имя файла на карте — только латиница и не длиннее 19 символов:
    # так требует файловая система карты, а кириллица там превратится
    # в мусор, и сохранение просто не найдётся.
    vol = info["volume"].upper()
    if not re.fullmatch(r"[A-Z0-9_]{1,32}", vol):
        raise SystemExit(f"game.txt: volume «{info['volume']}» — только "
                         f"латиница, цифры и подчёркивание, до 32 символов")
    info["volume"] = vol

    fn = info["savefile"]
    if len(fn) > 19:
        raise SystemExit(f"game.txt: savefile «{fn}» длиннее 19 символов")
    try:
        fn.encode("ascii")
    except UnicodeEncodeError:
        raise SystemExit(f"game.txt: savefile «{fn}» — нужна латиница")

    return info


def build_save_icon():
    """Служебный заголовок сохранения с иконкой — его читает менеджер BIOS."""
    import subprocess
    # Иконка: save_icon.png или save_icon.ico в корне проекта, любой
    # размер — конвертер сам ужмёт до 16x16 в шестнадцати цветах.
    src = None
    for cand in ("save_icon.png", "save_icon.ico"):
        if os.path.exists(os.path.join(ROOT, cand)):
            src = os.path.join(ROOT, cand)
            break
    if not src:
        print("  ! нет save_icon.png — сохранение будет без иконки")
        return
    info = read_game_info()
    out = out_path(DIR_SYS, "SAVEHDR.BIN")
    r = subprocess.run(
        [sys.executable, os.path.join(ROOT, "tools", "mksaveicon.py"),
         src, out, "--title", info["title"], "--savefile", info["savefile"]],
        capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"mksaveicon упал:\n{r.stderr}")
    print("SAVEHDR.BIN " + r.stdout.strip().split(": ", 1)[-1])


def _write_gallery(comp):
    """Таблица галереи: SCR/GAL.BIN.

    Запись 52 байта: до четырёх кадров (по 2), число кадров (1),
    задержка (1), резерв (2), название (40).

    Кадров несколько, потому что в галерее показываются и анимированные
    сцены, а не только статичные фоны.

    Сорок байт, а не двадцать четыре, потому что кириллица занимает по два
    байта на символ: в 24 байта влезало всего одиннадцать букв, и названия
    обрезались на полуслове. Сами признаки
    «открыто» лежат не здесь, а на карте памяти — по биту на сцену.
    """
    table = getattr(comp, "gallery", [])
    if not table:
        return
    out = bytearray(b"GALL")
    out += struct.pack("<HH", len(table), 0)
    for frames, delay, title in table:
        # Обрезаем по границе символа, иначе последний окажется разорван
        # пополам и превратится в мусор.
        name = title
        while len(name.encode("utf-8")) > 39:
            name = name[:-1]
        if name != title:
            print(f"  ! галерея: название «{title}» обрезано до «{name}»")
        name = name.encode("utf-8")
        fr = list(frames[:SCENE_MAX]) + [0] * (SCENE_MAX - len(frames[:SCENE_MAX]))
        out += struct.pack("<HHHH", *fr)
        out += bytes([len(frames[:SCENE_MAX]), delay & 255, 0, 0])
        out += name + bytes(40 - len(name))
    open(out_path(DIR_SCR, "GAL.BIN"), "wb").write(bytes(out))
    print(f"GAL.BIN   сцен галереи {len(table)}: " +
          ", ".join(t for _, _, t in table[:3]) +
          (" ..." if len(table) > 3 else ""))


def _write_chapters(comp):
    """Таблица глав: SCR/CHAPS.BIN.

    Запись — 32 байта: хеш названия, номер чанка и само название для
    экрана сохранений. Сохранение ссылается на главу ХЕШЕМ, поэтому
    вставка новой главы в середину сценария не ломает старые файлы.
    """
    table = getattr(comp, "chapter_table", [])
    if not table:
        return
    out = bytearray(b"CHAP")
    out += struct.pack("<HH", len(table), 0)
    for h, chunk, title in table:
        name = title.encode("utf-8")[:23]
        out += struct.pack("<IHH", h, chunk, 0) + name + bytes(24 - len(name))
    open(out_path(DIR_SCR, "CHAPS.BIN"), "wb").write(bytes(out))
    print(f"CHAPS.BIN глав {len(table)}: " +
          ", ".join(t for _, _, t in table[:4]) +
          (" ..." if len(table) > 4 else ""))


def check_buffers():
    """Сверяет размеры готовых файлов с буферами в C.

    Эта проверка появилась после того, как увеличение ячейки шрифта
    с 16 до 18 px раздуло FONT.TIM за пределы буфера в main.c — и игра
    молча уходила в чёрный экран. Класс ошибки неприятен тем, что данные
    и код правятся в разных файлах, а ломается только на железе.
    Значения ниже обязаны совпадать с константами в src/.
    """
    limits = [
        (DIR_SYS, "FONT.TIM", 40 * 1024, "font_tim[] в src/main.c"),
        (DIR_SCR, "CHAPS.BIN", 2 * 1024, "chap_raw[] в src/save.c"),
        (DIR_SCR, "GAL.BIN", 8 * 1024, "gal_raw[] в src/gallery.c"),
        (DIR_SYS, "SAVEHDR.BIN", 2 * 1024, "hdr_icon[] в src/card.c"),
        (DIR_SYS, "FONT.WID", 2 * 1024, "font_widths[] в src/main.c"),
        (DIR_BGM, "BANK.VAB", 288 * 1024, "vab_buf[] в src/seq.c"),
    ]
    for sub, pattern, cap, where in (
            (DIR_SCR, r"CH\d\d\.VNS", 96 * 1024, "SCRIPT_MAX в src/main.c"),
            (DIR_BGM, r"M\d\d\d\.SEQ", 128 * 1024, "seq_buf[] в src/seq.c"),
            (DIR_ART, r"A\d+\.TIM", 128 * 1024, "TMP_SIZE в src/cd.c")):
        d = os.path.join(OUT, sub)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if re.fullmatch(pattern, f):
                limits.append((sub, f, cap, where))

    # Указатель озвучки лежит в корне DATA
    vidx = os.path.join(OUT, "VOICE.IDX")
    if os.path.exists(vidx):
        limits.append(("", "VOICE.IDX", 96 * 1024, "voice_idx[] в src/voice.c"))

    problems = []
    for sub, name, cap, where in limits:
        path = os.path.join(OUT, sub, name) if sub else os.path.join(OUT, name)
        if not os.path.exists(path):
            continue
        size = os.path.getsize(path)
        need = ((size + 2047) // 2048) * 2048     # CdRead пишет секторами
        if cap < 2048:
            problems.append(
                f"  {name}: буфер {where} = {cap} б, меньше сектора. "
                f"CdRead пишет целыми секторами по 2048 и откажется читать.")
            continue

        if need > cap:
            problems.append(f"  {name}: {size} б (с округлением до секторов "
                            f"{need}) > буфера {cap} — поднимите {where}")

    if problems:
        print("\n!!! ФАЙЛЫ НЕ ВЛЕЗУТ В БУФЕРЫ — на консоли будет чёрный экран:")
        print("\n".join(problems))
        raise SystemExit(1)


def check_scene_limit():
    """Сверяет SCENE_MAX с движком: значения в двух местах легко разъезжаются,
    а последствие — молча пропавшие кадры анимации."""
    h = os.path.join(ROOT, "src", "vn.h")
    if not os.path.exists(h):
        return
    m = re.search(r"#define\s+SCENE_FRAMES\s+(\d+)", open(h, encoding="utf-8").read())
    if m and int(m.group(1)) != SCENE_MAX:
        raise SystemExit(
            f"SCENE_MAX={SCENE_MAX} в сборке, а SCENE_FRAMES={m.group(1)} "
            f"в src/vn.h — приведите к одному значению")


RELEASE = False


def main():
    ap = argparse.ArgumentParser(description="сборка данных для VN-движка")
    ap.add_argument("--release", action="store_true",
                    help="без отладочных данных: NAMES.DBG в образ не идёт")
    ap.add_argument("--chunk-size", type=int, default=CHUNK_DEFAULT,
                    help=f"целевой размер главы в байтах (по умолч. {CHUNK_DEFAULT})")
    args = ap.parse_args()

    global RELEASE
    RELEASE = args.release

    os.makedirs(OUT, exist_ok=True)
    widths = build_font()
    check_scene_limit()
    assets, scenes = discover_assets()
    videos, video_order = build_videos()
    music = build_music()
    sfx = build_bank()
    build_save_icon()

    # Убираем главы прошлой сборки: их могло стать меньше, и лишние
    # продолжали бы попадать в образ.
    scr = os.path.join(OUT, DIR_SCR)
    if os.path.isdir(scr):
        for f in os.listdir(scr):
            if re.fullmatch(r"CH\d\d\.VNS", f):
                os.remove(os.path.join(scr, f))

    voices = build_voice()

    comp = Compiler(assets, widths, videos, args.chunk_size, scenes, music,
                    sfx=sfx,
                    voices=voices)
    blobs = comp.compile(
        os.path.join(ROOT, "script", "main.vns"))

    total = 0
    _write_chapters(comp)
    _write_gallery(comp)

    for i, blob in enumerate(blobs):
        name = f"CH{i:02d}.VNS"
        open(out_path(DIR_SCR, name), "wb").write(blob)
        total += len(blob)
        print(f"{name}  {len(blob)} байт")
    print(f"сценарий: {len(blobs)} глав(ы), {total} байт всего")

    update_iso_xml(video_order)
    check_buffers()



if __name__ == "__main__":
    main()
