#!/usr/bin/env python3
"""Validate a Sony Psy-Q SEQ and compare note events with the source MIDI."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from midiread import parse
from mkseq import collect


def vlq(d, p):
    v = 0
    for _ in range(4):
        if p >= len(d): raise ValueError("truncated VLQ")
        b = d[p]; p += 1
        v = (v << 7) | (b & 0x7f)
        if not b & 0x80: return v, p
    raise ValueError("VLQ too long")


def decode(path):
    d = open(path, 'rb').read()
    if d[:4] != b'pQES': raise ValueError('not pQES')
    version, ppqn = struct.unpack('>HH', d[4:8])
    tempo = int.from_bytes(d[8:11], 'big')
    events=[]; p=13; tick=0; running=0; loop_start=None
    while p < len(d):
        delta,p=vlq(d,p); tick += delta
        status=d[p]
        if status & 0x80:
            p+=1
            if status < 0xf0: running=status
        else:
            if not running: raise ValueError(f'running status missing at {p}')
            status=running
        typ=status & 0xf0; ch=status & 0xf
        if typ==0x80:
            note=d[p]; vel=d[p+1]; p+=2; events.append((tick,'off',ch,note,vel))
        elif typ==0x90:
            note=d[p]; vel=d[p+1]; p+=2; events.append((tick,'on',ch,note,vel))
        elif typ==0xa0:
            p+=2
        elif typ==0xb0:
            cc=d[p]; val=d[p+1]; p+=2
            events.append((tick,'cc',ch,cc,val))
            if ch==0 and cc==99 and val==20: loop_start=p
            if ch==0 and cc==99 and val==30: events.append((tick,'loop',loop_start))
        elif typ==0xc0:
            prog=d[p]; p+=1; events.append((tick,'prog',ch,prog))
        elif typ==0xd0:
            p+=1
        elif typ==0xe0:
            p+=2
        elif status==0xff:
            mt=d[p]; p+=1
            if mt==0x51:
                tempo_ev=int.from_bytes(d[p:p+3],'big'); p+=3
                events.append((tick,'tempo',tempo_ev))
            elif mt==0x58:
                events.append((tick,'timesig',d[p],d[p+1],d[p+2],d[p+3])); p+=4
            elif mt==0x2f:
                events.append((tick,'end')); break
            else: raise ValueError(f'unknown meta {mt:#x} at {p}')
        else:
            raise ValueError(f'unsupported status {status:#x}')
    return dict(version=version,ppqn=ppqn,tempo=tempo,events=events,consumed=p,size=len(d))


def main():
    src, seq = sys.argv[1], sys.argv[2]
    r=decode(seq)
    ok=True
    print(f'header: Psy-Q SEQ v{r["version"]}, PPQN {r["ppqn"]}, tempo {r["tempo"]} us/qn')
    print(f'[ok] stream decoded to {r["consumed"]}/{r["size"]} bytes') if r['consumed']==r['size'] else print('!! trailing bytes')
    if r['version'] != 1: ok=False
    mid=parse(src)
    src_e=[]
    for key,e in collect(mid):
        if e[0]=='midi' and (e[1]&0xf0) in (0x80,0x90):
            src_e.append((key[0],e[1],e[2]))
    dec=[(e[0],0x90 if e[1]=='on' else 0x80,e[3]) for e in r['events'] if e[1] in ('on','off')]
    print(f'note events source: {len(src_e)}, SEQ: {len(dec)}')
    if len(src_e)!=len(dec): ok=False; print('!! note count mismatch')
    else:
        bad=sum(a[2]!=b[2] for a,b in zip(src_e,dec))
        print('[ok]' if bad == 0 else '[FAIL]', f'pitches match: {bad} mismatches')
        if bad: ok=False
    print('result:', 'OK' if ok else 'FAIL')
    return 0 if ok else 1

if __name__=='__main__': sys.exit(main())
