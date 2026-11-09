import struct, glob, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_data import SCRAMBLE_SEED, keystream
chunks=[]
# Главы переехали в data/SCR при разделении по каталогам, а здесь остался
# старый путь: верификатор находил ноль глав и бодро отчитывался, что всё
# в порядке. Проверка пустоты — тоже проверка, поэтому ниже стоит явный
# отказ, если глав не нашлось.
files = sorted(glob.glob("data/SCR/CH*.VNS")) or sorted(glob.glob("data/CH*.VNS"))
if not files:
    raise SystemExit("не найдено ни одной главы — проверять нечего")
for f in files:
    d=open(f,"rb").read()
    assert d[:4]==b"VNS3", f"плохая магия в {f}"
    co,cl,so,sl=struct.unpack("<IIII",d[4:20])
    cid,cnt=struct.unpack("<HH",d[20:24])
    # пул строк на диске зашумлён — снимаем, иначе проверка ссылок бессмысленна
    ks = keystream(SCRAMBLE_SEED ^ cid, sl)
    pool = bytes(a ^ b for a, b in zip(d[so:so+sl], ks))
    chunks.append(dict(f=f,code=d[co:co+cl],strs=pool,id=cid,cnt=cnt))
    print(f"{f}: chunk_id={cid}/{cnt} code={cl} strs={sl}")
n=len(chunks)
assert all(c['cnt']==n for c in chunks), "chunk_count не совпадает"
assert [c['id'] for c in chunks]==list(range(n)), "нумерация сбита"

FIX={0:1,1:3,2:8,21:6,22:3,3:5,4:5,5:1,6:3,7:3,9:2,11:5,12:3,13:7,14:2,15:6,16:7,19:3,20:5}

# Длина BGM зависит от подкоманды: play 4, stop 2, fadeout/fadein 4,
# volume 3. Раньше здесь стояла фиксированная двойка, и проход разъезжался
# на первой же музыке — а отчёт при этом выглядел успешным.
BGM_LEN={0:4, 1:2, 2:4, 3:4, 4:3}

def isize(code,off):
    op=code[off]
    if op==8:  return BGM_LEN.get(code[off+1], 2)
    if op==10: return 2+8*code[off+1]
    if op==17: return 6+8*code[off+5]
    if op==18: return 3+code[off+1]*2   # SCENE: 3 байта + по 2 на кадр
    if op not in FIX:
        raise SystemExit(f"неизвестный опкод {op} на смещении {off} — "
                         f"таблица верификатора отстала от компилятора")
    return FIX[op]

bounds=[]
for c in chunks:
    b=set(); off=0
    while off<len(c['code']):
        b.add(off); off+=isize(c['code'],off)
    assert off==len(c['code']), f"{c['f']}: код разъехался"
    bounds.append(b)
print("\n[ok] все чанки разбираются по инструкциям до конца")

def stroperands(code,off):
    op=code[off]
    if op in (3,4,16): return [off+1]
    if op==10: return [off+2+8*k for k in range(code[off+1])]
    if op==17: return [off+1]+[off+6+8*k for k in range(code[off+5])]
    return []

bad=0
for c in chunks:
    off=0
    while off<len(c['code']):
        for sp in stroperands(c['code'],off):
            g=struct.unpack("<I",c['code'][sp:sp+4])[0]
            if g>=len(c['strs']):
                print(f"  !! {c['f']} строка {g} вне пула ({len(c['strs'])})"); bad+=1
        off+=isize(c['code'],off)
print(f"[{'ok' if not bad else 'FAIL'}] строковые ссылки внутри своих пулов")

LBL={11:[1],13:[3]}
targets=0; bad2=0
for c in chunks:
    off=0
    while off<len(c['code']):
        op=c['code'][off]; ps=[]
        if op in LBL: ps=[off+p for p in LBL[op]]
        elif op==10: ps=[off+6+8*k for k in range(c['code'][off+1])]
        elif op==17: ps=[off+10+8*k for k in range(c['code'][off+5])]
        for p in ps:
            t=struct.unpack("<I",c['code'][p:p+4])[0]
            # 0xFFFFFFFF — не адрес, а особая цель пункта меню:
            # «открыть экран загрузки». Адресом её считать нельзя.
            # Особые цели пунктов меню, а не адреса: открыть загрузку
            # или галерею.
            if t in (0xFFFFFFFF, 0xFFFFFFFE): continue
            tc,to=t>>24,t&0xFFFFFF; targets+=1
            if tc>=n:
                print(f"  !! {c['f']}@{off}: чанк {tc} не существует"); bad2+=1
            elif to not in bounds[tc]:
                print(f"  !! {c['f']}@{off}: {tc}:{to} не граница инструкции"); bad2+=1
        off+=isize(c['code'],off)
print(f"[{'ok' if not bad2 else 'FAIL'}] переходов проверено: {targets}, битых: {bad2}")

# сквозной проход как в движке: fallthrough в следующий чанк
ci,pc,steps,seen=0,0,0,set()
while steps<10000:
    if ci>=n or ci<0:
        print(f"[FAIL] переход в несуществующую главу {ci}"); break
    if pc>=len(chunks[ci]['code']):
        if ci+1<n: ci,pc=ci+1,0; continue
        print(f"[ok] дошли до конца последней главы за {steps} шагов"); break
    op=chunks[ci]['code'][pc]; seen.add(op)
    if op==0:
        print(f"[ok] встретили END в главе {ci} за {steps} шагов"); break
    pc+=isize(chunks[ci]['code'],pc); steps+=1
print(f"     задействованы опкоды: {sorted(seen)}")
