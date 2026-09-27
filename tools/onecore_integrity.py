# Structural checks of Microsoft David / Zira / Mark's (en-US), Hazel / George / Susan's (en-GB), Eva's and Sarah's data (OneCoreVoice/),
# from facts documented in
# ms-david-zira-decomp's notes (frontend.md 1.1 - the .dat container; backend_io.md - APM; zb.h - [EmotionRecipe]).
# Reads only; well under a second.   python3 tools/onecore_integrity.py OneCoreVoice
import struct, sys
d = sys.argv[1] + '/'
ok = True
def check(c, what):
    global ok
    print(('ok   ' if c else 'FAIL ') + what); ok &= bool(c)

# .dat containers: chunk = GUID type, GUID id, u64 size, data; next chunk at (data + size + 7) & ~7. Three container
# types nest (0x099f9814, 0x3e13f66a, 0xe5f704bc); every other chunk is a resource. A file must tile exactly.
NEST = (0x099f9814, 0x3e13f66a, 0xe5f704bc)
def walk(b, o, end, depth, out):
    while o + 0x28 <= end:
        t1 = struct.unpack_from('<I', b, o)[0]
        sz = struct.unpack_from('<Q', b, o + 32)[0]
        c = o + 0x28
        if c + sz > end:
            return False
        if t1 in NEST:
            if not walk(b, c, c + sz, depth + 1, out):
                return False
        else:
            out.append((t1, sz))
        o = (c + sz + 7) & ~7
    return o >= end or end - o < 8
for name, want in (('MSTTSLocEnUS.dat', 20), ('enUS.Address.dat', 1), ('enUS.CompanyName.dat', 1), ('enUS.Computer.dat', 1),
                   ('enUS.Media.dat', 1), ('enUS.Message.dat', 1), ('enUS.Name.dat', 1),
                   ('MSTTSLocEnGB.dat', 17), ('EnGB.Address.dat', 1), ('EnGB.CityName.dat', 1), ('EnGB.CompanyName.dat', 1),
                   ('enGB.Computer.dat', 1), ('enGB.Message.dat', 1), ('enGB.Name.dat', 1)):
    b = open(d + name, 'rb').read()
    res = []
    tiled = walk(b, 0, len(b), 0, res)
    check(tiled and len(res) >= want, '%s: %d bytes, the chunks tile the file, %d resources' % (name, len(b), len(res)))

# the resources the text front end asks for by name must be in the main .dat (zf1_dat_get calls in Engine/onecore)
b = open(d + 'MSTTSLocEnUS.dat', 'rb').read()
res = []
walk(b, 0, len(b), 0, res)
have = {t for t, _ in res}
need = [0x7bd71f46, 0x29a5584b, 0xf81fd1d1, 0xe849e61b, 0xac4aefcf, 0xf6e4f50a, 0x78f6770d, 0x19a6569a, 0x00a2359e,
        0x5554ba64, 0xb54490e3, 0xd8951565, 0x0cb71848, 0x629aa5c4, 0x7d5841ab, 0xbfc4309d, 0xe67ab014]
missing = [hex(x) for x in need if x not in have]
check(not missing, 'MSTTSLocEnUS.dat has all %d resources the front end loads%s' % (len(need), (' - missing ' + ', '.join(missing)) if missing else ''))

# en-GB: the resources the front end needs (the optional suffix / quote / parallel-structure / TN pattern tables are not
# in this older data) and the word breaker in its older version 964f993a (zf1_wb.c reads both)
b = open(d + 'MSTTSLocEnGB.dat', 'rb').read()
res = []
walk(b, 0, len(b), 0, res)
have = {t for t, _ in res}
need_gb = [0x7bd71f46, 0x29a5584b, 0xf81fd1d1, 0xe849e61b, 0xac4aefcf, 0xf6e4f50a, 0x78f6770d, 0x19a6569a, 0x00a2359e,
           0x0cb71848, 0x629aa5c4, 0x7d5841ab, 0xe67ab014]
missing = [hex(x) for x in need_gb if x not in have]
check(not missing, 'MSTTSLocEnGB.dat has the %d resources the front end needs%s' % (len(need_gb), (' - missing ' + ', '.join(missing)) if missing else ''))
wb = [struct.unpack_from('<I', b, o + 16)[0] for o in range(0, len(b) - 20, 8) if struct.unpack_from('<I', b, o)[0] == 0x629aa5c4]
check(0x964f993a in wb, 'MSTTSLocEnGB.dat: word breaker resource version 964f993a')
# the phone set (29a5584b): 49 phones, no -SP-, stress / tone marks at ids 4 / 5
o = next(o for o in range(0, len(b) - 20, 8) if struct.unpack_from('<I', b, o)[0] == 0x29a5584b)   # chunks are 8-aligned
r = o + 0x28
rs, n = struct.unpack_from('<II', b, r)
ph = {}
for k in range(n):
    q = r + 8 + rs * k
    pid = struct.unpack_from('<H', b, q)[0]
    ph[pid] = b[q + 2:q + 48].decode('utf-16-le').split('\0')[0]
check(n == 49 and ph.get(4) == '1' and ph.get(5) == '2' and '-SP-' not in ph.values() and ph.get(49) == 'ZH',
      'MSTTSLocEnGB.dat phone set: %d phones, 4 = %r, 5 = %r, 49 = %r, no -SP-' % (n, ph.get(4), ph.get(5), ph.get(49)))

# voices: APM magic and 16 kHz, BEP present, INI with the three hidden emotions
for v in ('David', 'Zira', 'Mark'):
    apm = open(d + 'M1033%s.APM' % v, 'rb').read()
    rate = struct.unpack_from('<I', apm, 0x28)[0]
    check(apm[:4] == b'APM ' and rate == 16000, '%s.APM: "APM " header, %d Hz, %d bytes' % (v, rate, len(apm)))
    bep = open(d + 'M1033%s.BEP' % v, 'rb').read()
    check(len(bep) == 1776, '%s.BEP: %d bytes' % (v, len(bep)))
    ini = open(d + 'M1033%s.INI' % v, 'rb').read().decode('latin-1').replace('\r', '')
    sec = ini.split('[EmotionRecipe]', 1)[1].split('\n[', 1)[0] if '[EmotionRecipe]' in ini else ''
    emo = [l.split('=', 1)[1].strip() for l in sec.split('\n') if l.startswith('Emotion')]
    check(emo == ['happy', 'sad', 'angry'], '%s.INI: [EmotionRecipe] %s' % (v, ', '.join(emo) or 'MISSING'))
# en-GB voices: APM magic and 16 kHz, George's BEP (Hazel and Susan ship none), and NO [EmotionRecipe] in any INI
for v, bep_len in (('Hazel', 0), ('George', 1496), ('Susan', 0)):
    apm = open(d + 'M2057%s.APM' % v, 'rb').read()
    rate = struct.unpack_from('<I', apm, 0x28)[0]
    check(apm[:4] == b'APM ' and rate == 16000, '%s.APM: "APM " header, %d Hz, %d bytes' % (v, rate, len(apm)))
    if bep_len:
        bep = open(d + 'M2057%s.BEP' % v, 'rb').read()
        check(len(bep) == bep_len, '%s.BEP: %d bytes' % (v, len(bep)))
    ini = open(d + 'M2057%s.INI' % v, 'rb').read().decode('latin-1')
    check('[EmotionRecipe]' not in ini, '%s.INI: no [EmotionRecipe] (no emotion presets)' % v)
# Eva (neural): the NNM (631 inputs, 127 outputs per frame), the TDAT (version 3, 631 -> 508, 7 layers), the HEQ (one
# log-F0 table of 101 quantiles), the three CRF prosody models (labels x features = weight size), APM 16 kHz, INI emotions
apm = open(d + 'M1033Eva.APM', 'rb').read()
check(apm[:4] == b'APM ' and struct.unpack_from('<I', apm, 0x28)[0] == 16000, 'Eva.APM: "APM " header, 16000 Hz, %d bytes' % len(apm))
nnm = open(d + 'M1033Eva.NNM', 'rb').read()
qo = struct.unpack_from('<I', nnm, 0x38)[0]
nq = struct.unpack_from('<I', nnm, qo + 8 + 4 * struct.unpack_from('<I', nnm, qo + 8)[0] + 4)[0]
check(nnm[:4] == b'NNM ' and nq == 631, 'Eva.NNM: "NNM " header, %d network inputs' % nq)
td = open(d + 'M1033Eva.TDAT', 'rb').read()
io = struct.unpack_from('<I', td, 0x14)[0]; lt = struct.unpack_from('<I', td, 0x18)[0]
tin, tout = struct.unpack_from('<II', td, io); nl = struct.unpack_from('<I', td, lt)[0]
check(struct.unpack_from('<I', td, 0x10)[0] == 3 and (tin, tout, nl) == (631, 508, 7), 'Eva.TDAT: version 3, %d -> %d, %d layers' % (tin, tout, nl))
hq = open(d + 'M1033Eva.HEQ', 'rb').read()
check(hq[:4] == b'HEQT' and struct.unpack_from('<III', hq, 0x24) == (1, 2, 101) and len(hq) == 0x24 + 12 + 8 * 101,
      'Eva.HEQ: one log-F0 table of 101 quantiles, exact size')
for ext, labels, nfeat in (('BR2', 2, 167089), ('TON', 7, 6661), ('ACL', 2, 12182)):
    c = open(d + 'M1033Eva.' + ext, 'rb').read()
    nl_, nt, tr, ws, xs = struct.unpack_from('<IIIII', c, 0x2c)
    so, ss = struct.unpack_from('<II', c, 0x24)
    end = max(0x44 + 4 * (nl_ + nt) + tr + ws + xs, so + ss)   # the CRF part (BR2 carries 5.6 MB more that nothing reads)
    check(c[:3] == ext.encode() and c[0x1c:0x1f] == b'CRF' and nl_ == labels and ws == 4 * labels * nfeat and xs == 4 * labels * labels,
          'Eva.%s: CRF, %d labels, %d templates, %d x %d weights, CRF part ends at %#x of %d bytes' % (ext, nl_, nt, nfeat, labels, end, len(c)))
ini = open(d + 'M1033Eva.INI', 'rb').read().decode('latin-1').replace('\r', '')
sec = ini.split('[EmotionRecipe]', 1)[1].split('\n[', 1)[0] if '[EmotionRecipe]' in ini else ''
emo = [l.split('=', 1)[1].strip() for l in sec.split('\n') if l.startswith('Emotion')]
check(emo == ['happy', 'sad', 'angry'] and '[NN]' in ini, 'Eva.INI: [NN] section, [EmotionRecipe] %s' % (', '.join(emo) or 'MISSING'))
# Sarah (neural, en-GB; Windows' lower-case file names): the NNM (464 inputs, 142 outputs per frame, streams LSF / gain /
# log F0 / five-band excitation / voicing), the TDAT (version 3, 464 -> 142, 10 layers, all int16 or scaling), the HEQ
# (duration and log-F0 tables of 21 quantiles), APM 16 kHz, INI [NN] and MultiBandExcitation on, NO [EmotionRecipe]
apm = open(d + 'M2057Sarah.APM', 'rb').read()
check(apm[:4] == b'APM ' and struct.unpack_from('<I', apm, 0x28)[0] == 16000, 'Sarah.APM: "APM " header, 16000 Hz, %d bytes' % len(apm))
nnm = open(d + 'M2057Sarah.nnm', 'rb').read()
qo = struct.unpack_from('<I', nnm, 0x38)[0]
nfq = struct.unpack_from('<I', nnm, qo + 8)[0]
p_ = qo + 12 + 4 * nfq
nq = struct.unpack_from('<I', nnm, p_)[0]; p_ += 4
for _ in range(nq):
    p_ += 16 + 4 * struct.unpack_from('<I', nnm, p_ + 12)[0]
nprec = struct.unpack_from('<I', nnm, p_)[0]
mo = struct.unpack_from('<I', nnm, 0x40)[0]
ns = struct.unpack_from('<I', nnm, mo)[0]; q_ = mo + 4; types = []
for _ in range(ns):
    t_, nseg = struct.unpack_from('<II', nnm, q_); types.append(t_); q_ += 8 + 8 * nseg
check(nnm[:4] == b'NNM ' and nq == 464 and nprec == 142 and types == [1, 5, 2, 7, 4],
      'Sarah.nnm: "NNM " header, %d network inputs, %d outputs per frame, streams %s' % (nq, nprec, types))
td = open(d + 'M2057Sarah.tdat', 'rb').read()
io = struct.unpack_from('<I', td, 0x14)[0]; lt = struct.unpack_from('<I', td, 0x18)[0]
tin, tout = struct.unpack_from('<II', td, io); nl = struct.unpack_from('<I', td, lt)[0]
lin = [struct.unpack_from('<I', td, lt + struct.unpack_from('<I', td, lt + 4 + 4 * k)[0] + 0xc)[0] for k in range(nl)]
check(struct.unpack_from('<I', td, 0x10)[0] == 3 and (tin, tout, nl) == (464, 142, 10) and lin == [2] * 9 + [3],
      'Sarah.tdat: version 3, %d -> %d, %d feed-forward layers, linear types %s' % (tin, tout, nl, lin))
hq = open(d + 'M2057Sarah.heq', 'rb').read()
check(hq[:4] == b'HEQT' and struct.unpack_from('<III', hq, 0x24) == (2, 6, 21) and
      struct.unpack_from('<II', hq, 0x24 + 12 + 8 * 21) == (2, 21) and len(hq) == 0x24 + 4 + 2 * (8 + 8 * 21),
      'Sarah.heq: duration and log-F0 tables of 21 quantiles, exact size')
bep = open(d + 'M2057Sarah.bep', 'rb').read()
check(len(bep) == 22896, 'Sarah.bep: %d bytes' % len(bep))
ini = open(d + 'M2057Sarah.INI', 'rb').read().decode('latin-1').replace('\r', '')
check('[NN]' in ini and '[EmotionRecipe]' not in ini and 'Enabled=true' in ini.split('[MultiBandExcitation]', 1)[-1].split('\n[', 1)[0],
      'Sarah.INI: [NN] section, MultiBandExcitation enabled, no [EmotionRecipe] (no emotion presets)')
sys.exit(0 if ok else 1)
