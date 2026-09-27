# Whole-voice integrity checks of Microsoft Anna's data (AnnaVoice/), from facts documented in ms-ana-decomp's notes
# (engine.md 2.1/2.2, decwrp_common.md, wmav.md). Reads only; about 5 s.   python3 tools/anna_integrity.py AnnaVoice
import struct, sys
d = sys.argv[1] + '/M1033DSK.'
key = open(d + 'KEY', 'rb').read()
csd = open(d + 'CSD', 'rb').read()
idx = open(d + 'IDX', 'rb').read()
unt = open(d + 'UNT', 'rb').read()
wih = open(d + 'WIH', 'rb').read()
ok = True
def check(c, what):
    global ok
    print(('ok   ' if c else 'FAIL ') + what); ok &= bool(c)
nblk = (len(idx) - 72) // 2
check(nblk == 66218 and len(csd) == nblk * 450, 'IDX %d blocks, CSD %d bytes = blocks x 450' % (nblk, len(csd)))
pcmb = struct.unpack('<%dH' % nblk, idx[72:])
check(sum(pcmb) == 635637906 and all(x % 960 == 0 and 4800 <= x <= 18240 for x in pcmb[:-1]) and pcmb[-1] == 7506,
      'IDX PCM bytes total %d (notes: 635,637,906); all but the last (partial, 7506) are multiples of 960 in 4800..18240' % sum(pcmb))
bad = 0
for k in range(nblk):
    b0 = csd[k * 450] ^ key[0]
    if (b0 >> 4) != (k % 16) or not (b0 & 8): bad += 1
check(bad == 0, 'descrambled packet header: seq nibble = block mod 16 and the residual-LSP bit set in %d/%d packets' % (nblk - bad, nblk))
ver, tto, ttc, z, uo, uc = struct.unpack_from('<6I', unt, 0)
check((ver, tto, ttc, uo, uc) == (1, 0x100, 261, 0x514, 159043) and len(unt) == 0x514 + uc * 20, 'UNT header, %d units, size exact' % uc)
check(wih[:16] == bytes.fromhex('00000000030000' '00803e000002000100'), 'WIH header 00000000 03000000 803e0000 02000100')
body = wih[16:]
badw = 0; total = 0; prev_end = None; gaps = 0
for u in range(uc):
    f0, lenw, start, woff, wn = struct.unpack_from('<IIIIH', unt, 0x514 + 20 * u)
    n = (lenw >> 1) & 0xFFFFFF
    s = 0; i = woff; e = woff + wn
    while i < e:
        x = body[i] - 256 if body[i] > 127 else body[i]
        if x == 127:
            while True:
                i += 1
                if i >= e: break
                x += body[i]
                if body[i] != 0x7f: break
        elif x == -128:
            while True:
                i += 1
                if i >= e: break
                y = body[i] - 256 if body[i] > 127 else body[i]
                x += y
                if body[i] != 0x80: break
        s += abs(x); i += 1
    if s != n: badw += 1
    total += n
check(badw == 0, 'WIH: sum of |epoch| = unit length for %d/%d units' % (uc - badw, uc))
check(total == sum(pcmb) // 2 == 317818953, 'the units tile the decoded stream: %d samples = %.4f h (notes: 317,818,953, 5.5177 h)' % (total, total / 16000 / 3600))
print('ALL OK' if ok else 'FAILURES')
sys.exit(0 if ok else 1)
