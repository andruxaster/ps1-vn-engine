#!/usr/bin/env python3
"""Validate the Psy-Q VAB produced by mkvab.py."""
import struct, sys

def main(path):
    d=open(path,'rb').read(); ok=True
    if d[:4]!=b'pBAV': raise SystemExit('not pBAV')
    ver,id_,fsize=struct.unpack_from('<III',d,4)
    ps,ts,vs=struct.unpack_from('<HHH',d,18)
    tone_off=32+128*16
    index_off=tone_off+ps*16*32
    body_off=index_off+512
    print(f'VAB v{ver}: programs={ps}, tones={ts}, VAGs={vs}, size={len(d)}')
    if fsize!=len(d): ok=False; print(f'!! header size {fsize} != file {len(d)}')
    if body_off>len(d): ok=False; print('!! tables exceed file')
    vals=[]
    if body_off<=len(d):
        for i in range(1,vs+1):
            n=struct.unpack_from('<H',d,index_off+i*2)[0]*8
            vals.append(n)
        total=sum(vals)
        print(f'VAG body: {total} bytes, actual {len(d)-body_off}')
        if total!=len(d)-body_off: ok=False; print('!! VAG size index does not match body')
        for p in range(ps):
            pa=d[32+p*16:32+p*16+16]
            tones=pa[0]
            if tones:
                if tones>16: ok=False; print(f'!! program {p}: tones={tones}')
                ta=d[tone_off+p*512:tone_off+p*512+32]
                if ta[22] < 1 or ta[22] > vs:
                    ok=False; print(f'!! program {p}: invalid VAG {struct.unpack_from("<H",ta,22)[0]}')
    print('result:', 'OK' if ok else 'FAIL')
    return 0 if ok else 1
if __name__=='__main__': sys.exit(main(sys.argv[1]))
