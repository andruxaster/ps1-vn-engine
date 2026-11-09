#!/usr/bin/env python3
"""Сборка общего файла озвучки (XAS) с чередованием каналов.

  python3 tools/mkxas.py voice.txt data/VOICE.XAS data/VOICE.IDX

Зачем чередование. Поток 18900 Гц моно съедает 4.69 сектора в секунду, а
привод на одинарной скорости читает 75. Значит голос может занимать лишь
каждый шестнадцатый сектор — иначе данные придут быстрее, чем SPU успевает
их проигрывать. Если положить одну реплику подряд, 94% места уйдёт впустую.

Поэтому в свободные слоты кладутся ДРУГИЕ реплики: каждый сектор XA несёт
номер канала, а фильтр привода выбирает нужный. Реплики собираются пачками
по числу каналов; чтобы проиграть реплику, движок встаёт на начало её пачки
и ставит фильтр на её канал.

Манифест voice.txt:
    # имя      файл
    sylvie_01  voice/sylvie_01.wav
    me_01      voice/me_01.wav

Формат указателя VOICE.IDX:
     0  "VIDX"
     4  u16 записей
     6  u16 каналов в пачке
     8  u16 кадров развёртки на сектор, формат 8.8
    10  u16 резерв
    12  по 8 байт на запись:
          u32 сектор начала пачки (от начала XAS)
          u8  канал
          u8  резерв
          u16 секторов в реплике

Запись занимает 8 байт, а не 12: длительность выводится из числа секторов,
хранить её отдельно незачем. На десяти тысячах реплик это экономит
треть указателя, а он целиком лежит в памяти приставки.

ВАЖНО: пачка занимает столько места, сколько самая длинная реплика в ней.
Поэтому реплики сортируются по длине — так короткие не теряют место рядом
с длинными.
"""

import argparse
import os
import struct
import subprocess

SECTOR = 2336          # XA-сектор без синхронизации, как выдаёт psxavenc -t xa
SUBHDR = 8             # подзаголовок: файл, канал, подрежим, кодирование (и копия)


def encode_one(src, tmp, rate, channel, psxavenc):
    """Кодирует одну реплику в свой канал."""
    cmd = [psxavenc, "-t", "xa", "-f", str(rate), "-b", "4", "-c", "1",
           "-F", "0", "-C", str(channel), src, tmp]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True)
    except FileNotFoundError:
        raise SystemExit(
            f"не найден psxavenc. Укажите путь: --psxavenc /путь/к/psxavenc")
    if r.returncode:
        raise SystemExit(f"psxavenc упал на {src}:\n{r.stderr}")
    return open(tmp, "rb").read()


def set_channel(sector, channel):
    """Проставляет номер канала в подзаголовке и его копии.

    Раскладка сверена с libpsxav: подзаголовок это файл, канал, подрежим,
    кодирование — и сразу следом точная копия тех же четырёх байт.
    """
    s = bytearray(sector)
    s[1] = channel & 31
    s[5] = channel & 31
    return bytes(s)


def main():
    ap = argparse.ArgumentParser(description="сборка общего файла озвучки")
    ap.add_argument("manifest")
    ap.add_argument("out_xas")
    ap.add_argument("out_idx")
    ap.add_argument("--rate", type=int, default=18900, choices=(18900, 37800))
    ap.add_argument("--speed", type=int, default=1, choices=(1, 2),
                    help="скорость привода при воспроизведении")
    ap.add_argument("--psxavenc", default="psxavenc")
    ap.add_argument("--root", default=None,
                    help="корень проекта (по умолчанию — на уровень выше tools/)")
    args = ap.parse_args()

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # Сколько каналов помещается: полоса привода делённая на аппетит потока
    frames_per_sector = 4032           # моно
    per_sec = args.rate / frames_per_sector
    lanes = int((75 * args.speed) / per_sec)
    if lanes > 32:
        lanes = 32
    print(f"поток {args.rate} Гц моно: {per_sec:.2f} сект/с, "
          f"привод {75 * args.speed} сект/с -> каналов в пачке {lanes}")

    lines = []
    for line in open(os.path.join(root, args.manifest), encoding="utf-8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        name, src = line.split()[:2]
        p = os.path.join(root, src)
        if not os.path.exists(p):
            raise SystemExit(f"нет файла {src}")
        lines.append((name, p))

    if not lines:
        raise SystemExit("в манифесте нет ни одной реплики")

    # Кодируем каждую в канал 0, длину узнаём по числу секторов
    tmp = os.path.join(root, "_voice_tmp.xa")
    blobs = []
    for name, path in lines:
        data = encode_one(path, tmp, args.rate, 0, args.psxavenc)
        n = len(data) // SECTOR
        blobs.append((name, data, n))
        print(f"  {name:16} {n:4} сект. ({n / per_sec:5.1f} с)")
    if os.path.exists(tmp):
        os.remove(tmp)

    # Сортируем по длине: пачка занимает столько, сколько самая длинная
    # реплика в ней, поэтому близкие по длине надо держать вместе. На
    # четырёх сотнях реплик это снижает добивку с 68% до 18%.
    #
    # Но на малом наборе сортировка ВРЕДИТ: последняя неполная пачка всё
    # равно резервирует все каналы, а после сортировки в неё попадают самые
    # длинные реплики. Поэтому при малом числе реплик оставляем как есть.
    plain = list(range(len(blobs)))
    order = sorted(plain, key=lambda i: blobs[i][2])

    def total(seq):
        t = 0
        for g in range(0, len(seq), lanes):
            t += max(blobs[i][2] for i in seq[g:g + lanes]) * lanes
        return t

    if total(plain) <= total(order):
        order = plain
        print("  порядок оставлен исходным: так компактнее")
    else:
        print(f"  реплики упорядочены по длине: "
              f"{(total(plain) - total(order)) * SECTOR // 1024} КБ экономии")

    entries = [None] * len(blobs)
    body = bytearray()
    group_start = 0

    for g in range(0, len(order), lanes):
        group = order[g:g + lanes]
        longest = max(blobs[i][2] for i in group)

        # Пачка ВСЕГДА занимает lanes слотов подряд, даже если реплик в ней
        # меньше. Расстояние между секторами одного канала задаёт темп
        # потока: сожмёшь его — и звук придёт быстрее, чем SPU успевает
        # проигрывать. Пустые слоты добиваются нулями.
        for k in range(longest):
            for lane in range(lanes):
                if lane < len(group):
                    name, data, n = blobs[group[lane]]
                    if k < n:
                        body += set_channel(data[k * SECTOR:(k + 1) * SECTOR],
                                            lane)
                        continue
                body += bytes(SECTOR)

        for lane, idx in enumerate(group):
            name, data, n = blobs[idx]
            entries[idx] = (group_start, lane, n)

        group_start += longest * lanes

    # Кадров развёртки на сектор в формате 8.8: движок по нему считает
    # длительность реплики, не храня её для каждой записи.
    fps_q8 = int(60.0 / per_sec * 256)

    idx = bytearray(12)
    idx[0:4] = b"VIDX"
    struct.pack_into("<HHHH", idx, 4, len(entries), lanes, fps_q8, 0)
    for sec, lane, n in entries:
        idx += struct.pack("<IBBH", sec, lane, 0, n)

    open(os.path.join(root, args.out_xas), "wb").write(bytes(body))
    open(os.path.join(root, args.out_idx), "wb").write(bytes(idx))

    print(f"\n{args.out_xas}: {len(body) / 1024 / 1024:.2f} МБ, "
          f"реплик {len(entries)}, пачек {(len(order) + lanes - 1) // lanes}")
    print(f"{args.out_idx}: указатель {len(idx)} байт")
    total = sum(n for _, _, n in blobs)
    print(f"полезных секторов {total}, всего {len(body) // SECTOR} "
          f"(добивка {100 - total * 100 // max(1, len(body) // SECTOR)}%)")


if __name__ == "__main__":
    main()
