"""Кодирование звука в SPU-ADPCM — родной формат сэмплов PlayStation.

Блок = 16 байт = 28 сэмплов:
  байт 0: shift (младший полубайт) | filter (старший)
  байт 1: флаги — бит0 конец петли, бит1 повтор, бит2 начало петли
  байты 2..15: 28 полубайтов по 4 бита

Декодирование (как в железе):
  t = знаковое_4бита(полубайт)
  s = (t << 12) >> shift  +  (prev1*f0 + prev2*f1 + 32) >> 6
  с ограничением в int16

Кодировщик перебирает все 5 фильтров и все сдвиги, считая ошибку с учётом
обратной связи квантования (как декодер), и берёт лучший вариант. Так
накопления ошибки не происходит.
"""

# коэффициенты предсказателя, умноженные на 64
F0 = (0, 60, 115, 98, 122)
F1 = (0, 0, -52, -55, -60)


def _clamp16(v):
    return -32768 if v < -32768 else (32767 if v > 32767 else v)


def _encode_block(samples, prev1, prev2):
    """Возвращает (16 байт, prev1, prev2) для 28 сэмплов."""
    best = None

    for f in range(5):
        # прикидываем нужный сдвиг по максимальному остатку
        peak = 0
        p1, p2 = prev1, prev2
        for s in samples:
            pred = (p1 * F0[f] + p2 * F1[f] + 32) >> 6
            d = s - pred
            peak = max(peak, abs(d))
            p1, p2 = s, p1

        shift = 0
        while shift < 12 and (peak >> (12 - shift)) > 7:
            shift += 1

        for sh in (shift, min(shift + 1, 12)):
            p1, p2 = prev1, prev2
            nibbles, err = [], 0
            for s in samples:
                pred = (p1 * F0[f] + p2 * F1[f] + 32) >> 6
                d = s - pred
                q = (d + (1 << (11 - sh))) >> (12 - sh) if sh < 12 else d
                q = -8 if q < -8 else (7 if q > 7 else q)
                rec = _clamp16(((q << 12) >> sh) + pred)
                err += (s - rec) ** 2
                nibbles.append(q & 0xF)
                p1, p2 = rec, p1
            if best is None or err < best[0]:
                best = (err, f, sh, nibbles, p1, p2)

    _, f, sh, nib, p1, p2 = best
    out = bytearray(16)
    out[0] = (f << 4) | sh
    out[1] = 0
    for i in range(14):
        out[2 + i] = nib[i * 2] | (nib[i * 2 + 1] << 4)
    return bytes(out), p1, p2


def encode(pcm, loop_start=None):
    """pcm — список int16. Возвращает bytes в SPU-ADPCM.

    loop_start — номер сэмпла, с которого зациклить (None = одиночный звук).
    SPU требует, чтобы у последнего блока стоял флаг конца, иначе голос
    убежит читать чужую память.
    """
    data = bytearray()
    p1 = p2 = 0
    nblocks = (len(pcm) + 27) // 28
    loop_block = None if loop_start is None else loop_start // 28

    for b in range(nblocks):
        chunk = list(pcm[b * 28:(b + 1) * 28])
        chunk += [0] * (28 - len(chunk))
        blk, p1, p2 = _encode_block(chunk, p1, p2)
        blk = bytearray(blk)

        if loop_block is not None and b == loop_block:
            blk[1] |= 0x04                     # начало петли
        if b == nblocks - 1:
            blk[1] |= 0x01                     # конец данных
            blk[1] |= 0x02 if loop_block is not None else 0x00
        data += blk

    return bytes(data)


def decode(data):
    """Обратное преобразование — для проверки качества кодирования."""
    out = []
    p1 = p2 = 0
    for off in range(0, len(data) - 15, 16):
        hdr = data[off]
        sh, f = hdr & 0x0F, (hdr >> 4) & 0x07
        if f > 4:
            f = 0
        if sh > 12:
            sh = 9
        for i in range(28):
            byte = data[off + 2 + i // 2]
            n = (byte & 0x0F) if (i % 2 == 0) else (byte >> 4)
            if n > 7:
                n -= 16
            s = _clamp16(((n << 12) >> sh) + ((p1 * F0[f] + p2 * F1[f] + 32) >> 6))
            out.append(s)
            p2, p1 = p1, s
    return out
