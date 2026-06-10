#!/usr/bin/env python3
# Locate where two 16-bit WAVs first diverge (left channel).
#   python tools/wav_diff.py a.wav b.wav
import sys, wave, struct

def load(p):
    w = wave.open(p, 'rb'); ch = w.getnchannels(); fr = w.getframerate()
    n = w.getnframes(); raw = w.readframes(n); w.close()
    s = struct.unpack('<%dh' % (len(raw)//2), raw)
    return list(s[0::ch]), fr

a, fr = load(sys.argv[1])
b, _ = load(sys.argv[2])
n = min(len(a), len(b))
first = None; big = 0; peak = 0
for i in range(n):
    d = a[i] - b[i]
    if d and first is None: first = i
    if abs(d) > 30: big += 1
    if abs(d) > peak: peak = abs(d)
print("samples:", n, "first diff:", first,
      ("t=%.3fs" % (first/fr)) if first is not None else "")
print("count |d|>30:", big, " peak |d|:", peak)
if first is not None:
    for i in range(first, min(first+6, n)):
        print("  i=%d a=%d b=%d d=%d" % (i, a[i], b[i], a[i]-b[i]))
