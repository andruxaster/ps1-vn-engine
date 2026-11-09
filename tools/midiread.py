"""Минимальный парсер SMF (Standard MIDI File) — без внешних зависимостей."""
import struct

def _vlq(d, i):
    v = 0
    while True:
        b = d[i]; i += 1
        v = (v << 7) | (b & 0x7F)
        if not (b & 0x80): return v, i

def parse(path):
    d = open(path, "rb").read()
    assert d[:4] == b"MThd", "не SMF"
    hlen = struct.unpack(">I", d[4:8])[0]
    fmt, ntrk, div = struct.unpack(">HHH", d[8:14])
    pos = 8 + hlen
    tracks = []
    while pos < len(d) and d[pos:pos+4] == b"MTrk":
        tlen = struct.unpack(">I", d[pos+4:pos+8])[0]
        body = d[pos+8:pos+8+tlen]
        pos += 8 + tlen
        ev, i, tick, run = [], 0, 0, None
        while i < len(body):
            dt, i = _vlq(body, i)
            tick += dt
            b = body[i]
            if b == 0xFF:                       # meta
                i += 1; mtype = body[i]; i += 1
                ln, i = _vlq(body, i)
                data = body[i:i+ln]; i += ln
                ev.append((tick, "meta", mtype, data)); run = None
            elif b in (0xF0, 0xF7):             # sysex
                i += 1; ln, i = _vlq(body, i); i += ln; run = None
            else:
                if b & 0x80: status = b; i += 1; run = status
                else: status = run
                cmd, ch = status & 0xF0, status & 0x0F
                if cmd in (0xC0, 0xD0): a = body[i]; i += 1; args = (a,)
                else: a, bb = body[i], body[i+1]; i += 2; args = (a, bb)
                ev.append((tick, "midi", cmd, ch, args))
        tracks.append(ev)
    return dict(format=fmt, division=div, tracks=tracks)
