#!/usr/bin/env python3
"""Сборка Psy-Q VAB: несколько программ + отдельная программа ударных.

Прежняя версия умела одно: один WAV — одна GM-программа, один тон на весь
диапазон нот. Для реальной партитуры этого мало: девять мелодических партий
играли одним звуком, а ударные — им же, хотя на них приходится до сорока
процентов всех нот.

Здесь банк описывается манифестом instruments.txt:

    # мелодические программы (несколько GM-номеров могут делить один сэмпл)
    prog 0,2,4,5      piano      60
    prog 33,34,38     bass       48

    # ударные: нота -> свой сэмпл, высота не сдвигается
    drum 35,36        kick
    drum 42,44        hihat

Имя без расширения ищется в instruments/<имя>.wav. Если файла нет, звук
синтезируется по имени — банк собирается сразу, а свои записи можно
подкладывать по одной.

Ударные живут в программе DRUM_PROG: конвертер SEQ переводит на неё
десятый канал MIDI.
"""

import argparse
import math
import os
import random
import struct
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spuadpcm import encode                                  # noqa: E402

SR = 22050
DRUM_PROG = 127          # обязан совпадать с DRUM_PROG в tools/mkseq.py
PROG_COUNT = 128
VAB_TONES = 16
VAB_HDR = 32
VAB_TONE_SIZE = 32
VAB_INDEX = 512


def _env(n, attack, decay):
    out = []
    for i in range(n):
        t = i / SR
        a = min(1.0, t / attack) if attack > 0 else 1.0
        out.append(a * math.exp(-decay * t))
    return out


def _tone(freq, sec, decay, harmonics, attack=0.004):
    n = int(SR * sec)
    env = _env(n, attack, decay)
    pcm = []
    for i in range(n):
        t = i / SR
        v = sum(math.sin(2 * math.pi * freq * k * t) * a for k, a in harmonics)
        pcm.append(int(max(-1.0, min(1.0, v * env[i])) * 28000))
    return pcm


def _tone_loop(freq, harmonics, attack=0.02, decay=0.25, sustain=0.7,
               loops=8):
    """Тянущийся инструмент с бесшовной петлёй.

    Возвращает (сэмплы, точка_петли).

    Петля бесшовна, только если её длина укладывается в ЦЕЛОЕ число
    периодов основного тона — иначе на стыке волна не совпадёт и каждый
    цикл будет давать щелчок. Дополнительно длина должна быть кратна 28:
    столько сэмплов в одном блоке кодировщика SPU.

    Поэтому частоту чуть подгоняем под период, кратный 28. Расхождение
    выходит около шести центов — при пороге различимости примерно в
    десять центов это неслышно.
    """
    period = max(28, int(round(SR / freq / 28)) * 28)

    # Частоту пересчитываем ПОД выбранный период. Иначе фаза за петлю
    # уходит, и стык перестаёт совпадать — а это и есть щелчок.
    freq = SR / period

    a = max(1, int(SR * attack))
    d = max(1, int(SR * decay))
    head = a + d
    loop_start = ((head + period - 1) // period) * period   # к границе периода
    total = loop_start + loops * period

    out = []
    for i in range(total):
        # огибающая: атака, спад до уровня удержания, дальше ровно
        if i < a:
            env = i / a
        elif i < head:
            k = (i - a) / d
            env = 1.0 - (1.0 - sustain) * k
        else:
            env = sustain

        # Высокие обертоны гаснут быстрее низких — так звучат настоящие
        # инструменты. В области петли затухание останавливается, иначе
        # петля не была бы бесшовной.
        t = min(i, head) / SR
        v = 0.0
        for mult, amp in harmonics:
            damp = 2.718281828 ** (-t * 0.8 * mult)
            v += amp * damp * __import__("math").sin(
                2 * 3.141592653589793 * freq * mult * i / SR)
        out.append(int(max(-1.0, min(1.0, v * env)) * 30000))

    return out, loop_start


def _noise(sec, decay, lowpass=0.0, seed=1):
    rnd = random.Random(seed)
    n = int(SR * sec)
    env = _env(n, 0.001, decay)
    pcm, prev = [], 0.0
    for i in range(n):
        v = rnd.uniform(-1, 1)
        if lowpass:
            v = prev + (v - prev) * lowpass
            prev = v
        pcm.append(int(max(-1.0, min(1.0, v * env[i])) * 26000))
    return pcm


def _sweep(f0, f1, sec, decay):
    n = int(SR * sec)
    env = _env(n, 0.001, decay)
    pcm, phase = [], 0.0
    for i in range(n):
        t = i / n
        f = f0 * (f1 / f0) ** t
        phase += 2 * math.pi * f / SR
        pcm.append(int(max(-1.0, min(1.0, math.sin(phase) * env[i])) * 30000))
    return pcm


def _mix(a, b):
    n = max(len(a), len(b))
    a = a + [0] * (n - len(a))
    b = b + [0] * (n - len(b))
    return [max(-32000, min(32000, x + y)) for x, y in zip(a, b)]


SYNTH = {
    "piano":     lambda: _tone(261.6, 1.0, 3.2, [(1, .6), (2, .25), (3, .1), (4, .05)]),
    "epiano":    lambda: _tone(261.6, 1.0, 2.6, [(1, .7), (2, .18), (4, .08)]),
    "marimba":   lambda: _tone(261.6, 0.5, 9.0, [(1, .7), (4, .2), (9, .08)]),
    "guitar":    lambda: _tone(261.6, 0.9, 4.0, [(1, .6), (2, .2), (3, .12), (5, .06)]),
    "bass":      lambda: _tone(130.8, 0.9, 3.0, [(1, .8), (2, .15), (3, .05)]),
    "lead":      lambda: _tone(261.6, 0.8, 1.6, [(1, .5), (2, .25), (3, .15), (4, .1)]),
    "pad":       lambda: _tone_loop(261.6, [(1, .5), (2, .2), (3, .15), (5, .1)],
                                    attack=0.12, decay=0.35, sustain=0.85),
    "strings":   lambda: _tone_loop(261.6, [(1, .55), (2, .22), (3, .13)],
                                    attack=0.09, decay=0.3, sustain=0.8),
    # орган: нечётные гармоники, почти без затухания
    "organ":     lambda: _tone_loop(261.6, [(1, .45), (2, .25), (3, .15),
                                            (4, .1), (6, .05)],
                                    attack=0.02, decay=0.05, sustain=0.95),
    # медь: яркая, с наросшей атакой
    "brass":     lambda: _tone_loop(261.6, [(1, .4), (2, .3), (3, .2),
                                            (4, .12), (5, .07)],
                                    attack=0.05, decay=0.2, sustain=0.75),
    # язычковые: тёплые нечётные гармоники
    "reed":      lambda: _tone_loop(261.6, [(1, .55), (3, .25), (5, .12),
                                            (7, .05)],
                                    attack=0.03, decay=0.2, sustain=0.8),
    # флейта: почти чистый тон с лёгким призвуком
    "flute":     lambda: _tone_loop(261.6, [(1, .75), (2, .12), (3, .05)],
                                    attack=0.06, decay=0.15, sustain=0.85),
    # колокол: негармонические призвуки, долгий хвост
    "bell":      lambda: _tone(261.6, 1.1, 2.2, [(1, .5), (2.76, .25),
                                                 (5.4, .12), (8.9, .06)]),
    "kick":      lambda: _sweep(120, 45, 0.28, 14),
    "snare":     lambda: _mix(_noise(0.22, 22, 0.7, 2), _tone(190, 0.22, 20, [(1, .5)])),
    "hihat":     lambda: _noise(0.06, 60, 0.95, 3),
    "hihatopen": lambda: _noise(0.35, 11, 0.95, 4),
    "crash":     lambda: _noise(0.9, 4.5, 0.9, 5),
    "ride":      lambda: _noise(0.7, 6.0, 0.85, 6),
    "clap":      lambda: _noise(0.18, 26, 0.8, 7),
    "tom":       lambda: _sweep(220, 110, 0.30, 11),
    "shaker":    lambda: _noise(0.10, 40, 0.98, 8),
    "tamb":      lambda: _noise(0.16, 28, 0.97, 9),
}


def read_wav_loop(path):
    """Точка петли из служебного блока smpl, если он есть.

    Так её размечают авторы банков SoundFont, и наш извлекатель
    tools/sf2dump.py её сохраняет. Без петли тянущиеся инструменты
    обрываются посреди ноты.
    """
    d = open(path, "rb").read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        return None
    pos = 12
    while pos + 8 <= len(d):
        cid = d[pos:pos + 4]
        size = struct.unpack("<I", d[pos + 4:pos + 8])[0]
        if cid == b"smpl" and size >= 60:
            b = d[pos + 8:pos + 8 + size]
            loops = struct.unpack("<I", b[28:32])[0]
            if loops:
                ls, le = struct.unpack("<II", b[44:52])
                if le > ls:
                    return ls
            return None
        pos += 8 + size + (size & 1)
    return None


def load_wav(path):
    with wave.open(path, "rb") as w:
        ch, width, rate, n = (w.getnchannels(), w.getsampwidth(),
                              w.getframerate(), w.getnframes())
        raw = w.readframes(n)
    if width != 2:
        raise SystemExit(f"{path}: нужен 16-битный WAV")
    pcm = list(struct.unpack(f"<{len(raw) // 2}h", raw))
    if ch > 1:
        pcm = [sum(pcm[i:i + ch]) // ch for i in range(0, len(pcm), ch)]
    return pcm, rate


def get_sample(root, name):
    """Возвращает (сэмплы, частота, откуда, точка_петли).

    Точка петли — номер сэмпла, с которого звук зацикливается, или None
    для одиночных звуков. Тянущиеся инструменты без петли обрывались
    посреди ноты: сэмпл кончался раньше, чем нота.
    """
    path = os.path.join(root, "instruments", name + ".wav")
    if os.path.exists(path):
        pcm, rate = load_wav(path)
        loop = read_wav_loop(path)
        return pcm, rate, "файл, петля" if loop is not None else "файл", loop
    if name in SYNTH:
        r = SYNTH[name]()
        if isinstance(r, tuple):
            pcm, loop = r
            return pcm, SR, "синтез, петля", loop
        return r, SR, "синтез", None
    raise SystemExit(f"нет ни instruments/{name}.wav, ни синтеза для '{name}'")


def pack_prog(tones, vol=127, prior=64, mode=0, pan=64):
    return struct.pack("<BBBBBbHII", tones, vol, prior, mode, pan, 0, 0, 0, 0)


def pack_tone(prog, vag, center, lo, hi, adsr1, adsr2, vol=127, pan=64):
    """Раскладка сверена с разбором в src/seq.c: [2]=vol, [3]=pan,
    [4]=center, [5]=shift, [6]=min, [7]=max, [16..]=ADSR, prog, vag."""
    return struct.pack(
        "<14B2BHHhh4h",
        64, 0, vol, pan, center & 127, 0, lo & 127, hi & 127,
        0, 0, 0, 0, 2, 12, 0, 0,
        adsr1, adsr2, prog, vag,
        0, 0, 0, 0)


def parse_manifest(path):
    melodic, drums, effects = [], [], []
    for line in open(path, encoding="utf-8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        p = line.split()
        if p[0] == "prog":
            melodic.append(([int(x) for x in p[1].split(",")], p[2],
                            int(p[3]) if len(p) > 3 else 60))
        elif p[0] == "drum":
            # "35,36" или диапазон "41-50"; третье поле — центральная нота
            notes = []
            for part in p[1].split(","):
                if "-" in part:
                    a, b = part.split("-")
                    notes.append((int(a), int(b)))
                else:
                    notes.append((int(part), int(part)))
            center = int(p[3]) if len(p) > 3 else None
            drums.append((notes, p[2], center))
        elif p[0] == "sfx":
            # sfx <имя> <сэмпл> — короткий звук для команды se.
            # Он не привязан к программе и ноте: движок играет его
            # напрямую по номеру сэмпла, на голосах свободных от музыки.
            if len(p) < 3:
                raise SystemExit("sfx: нужно имя и сэмпл")
            effects.append((p[1], p[2]))
        else:
            raise SystemExit(f"непонятная строка манифеста: {line}")
    return melodic, drums, effects


def main():
    ap = argparse.ArgumentParser(description="сборка Psy-Q VAB")
    ap.add_argument("out")
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--root", default=None)
    args = ap.parse_args()

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    manifest = args.manifest or os.path.join(root, "instruments.txt")

    if os.path.exists(manifest):
        melodic, drums, effects = parse_manifest(manifest)
    else:
        print("  instruments.txt не найден — собираю один пробный тон")
        melodic, drums, effects = [([0], "piano", 60)], [], []

    adsr1, adsr2 = 0x80FF, 0x09CE

    vags, vag_of = [], {}

    def vag_index(name):
        if name not in vag_of:
            pcm, rate, how, loop = get_sample(root, name)
            # Точка петли уходит в кодировщик: он расставит флаги
            # зацикливания в блоках ADPCM.
            vags.append((name, encode(pcm, loop), rate, how))
            vag_of[name] = len(vags)
        return vag_of[name]

    prog_tones = {}
    for nums, name, center in melodic:
        vi = vag_index(name)
        for p in nums:
            prog_tones.setdefault(p, []).append(
                pack_tone(p, vi, center, 0, 127, adsr1, adsr2))

    for spans, name, center in drums:
        vi = vag_index(name)
        for lo, hi in spans:
            # Для одиночной ноты центр равен ей самой: ударные не
            # транспонируются. Для диапазона центр задаётся явно, и соседние
            # ноты звучат чуть выше или ниже — так десяток томов
            # укладывается в один тон.
            c = center if center is not None else lo
            prog_tones.setdefault(DRUM_PROG, []).append(
                pack_tone(DRUM_PROG, vi, c, lo, hi, adsr1, adsr2))

    # Эффекты кладём в банк последними, чтобы их номера не сдвигались при
    # правке инструментов: сценарий ссылается на них по имени, но номер
    # попадает в байткод при сборке.
    sfx_index = {}
    for label, sample in effects:
        sfx_index[label] = vag_index(sample)

    over = [p for p, t in prog_tones.items() if len(t) > VAB_TONES]
    if over:
        raise SystemExit(f"в программе {over[0]} больше {VAB_TONES} тонов — "
                         f"столько VAB не вмещает")

    prog_table = bytearray()
    for p in range(PROG_COUNT):
        n = len(prog_tones.get(p, []))
        prog_table += pack_prog(n) if n else pack_prog(0, vol=0, prior=0)

    n_progs = (max(prog_tones) + 1) if prog_tones else 1
    tone_table = bytearray()
    for p in range(n_progs):
        tl = prog_tones.get(p, [])
        for t in range(VAB_TONES):
            tone_table += tl[t] if t < len(tl) else bytes(VAB_TONE_SIZE)

    body = bytearray()
    index = bytearray(VAB_INDEX)
    for i, (name, adpcm, rate, how) in enumerate(vags, start=1):
        struct.pack_into("<H", index, i * 2, len(adpcm) // 8)
        body += adpcm
        print(f"  сэмпл {i}: {name:10} {len(adpcm):6} байт  ({how})")

    hdr = bytearray(VAB_HDR)
    hdr[0:4] = b"pBAV"
    struct.pack_into("<I", hdr, 4, 7)
    struct.pack_into("<I", hdr, 12, VAB_HDR + len(prog_table) +
                     len(tone_table) + VAB_INDEX + len(body))
    struct.pack_into("<H", hdr, 18, n_progs)
    struct.pack_into("<H", hdr, 20, n_progs * VAB_TONES)
    struct.pack_into("<H", hdr, 22, len(vags))
    hdr[24] = 127

    out = bytes(hdr) + bytes(prog_table) + bytes(tone_table) + bytes(index) + bytes(body)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    open(args.out, "wb").write(out)

    if sfx_index:
        # Строка разбирается сборкой данных: имя=номер через запятую.
        print("  sfx: " + ",".join(f"{k}={v}" for k, v in sfx_index.items()))

    mel = [p for p in sorted(prog_tones) if p != DRUM_PROG]
    print(f"{args.out}: {len(out)} байт, программ {n_progs}, "
          f"сэмплов {len(vags)}, тело {len(body) // 1024} КБ")
    print(f"  мелодические программы: {mel}")
    if DRUM_PROG in prog_tones:
        print(f"  ударные: {len(prog_tones[DRUM_PROG])} нот в программе {DRUM_PROG}")


if __name__ == "__main__":
    main()
