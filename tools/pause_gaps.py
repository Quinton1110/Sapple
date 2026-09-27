#!/usr/bin/env python3
"""Measure silent gaps inside a speech WAV (16-bit or float32, mono or first channel).

Usage: tools/pause_gaps.py a.wav b.wav ... (the pause fix, 2026-09-22: Mac harness renders in samples/ssml,
self-test WAVs pulled from the phone, Apple reference voices).

For each file: total duration, leading/trailing silence, and every silent run inside the speech
(between the first and last loud frame) that is at least MIN_GAP ms long. Silence = 5 ms frames whose
RMS is more than REL dB below the loudest frame (and below an absolute floor)."""
import struct, sys, wave, math

FRAME_MS = 5
REL_DB = 40.0
MIN_GAP_MS = 40


def read(path):
    with open(path, 'rb') as f:
        data = f.read()
    # minimal RIFF parser (handles float32 and WAVE_FORMAT_EXTENSIBLE, which the wave module rejects)
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE', path
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack('<I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            tag, ch, rate, _, _, bits = struct.unpack('<HHIIHH', body[:16])
            if tag == 0xFFFE:
                tag = struct.unpack('<H', body[24:26])[0]
            fmt = (tag, ch, rate, bits)
        elif cid == b'data':
            pcm = body
        pos += 8 + size + (size & 1)
    tag, ch, rate, bits = fmt
    if tag == 3 and bits == 32:
        n = len(pcm) // 4
        vals = struct.unpack('<%df' % n, pcm[:n * 4])
        s = [v * 32768.0 for v in vals[::ch]]
    else:
        n = len(pcm) // 2
        vals = struct.unpack('<%dh' % n, pcm[:n * 2])
        s = list(vals[::ch])
    return rate, s


def analyse(path):
    rate, s = read(path)
    fl = max(1, int(rate * FRAME_MS / 1000))
    rms = []
    for i in range(0, len(s) - fl + 1, fl):
        seg = s[i:i + fl]
        m = sum(abs(x) for x in seg) / fl
        mean = sum(seg) / fl
        r = math.sqrt(sum((x - mean) ** 2 for x in seg) / fl)
        rms.append(20 * math.log10(r + 1e-9))
    if not rms:
        return dict(dur=0)
    peak = max(rms)
    thr = max(peak - REL_DB, 20 * math.log10(250))  # also below -42 dBFS (the trimmer's END_FLOOR)
    loud = [r > thr for r in rms]
    if not any(loud):
        return dict(dur=len(s) / rate, gaps=[])
    first = loud.index(True)
    last = len(loud) - 1 - loud[::-1].index(True)
    gaps, run = [], 0
    for i in range(first, last + 1):
        if not loud[i]:
            run += 1
        else:
            if run * FRAME_MS >= MIN_GAP_MS:
                gaps.append(((i - run) * FRAME_MS, run * FRAME_MS))
            run = 0
    return dict(dur=len(s) / rate, lead=first * FRAME_MS, tail=(len(rms) - 1 - last) * FRAME_MS,
                speech=(last - first + 1) * FRAME_MS, gaps=gaps)


if __name__ == '__main__':
    for p in sys.argv[1:]:
        a = analyse(p)
        gaps = a.get('gaps', [])
        big = max((g[1] for g in gaps), default=0)
        print('%-44s dur %5.0f ms  lead %4d  tail %4d  longest gap %4d ms  gaps %s' % (
            p.split('/')[-1], a['dur'] * 1000, a.get('lead', 0), a.get('tail', 0), big,
            ' '.join('%d@%d' % (g[1], g[0]) for g in gaps)))
