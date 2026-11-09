#!/usr/bin/env python3
"""Распаковка ART.PAK — чтобы содержимое можно было проверить глазами.

  python3 tools/unpak.py data/ART.PAK            список записей
  python3 tools/unpak.py data/ART.PAK out/       выгрузить всё в каталог
"""
import os, struct, sys

def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    d = open(sys.argv[1], "rb").read()
    if d[:4] != b"APAK":
        raise SystemExit("это не APAK")
    n = struct.unpack("<H", d[4:6])[0]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    if out:
        os.makedirs(out, exist_ok=True)
    print(f"записей: {n}, файл {len(d)/1024/1024:.2f} МБ")
    for i in range(n):
        sec, size = struct.unpack("<II", d[8 + i * 8:16 + i * 8])
        blob = d[sec * 2048:sec * 2048 + size]
        tag = "TIM" if blob[:2] == b"\x10\x00" else "?"
        if out:
            open(os.path.join(out, f"A{i:04d}.TIM"), "wb").write(blob)
        else:
            print(f"  {i:4}: сектор {sec:6} размер {size:7} {tag}")
    if out:
        print(f"выгружено в {out}")

if __name__ == "__main__":
    main()
