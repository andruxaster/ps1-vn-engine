#!/usr/bin/env python3
"""Конвертация видео в STR-заставку через psxavenc.

  python3 tools/encode_video.py intro video/intro_src.avi
  python3 tools/encode_video.py intro video/intro_src.mp4 --fps 15 --size 320x224

Формат исходника значения не имеет: psxavenc использует libav от FFmpeg и
читает любой контейнер, который понимает ffmpeg — AVI, MP4, MOV, MKV и т.д.
Это ваш AVI пойдёт напрямую, перекодировать во что-то промежуточное не нужно.

Что делает:
  1. Проверяет, что psxavenc установлен.
  2. Валидирует параметры под ограничения PS1/MDEC (кратность 16 и т.п.).
  3. Гонит psxavenc -t str -v v2 (видео BS v2 + XA-звук, сектора 2336 байт).
  4. Кладёт результат в video/<имя>.str и регистрирует его в videos.txt.

Почему именно эти флаги:
  -t str     сектора 2336 байт (подзаголовок XA + данные, без sync/header).
             Именно этот размер принимает mkpsxiso: он сам допишет sync,
             header и пересоберёт EDC/ECC под фактическое место на диске.
             Вариант strcd даёт 2352 байта (полный сырой CD-сектор) — он
             для прямой записи болванки, mkpsxiso его отвергает с ошибкой
             "not a multiple of 2336 or 2048 bytes".
  -v v2      битстрим BS версии 2 — единственный, который надёжно понимает
             DecDCTvlc из PSn00bSDK (v3/v3dc там не декодируются).
  -f 37800   частота XA-звука для 2x CD (37800 Гц — стандарт для стерео 4-бит).
  -x 2       поток рассчитан на скорость привода 2x.

Сам STR-плеер (src/str.c) читает 2048-байтные ПОЛЬЗОВАТЕЛЬСКИЕ данные,
которые отдаёт CdGetSector в режиме RT — XA-звук фильтруется железом,
поэтому со стороны движка ничего дополнительно делать не нужно.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VIDEO_DIR = os.path.join(ROOT, "video")
VIDEOS_TXT = os.path.join(ROOT, "videos.txt")

INSTALL_HINT = """\
psxavenc не найден в PATH.

Установка (Linux):
    git clone https://github.com/WonderfulToolchain/psxavenc
    cd psxavenc
    meson setup build            # нужны libav* от FFmpeg и meson
    meson compile -C build
    sudo meson install -C build

После установки повторите эту команду. Если psxavenc лежит не в PATH,
укажите путь через --psxavenc /путь/к/psxavenc.
"""


def die(msg):
    print("ошибка:", msg, file=sys.stderr)
    sys.exit(1)


def probe(path):
    """Возвращает (width, height, fps, has_audio) через ffprobe."""
    if not shutil.which("ffprobe"):
        return None
    try:
        out = subprocess.check_output(
            ["ffprobe", "-v", "error", "-show_entries",
             "stream=codec_type,width,height,avg_frame_rate",
             "-of", "default=noprint_wrappers=1", path],
            text=True)
    except subprocess.CalledProcessError:
        return None

    w = h = None
    fps = None
    has_audio = False
    cur_type = None
    for line in out.splitlines():
        k, _, v = line.partition("=")
        if k == "codec_type":
            cur_type = v
            if v == "audio":
                has_audio = True
        elif k == "width" and v.isdigit():
            w = int(v)
        elif k == "height" and v.isdigit():
            h = int(v)
        elif k == "avg_frame_rate" and cur_type == "video" and "/" in v:
            num, den = v.split("/")
            if den != "0":
                fps = round(int(num) / int(den), 2)
    return w, h, fps, has_audio


def register(name, rel_path):
    """Дописывает/обновляет строку в videos.txt."""
    lines = []
    found = False
    if os.path.exists(VIDEOS_TXT):
        for line in open(VIDEOS_TXT, encoding="utf-8"):
            stripped = line.split("#")[0].strip()
            if stripped and stripped.split()[0] == name:
                lines.append(f"{name}  {rel_path}\n")
                found = True
            else:
                lines.append(line if line.endswith("\n") else line + "\n")
    if not found:
        lines.append(f"{name}  {rel_path}\n")
    open(VIDEOS_TXT, "w", encoding="utf-8").write("".join(lines))


def main():
    ap = argparse.ArgumentParser(description="видео -> STR через psxavenc")
    ap.add_argument("name", help="имя видео (для videos.txt и сценария)")
    ap.add_argument("source", help="исходный файл видео")
    ap.add_argument("--fps", type=int, default=15, help="кадров/с (по умолч. 15)")
    ap.add_argument("--size", default="320x224", help="WxH, кратно 16 (320x224)")
    ap.add_argument("--mono", action="store_true", help="моно-звук вместо стерео")
    ap.add_argument("--psxavenc", default="psxavenc", help="путь к psxavenc")
    args = ap.parse_args()

    exe = shutil.which(args.psxavenc) or (
        args.psxavenc if os.path.isfile(args.psxavenc) else None)
    if not exe:
        print(INSTALL_HINT, file=sys.stderr)
        sys.exit(2)

    if not os.path.isfile(args.source):
        die(f"нет входного файла {args.source}")

    m = re.fullmatch(r"(\d+)x(\d+)", args.size)
    if not m:
        die("--size должен быть в форме WxH, например 320x224")
    w, h = int(m.group(1)), int(m.group(2))
    if w % 16 or h % 16:
        die(f"размер {w}x{h} не кратен 16 — MDEC работает макроблоками 16x16")
    if w > 320 or h > 240:
        die(f"размер {w}x{h} больше 320x240 — не влезет в кадровый буфер PS1")
    if not 1 <= args.fps <= 30:
        die("fps вне разумного диапазона 1..30")

    # Информативная сверка с реальными параметрами файла (не блокирующая).
    info = probe(args.source)
    if info:
        sw, sh, sfps, has_audio = info
        if sw and sh and (sw, sh) != (w, h):
            print(f"  инфо: исходник {sw}x{sh}, будет масштабирован в {w}x{h}")
        if not has_audio:
            print("  инфо: в исходнике нет аудио — STR будет без звука")

    os.makedirs(VIDEO_DIR, exist_ok=True)
    out_path = os.path.join(VIDEO_DIR, f"{args.name}.str")
    channels = 1 if args.mono else 2

    cmd = [
        exe,
        "-t", "str",            # 2336-байтные сектора: mkpsxiso сам допишет
                                # sync/header и пересоберёт EDC/ECC
        "-v", "v2",             # BS v2 — совместимо с DecDCTvlc в PSn00bSDK
        "-f", "37800",          # частота XA-звука
        "-b", "4",              # 4 бита на XA-сэмпл
        "-c", str(channels),
        "-s", f"{w}x{h}",
        "-r", str(args.fps),
        "-x", "2",              # поток для привода 2x
        args.source,
        out_path,
    ]
    print("psxavenc:", " ".join(cmd))
    try:
        subprocess.run(cmd, check=True)
    except subprocess.CalledProcessError as e:
        die(f"psxavenc вернул код {e.returncode}")

    size = os.path.getsize(out_path)
    sectors = size // 2336
    secs = sectors / (75 * 2)     # 75 секторов/с * 2x
    rel = os.path.relpath(out_path, ROOT)
    register(args.name, rel)

    print(f"\nготово: {rel}")
    print(f"  {size} байт, ~{sectors} секторов, ~{secs:.1f}s при 2x")
    print(f"  зарегистрировано в videos.txt как '{args.name}'")
    print(f"\nДальше:")
    print(f"  1. в script/main.vns:  str {args.name}")
    print(f"  2. в iso.xml добавьте:  "
          f'<file name="V000.STR" type="str" '
          f'source="${{PROJECT_SOURCE_DIR}}/{rel}" />')
    print(f"  3. python3 tools/build_data.py  (пересчитает кадры)")


if __name__ == "__main__":
    main()
