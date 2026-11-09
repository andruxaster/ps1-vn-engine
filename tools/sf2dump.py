#!/usr/bin/env python3
"""Извлечение сэмплов из банка SoundFont (.sf2) в обычные WAV.

  python3 tools/sf2dump.py bank.sf2 --list
  python3 tools/sf2dump.py bank.sf2 --extract 0 --out instruments/piano.wav
  python3 tools/sf2dump.py bank.sf2 --extract 48 --key 60 --out strings.wav

Зачем. Наш конвейер берёт инструменты из obычных WAV в instruments/, а
свободные банки распространяются в формате SF2. Внутри него лежат ровно
те данные, которых нам не хватает: сами записи, ТОЧКИ ПЕТЛИ и базовая
нота — всё размечено авторами банка.

Файл SF2 устроен как RIFF с тремя разделами:
    INFO  сведения о банке
    sdta  подраздел smpl: непрерывный поток 16-битных сэмплов
    pdta  таблицы, описывающие, что где лежит:
            phdr  пресеты (номер программы и банка)
            pbag  зоны пресета -> pgen
            pgen  свойства зоны, среди них ссылка на инструмент (код 41)
            inst  инструменты
            ibag  зоны инструмента -> igen
            igen  свойства зоны: диапазон клавиш (43) и номер сэмпла (53)
            shdr  заголовки сэмплов: границы, петля, частота, базовая нота

Записанный WAV содержит служебный блок smpl с точкой петли, поэтому
сборщик банка подхватит её автоматически.
"""

import argparse
import os
import struct

GEN_KEYRANGE = 43
GEN_INSTRUMENT = 41
GEN_SAMPLEID = 53
GEN_OVERRIDE_ROOT = 58


def read_chunks(data, start, end):
    """Разбирает подряд идущие RIFF-блоки в диапазоне."""
    out = {}
    pos = start
    while pos + 8 <= end:
        cid = data[pos:pos + 4].decode("latin-1")
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        out[cid] = (pos + 8, size)
        pos += 8 + size + (size & 1)      # блоки выровнены по чётному
    return out


class SoundFont:
    def __init__(self, path):
        d = open(path, "rb").read()
        if d[:4] != b"RIFF" or d[8:12] != b"sfbk":
            raise SystemExit(f"{path}: это не SoundFont")

        top = read_chunks(d, 12, len(d))
        self.data = d

        # Разделы LIST помечены своим именем в первых четырёх байтах.
        lists = {}
        pos = 12
        while pos + 8 <= len(d):
            cid = d[pos:pos + 4]
            size = struct.unpack("<I", d[pos + 4:pos + 8])[0]
            if cid == b"LIST":
                lists[d[pos + 8:pos + 12].decode("latin-1")] = (
                    pos + 12, pos + 8 + size)
            pos += 8 + size + (size & 1)

        if "sdta" not in lists or "pdta" not in lists:
            raise SystemExit(f"{path}: нет разделов sdta или pdta")

        s0, s1 = lists["sdta"]
        self.smpl = read_chunks(d, s0, s1).get("smpl")
        if not self.smpl:
            raise SystemExit(f"{path}: нет потока сэмплов")

        p0, p1 = lists["pdta"]
        self.pdta = read_chunks(d, p0, p1)

        self.shdr = self._read_shdr()
        self.inst = self._read_names("inst", 22)
        self.phdr = self._read_phdr()
        self.ibag = self._read_bag("ibag")
        self.pbag = self._read_bag("pbag")
        self.igen = self._read_gen("igen")
        self.pgen = self._read_gen("pgen")

    def _read_shdr(self):
        off, size = self.pdta["shdr"]
        out = []
        for i in range(size // 46):
            b = self.data[off + i * 46: off + (i + 1) * 46]
            name = b[:20].split(b"\0")[0].decode("latin-1")
            start, end, ls, le, rate = struct.unpack("<IIIII", b[20:40])
            root, corr, link, typ = struct.unpack("<BbHH", b[40:46])
            out.append(dict(name=name, start=start, end=end,
                            loop_start=ls, loop_end=le, rate=rate,
                            root=root, corr=corr, type=typ))
        return out

    def _read_names(self, chunk, stride):
        off, size = self.pdta[chunk]
        out = []
        for i in range(size // stride):
            b = self.data[off + i * stride: off + (i + 1) * stride]
            name = b[:20].split(b"\0")[0].decode("latin-1")
            idx = struct.unpack("<H", b[20:22])[0]
            out.append(dict(name=name, bag=idx))
        return out

    def _read_phdr(self):
        off, size = self.pdta["phdr"]
        out = []
        for i in range(size // 38):
            b = self.data[off + i * 38: off + (i + 1) * 38]
            name = b[:20].split(b"\0")[0].decode("latin-1")
            prog, bank, bag = struct.unpack("<HHH", b[20:26])
            out.append(dict(name=name, prog=prog, bank=bank, bag=bag))
        return out

    def _read_bag(self, chunk):
        off, size = self.pdta[chunk]
        return [struct.unpack("<HH", self.data[off + i * 4: off + i * 4 + 4])
                for i in range(size // 4)]

    def _read_gen(self, chunk):
        off, size = self.pdta[chunk]
        out = []
        for i in range(size // 4):
            op, val = struct.unpack("<HH", self.data[off + i * 4: off + i * 4 + 4])
            out.append((op, val))
        return out

    # ---- поиск ----

    def list_presets(self):
        # Последняя запись служебная, её пропускаем.
        return [p for p in self.phdr[:-1]]

    def _zones(self, bag, gens, first, last):
        """Свойства зон в диапазоне мешков."""
        out = []
        for bi in range(first, min(last, len(bag) - 1)):
            g0 = bag[bi][0]
            g1 = bag[bi + 1][0]
            out.append(dict(gens[g0:g1]))
        return out

    def find_sample(self, prog, bank=0, key=60):
        """Ищет сэмпл для программы и ноты. Возвращает запись shdr."""
        pres = [p for p in self.list_presets()
                if p["prog"] == prog and p["bank"] == bank]
        if not pres:
            return None

        pi = self.phdr.index(pres[0])
        pzones = self._zones(self.pbag, self.pgen,
                             self.phdr[pi]["bag"], self.phdr[pi + 1]["bag"])

        for pz in pzones:
            if GEN_INSTRUMENT not in pz:
                continue
            ii = pz[GEN_INSTRUMENT]
            if ii + 1 >= len(self.inst):
                continue
            izones = self._zones(self.ibag, self.igen,
                                 self.inst[ii]["bag"], self.inst[ii + 1]["bag"])

            best = None
            for iz in izones:
                if GEN_SAMPLEID not in iz:
                    continue
                if GEN_KEYRANGE in iz:
                    lo = iz[GEN_KEYRANGE] & 0xFF
                    hi = (iz[GEN_KEYRANGE] >> 8) & 0xFF
                    if not (lo <= key <= hi):
                        continue
                sid = iz[GEN_SAMPLEID]
                if sid >= len(self.shdr):
                    continue
                s = dict(self.shdr[sid])
                if GEN_OVERRIDE_ROOT in iz and iz[GEN_OVERRIDE_ROOT] != 0xFFFF:
                    s["root"] = iz[GEN_OVERRIDE_ROOT]
                best = s
                break
            if best:
                return best
        return None

    def pcm(self, s):
        """Сэмплы одного звука как список 16-битных значений."""
        off, _ = self.smpl
        a = off + s["start"] * 2
        b = off + s["end"] * 2
        raw = self.data[a:b]
        return list(struct.unpack(f"<{len(raw) // 2}h", raw))


def write_wav(path, pcm, rate, loop_start=None, loop_end=None):
    """WAV с блоком smpl: в нём хранится точка петли."""
    body = struct.pack(f"<{len(pcm)}h", *pcm)

    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(body)) + body
    if len(body) & 1:
        chunks += b"\0"

    if loop_start is not None and loop_end and loop_end > loop_start:
        # Заголовок блока smpl ровно 36 байт (девять полей по четыре),
        # дальше по 24 байта на цикл. Ошибка в размере заголовка сдвигает
        # все последующие поля, и точка петли читается как мусор.
        smpl = struct.pack(
            "<IIIIIIIII",
            0,                      # manufacturer
            0,                      # product
            int(1e9 / rate),        # длительность сэмпла в наносекундах
            60,                     # базовая нота
            0,                      # доля полутона
            0,                      # формат SMPTE
            0,                      # смещение SMPTE
            1,                      # число циклов
            0)                      # длина дополнительных данных
        smpl += struct.pack(
            "<IIIIII",
            0,                      # номер цикла
            0,                      # тип: прямая петля
            loop_start,
            loop_end,
            0,                      # доля
            0)                      # повторов: 0 = бесконечно
        chunks += b"smpl" + struct.pack("<I", len(smpl)) + smpl

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE")
        f.write(chunks)


def main():
    ap = argparse.ArgumentParser(description="извлечение сэмплов из SF2")
    ap.add_argument("sf2")
    ap.add_argument("--list", action="store_true", help="показать инструменты")
    ap.add_argument("--extract", type=int, help="номер программы (0..127)")
    ap.add_argument("--bank", type=int, default=0)
    ap.add_argument("--key", type=int, default=60,
                    help="для какой ноты искать сэмпл (60 = до первой)")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    sf = SoundFont(args.sf2)

    if args.list or args.extract is None:
        print(f"{args.sf2}: инструментов {len(sf.list_presets())}\n")
        for p in sorted(sf.list_presets(), key=lambda x: (x["bank"], x["prog"])):
            print(f"  банк {p['bank']:3} программа {p['prog']:3}  {p['name']}")
        return

    s = sf.find_sample(args.extract, args.bank, args.key)
    if not s:
        raise SystemExit(f"программа {args.extract} в банке {args.bank} "
                         f"не найдена или без сэмпла для ноты {args.key}")

    pcm = sf.pcm(s)
    ls = s["loop_start"] - s["start"]
    le = s["loop_end"] - s["start"]
    has_loop = 0 <= ls < le <= len(pcm)

    out = args.out or f"instruments/prog{args.extract:03d}.wav"
    write_wav(out, pcm, s["rate"],
              ls if has_loop else None, le if has_loop else None)

    print(f"{out}: «{s['name']}»")
    print(f"  {len(pcm)} сэмплов, {s['rate']} Гц, {len(pcm)/s['rate']:.2f} с")
    print(f"  базовая нота {s['root']}"
          + (f", поправка {s['corr']} центов" if s["corr"] else ""))
    print(f"  петля: " + (f"с {ls} по {le}" if has_loop else "нет"))


if __name__ == "__main__":
    main()
