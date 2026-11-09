#!/usr/bin/env python3
"""MIDI/SMF -> Sony Psy-Q SEQ (pQES).

The generated file uses the documented single-track PlayStation SEQ format:
  0x00  "pQES"
  0x04  u16 BE version (1)
  0x06  u16 BE PPQN
  0x08  u24 BE initial tempo (microseconds/quarter)
  0x0B  u8 time-signature numerator
  0x0C  u8 time-signature denominator exponent
  0x0D  SEQ event stream

Event deltas are standard big-endian MIDI VLQs. Channel events use MIDI
status/data bytes. PS1 SEQ tempo meta events are FF 51 tt tt tt (no MIDI
length byte), and end-of-track is FF 2F.

For looping, the converter emits Sony's usual CC99 markers on channel 0:
  B0 63 14  = loop start
  B0 63 1E  = loop forever
"""

DRUM_PROG = 127   # обязан совпадать с DRUM_PROG в tools/mkvab.py

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from midiread import parse

MAGIC = b"pQES"
VERSION = 1


def vlq(n):
    n = max(0, int(n))
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


def collect(mid, transpose=0):
    """Merge SMF tracks into one stable, absolute-tick event list."""
    evs = []
    for ti, tr in enumerate(mid["tracks"]):
        for ei, e in enumerate(tr):
            tick = e[0]
            key = (tick, ti, ei)
            if e[1] == "meta":
                mt = e[2]
                if mt == 0x51 and len(e[3]) == 3:
                    evs.append((key, ("tempo", int.from_bytes(e[3], "big"))))
                elif mt == 0x58 and len(e[3]) >= 2:
                    evs.append((key, ("timesig", e[3][0], e[3][1])))
                elif mt == 0x06 and b"loop" in e[3].lower():
                    evs.append((key, ("loop",)))
            elif e[1] == "midi":
                cmd, ch, a = e[2], e[3], e[4]
                tr = 0 if ch == 9 else transpose
                if cmd == 0x90 and a[1] > 0:
                    evs.append((key, ("midi", 0x90 | ch,
                                      (a[0] + tr) & 127, a[1] & 127)))
                elif cmd == 0x80 or (cmd == 0x90 and a[1] == 0):
                    evs.append((key, ("midi", 0x80 | ch,
                                      (a[0] + tr) & 127, a[1] & 127)))
                elif cmd == 0xC0:
                    # Смену программы на десятом канале игнорируем: там номер
                    # ноты означает инструмент ударных, а не высоту, и партия
                    # должна идти через отдельную программу банка.
                    if ch != 9:
                        evs.append((key, ("midi", 0xC0 | ch, a[0] & 127)))
                elif cmd == 0xB0:
                    # Keep all CC events. In particular CC7/10 affect volume/pan,
                    # while CC99/98 are used by some Sony sequences for looping.
                    evs.append((key, ("midi", 0xB0 | ch, a[0] & 127, a[1] & 127)))
                elif cmd == 0xE0:
                    evs.append((key, ("midi", 0xE0 | ch, a[0] & 127, a[1] & 127)))
    # Десятый канал MIDI по стандарту всегда ударные. Прописываем ему
    # программу ударных нулевым событием, до всех нот.
    evs.append(((-1, -1, -1), ("midi", 0xC0 | 9, DRUM_PROG)))

    evs.sort(key=lambda x: x[0])
    return evs


def build(evs, division, loop_default=False):
    """Encode merged absolute events to a Psy-Q SEQ stream."""
    stream = bytearray()
    prev_tick = 0
    initial_tempo = 500000
    time_num, time_den_exp = 4, 2
    saw_tempo = False
    loop_marked = False

    # Header tempo is the first tempo event, matching the normal SMF default
    # if no tempo meta event exists.
    for (_key, ev) in evs:
        if ev[0] == "tempo":
            initial_tempo = ev[1]
            saw_tempo = True
            break
    if not saw_tempo:
        initial_tempo = 500000

    for (key, ev) in evs:
        tick = key[0]
        stream += vlq(tick - prev_tick)
        prev_tick = tick

        kind = ev[0]
        if kind == "tempo":
            tempo = max(1, min(0xFFFFFF, ev[1]))
            stream += b"\xFF\x51" + tempo.to_bytes(3, "big")
        elif kind == "timesig":
            # SEQ/libSnd understands MIDI time-signature meta events. Keep the
            # standard 4-byte payload used by the original source MIDI.
            stream += bytes((0xFF, 0x58, ev[1] & 0xFF, ev[2] & 0xFF, 24, 8))
            time_num, time_den_exp = ev[1] & 0xFF, ev[2] & 0xFF
        elif kind == "loop":
            # Explicit loop point from the source file.
            stream += b"\xB0\x63\x14"
            loop_marked = True
        elif kind == "midi":
            stream.append(ev[1])
            stream.extend(ev[2:])

    if loop_default and not loop_marked:
        # Put loop start at the beginning of the score, then loop forever at EOT.
        # This is intentionally a real SEQ CC marker rather than a private header.
        stream = bytearray(vlq(0) + b"\xB0\x63\x14") + stream
        loop_marked = True

    if loop_marked:
        stream += vlq(0) + b"\xB0\x63\x1E"
    stream += vlq(0) + b"\xFF\x2F"

    hdr = (MAGIC + struct.pack(">HH", VERSION, division & 0xFFFF) +
           initial_tempo.to_bytes(3, "big") +
           bytes((time_num, time_den_exp)))
    return hdr + stream, initial_tempo, loop_marked


def main():
    ap = argparse.ArgumentParser(description="MIDI -> Psy-Q SEQ")
    ap.add_argument("source")
    ap.add_argument("out")
    ap.add_argument("--transpose", type=int, default=0)
    ap.add_argument("--loop", action="store_true")
    args = ap.parse_args()

    mid = parse(args.source)
    if not (0 < mid["division"] <= 0xFFFF):
        raise SystemExit(f"неподдерживаемый PPQN: {mid['division']}")
    evs = collect(mid, args.transpose)
    blob, tempo, looped = build(evs, mid["division"], args.loop)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    open(args.out, "wb").write(blob)

    print(f"{args.out}: {len(blob)} байт")
    print(f"  Psy-Q SEQ, PPQN {mid['division']}, initial tempo {tempo} us/qn")
    print(f"  loop: {'да' if looped else 'нет'}")
    print(f"  events: {len(evs)}")


if __name__ == "__main__":
    main()
