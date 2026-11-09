#!/usr/bin/env python3
"""Диагностика STR: определяет формат сектора и разбирает заголовки.

  python3 tools/probe_str.py video/intro.str

Понимает 2352 (strcd, сырой CD-сектор), 2336 (str, для mkpsxiso)
и 2048 (strv, только видео).
"""
import sys, struct

data = open(sys.argv[1], "rb").read()

# Определяем раскладку по кратности размера
if len(data) % 2352 == 0:
    SECTOR, SUB, DATA, kind = 2352, 0x12, 0x18, "strcd (сырой CD-сектор)"
elif len(data) % 2336 == 0:
    SECTOR, SUB, DATA, kind = 2336, 0x02, 0x08, "str (для mkpsxiso)"
elif len(data) % 2048 == 0:
    SECTOR, SUB, DATA, kind = 2048, None, 0, "strv (только видео)"
else:
    print(f"размер {len(data)} не кратен ни 2352, ни 2336, ни 2048 — "
          f"mkpsxiso такой файл отвергнет")
    sys.exit(1)

n = len(data) // SECTOR
print(f"файл {len(data)} байт = {n} секторов по {SECTOR}  -> {kind}")
print(f"mkpsxiso принимает 2336 и 2048: "
      f"{'ДА' if SECTOR in (2336, 2048) else 'НЕТ — нужен -t str'}")

def submode(i):
    return data[i*SECTOR + SUB] if SUB is not None else 0

# Первый видеокадр — печатаем весь STR-заголовок
for i in range(n):
    if submode(i) & 0x20:          # Form2 = аудио
        continue
    body = data[i*SECTOR + DATA : i*SECTOR + DATA + 32]
    if len(body) < 20:
        continue
    sid, stype, sn, sc = struct.unpack("<HHHH", body[:8])
    if sid == 0x0160 and stype == 0x8001 and sn == 0:
        print(f"\nпервый кадр — сектор {i}, submode {submode(i):#x}")
        print("STR-заголовок:", " ".join(f"{b:02x}" for b in body))
        print(f"  sec_no={sn} sec_count={sc}")
        print(f"  frame_no  = {struct.unpack('<I', body[8:12])[0]}")
        print(f"  bs_length = {struct.unpack('<I', body[12:16])[0]}")
        w, h = struct.unpack("<HH", body[16:20])
        print(f"  width x height = {w}x{h}")
        break
else:
    print("\nне нашёл видеосектор с sec_no==0")

frames, counts, audio = 0, {}, 0
for i in range(n):
    if submode(i) & 0x20:
        audio += 1
        continue
    body = data[i*SECTOR + DATA : i*SECTOR + DATA + 8]
    if len(body) < 8:
        continue
    sid, stype, sn, sc = struct.unpack("<HHHH", body)
    if sid == 0x0160 and stype == 0x8001:
        if sn == 0:
            frames += 1
        counts[sc] = counts.get(sc, 0) + 1

print(f"\nкадров (sec_no==0) = {frames}, аудиосекторов = {audio}")
print(f"sec_count-значения: {counts}")
