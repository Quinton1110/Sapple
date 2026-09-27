#!/usr/bin/env python3
"""Print '<sha1 of PCM frames> <samples> <file>' for each WAV argument (header-independent)."""
import hashlib, sys, wave
for p in sys.argv[1:]:
    w = wave.open(p, 'rb'); d = w.readframes(w.getnframes()); n = w.getnframes(); w.close()
    print(hashlib.sha1(d).hexdigest()[:16], n, p.split('/')[-1])
