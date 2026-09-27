#!/usr/bin/env python3
"""List the storages/streams inside an OLE compound file (read-only, no dependencies)."""
import struct, sys

def load(path):
    d = open(path, 'rb').read()
    assert d[:8] == bytes.fromhex('d0cf11e0a1b11ae1')
    ssz = 1 << struct.unpack_from('<H', d, 30)[0]
    mssz = 1 << struct.unpack_from('<H', d, 32)[0]
    nfat, dir0, _, cutoff, mfat0, nmfat, difat0, ndifat = struct.unpack_from('<IIIIIIII', d, 44)
    difat = list(struct.unpack_from('<109I', d, 76))
    s = difat0
    while s < 0xFFFFFFFA and ndifat:
        off = 512 + s * ssz
        v = struct.unpack_from('<%dI' % (ssz // 4), d, off)
        difat += v[:-1]; s = v[-1]; ndifat -= 1
    fat = []
    for fs in difat[:nfat]:
        fat += struct.unpack_from('<%dI' % (ssz // 4), d, 512 + fs * ssz)
    def chain(s, table):
        out = []
        while s < 0xFFFFFFFA:
            out.append(s); s = table[s]
        return out
    def read(s, size=None):
        b = b''.join(d[512 + x * ssz: 512 + (x + 1) * ssz] for x in chain(s, fat))
        return b if size is None else b[:size]
    dirs = read(dir0)
    ents = []
    for i in range(len(dirs) // 128):
        e = dirs[i * 128:(i + 1) * 128]
        nl = struct.unpack_from('<H', e, 64)[0]
        name = e[:max(nl - 2, 0)].decode('utf-16le')
        typ = e[66]
        l, r, ch = struct.unpack_from('<III', e, 68)
        start, size = struct.unpack_from('<II', e, 116)
        ents.append(dict(i=i, name=name, type=typ, left=l, right=r, child=ch, start=start, size=size))
    mfat = []
    for x in chain(mfat0, fat) if mfat0 < 0xFFFFFFFA else []:
        mfat += struct.unpack_from('<%dI' % (ssz // 4), d, 512 + x * ssz)
    root = ents[0]
    ministream = read(root['start'], root['size']) if root['start'] < 0xFFFFFFFA else b''
    def stream(e):
        if e['size'] < cutoff:
            return b''.join(ministream[x * mssz:(x + 1) * mssz] for x in chain(e['start'], mfat))[:e['size']]
        return read(e['start'], e['size'])
    return ents, stream

def walk(ents, idx, depth, out):
    if idx >= 0xFFFFFFFA or idx >= len(ents): return
    e = ents[idx]
    walk(ents, e['left'], depth, out)
    out.append((depth, e))
    if e['type'] in (1, 5): walk(ents, e['child'], depth + 1, out)
    walk(ents, e['right'], depth, out)

if __name__ == '__main__':
    ents, stream = load(sys.argv[1])
    out = []
    root = ents[0]
    print('Root', root['size'])
    walk(ents, root['child'], 1, out)
    for depth, e in out:
        kind = {1: 'storage', 2: 'stream'}.get(e['type'], str(e['type']))
        print('  ' * depth + '%s [%s] %d' % (e['name'], kind, e['size']))
