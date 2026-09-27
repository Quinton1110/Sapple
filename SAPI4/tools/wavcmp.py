#!/usr/bin/env python3
"""Compare two 16-bit mono WAVs: duration, identical samples, max diff, correlation."""
import struct, sys, wave, math
def load(p):
    w = wave.open(p, 'rb'); n = w.getnframes(); d = w.readframes(n); r = w.getframerate(); w.close()
    return list(struct.unpack('<%dh' % n, d)), r
a, ra = load(sys.argv[1]); b, rb = load(sys.argv[2])
n = min(len(a), len(b))
same = sum(1 for i in range(n) if a[i] == b[i])
diff = [abs(a[i] - b[i]) for i in range(n)]
num = sum(a[i] * b[i] for i in range(n)); den = math.sqrt(sum(x * x for x in a[:n]) * sum(x * x for x in b[:n]))
print('%s: %.3f s @%d | %s: %.3f s @%d | identical %d/%d (%.2f%%) | max|diff| %d | corr %.6f' % (
    sys.argv[1], len(a) / ra, ra, sys.argv[2], len(b) / rb, rb, same, n, 100.0 * same / max(n, 1), max(diff) if diff else 0, num / den if den else 0))
