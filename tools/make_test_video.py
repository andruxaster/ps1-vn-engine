#!/usr/bin/env python3
"""Тестовый ролик для проверки STR на железе: video/test.str

  python3 tools/make_test_video.py [--seconds 10] [--psxavenc путь]

Картинка собрана так, чтобы по фотографии экрана было видно, что пошло
не так:

  - крупный номер кадра и время: пропуски кадров видны сразу;
  - полоса, бегущая слева направо за секунду: рывки и разрывы кадра;
  - цветные полосы сверху: правильность цветов MDEC;
  - фон демо медленно наезжает: настоящая картинка, а не только заливка.

Звук: короткий сигнал в начале каждой секунды, в левом канале на чётных
секундах и в правом на нечётных — так слышно и синхронность, и стерео.

Нужны ffmpeg (с drawtext) и psxavenc; дальше работает
tools/encode_video.py с обычными параметрами движка.
"""

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.environ.get("VN_FONT",
                      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--psxavenc", default="psxavenc")
    args = ap.parse_args()

    if not os.path.isfile(FONT):
        sys.exit(f"нет шрифта {FONT}; укажите VN_FONT=/путь/к/шрифту.ttf")

    secs = args.seconds
    fps = 15
    bg = os.path.join(ROOT, "art", "bg", "meadow.png")
    os.makedirs(os.path.join(ROOT, "video"), exist_ok=True)
    src = os.path.join(ROOT, "video", "test_src.mp4")

    # Двоеточия в пути шрифта для drawtext надо экранировать.
    font = FONT.replace(":", r"\:")
    vf = ",".join([
        # медленный наезд на фон
        f"scale=640:480,zoompan=z='1+0.3*on/{secs * fps}':d=1:"
        f"x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':s=320x224:fps={fps}",
        # цветные полосы сверху
        "drawbox=x=0:y=0:w=40:h=16:color=white:t=fill",
        "drawbox=x=40:y=0:w=40:h=16:color=yellow:t=fill",
        "drawbox=x=80:y=0:w=40:h=16:color=cyan:t=fill",
        "drawbox=x=120:y=0:w=40:h=16:color=lime:t=fill",
        "drawbox=x=160:y=0:w=40:h=16:color=magenta:t=fill",
        "drawbox=x=200:y=0:w=40:h=16:color=red:t=fill",
        "drawbox=x=240:y=0:w=40:h=16:color=blue:t=fill",
        "drawbox=x=280:y=0:w=40:h=16:color=black:t=fill",
        # полоса, пробегающая экран за секунду
        "drawbox=x='mod(t,1)*296':y=200:w=24:h=24:color=white:t=fill",
        # номер кадра и время
        f"drawtext=fontfile='{font}':text='%{{frame_num}}':"
        "x=(w-tw)/2:y=60:fontsize=72:fontcolor=white:borderw=4:bordercolor=black",
        f"drawtext=fontfile='{font}':text='%{{pts\\:hms}}':"
        "x=(w-tw)/2:y=150:fontsize=24:fontcolor=yellow:borderw=3:bordercolor=black",
        "drawtext=fontfile='{0}':text='STR TEST':x=8:y=24:fontsize=16:"
        "fontcolor=white:borderw=2:bordercolor=black".format(font),
    ])

    # Сигнал 0.1 с в начале каждой секунды: слева на чётных, справа на
    # нечётных.
    beep = "0.6*sin(2*PI*880*t)*lt(mod(t,1),0.1)"
    af = (f"aevalsrc='{beep}*eq(mod(floor(t),2),0)|"
          f"{beep}*eq(mod(floor(t),2),1)':s=44100:d={secs}")

    cmd = ["ffmpeg", "-y", "-loglevel", "warning",
           "-loop", "1", "-framerate", str(fps), "-t", str(secs), "-i", bg,
           "-f", "lavfi", "-i", af,
           "-vf", vf, "-t", str(secs),
           "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
           "-shortest", src]
    print("ffmpeg: исходник", os.path.relpath(src, ROOT))
    subprocess.run(cmd, check=True)

    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "encode_video.py"),
                    "test", src, "--fps", str(fps), "--size", "320x224",
                    "--psxavenc", args.psxavenc], check=True)
    os.remove(src)


if __name__ == "__main__":
    main()
