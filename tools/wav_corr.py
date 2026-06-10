#!/usr/bin/env python3
# A/B WAV comparator for the wavetable engine vs golden reference.
#   python tools/wav_corr.py a.wav b.wav
# Prints per-channel Pearson correlation and RMS-error-in-dBFS over the
# overlapping region. Mono refs are compared against L of a stereo file.
import sys, wave, struct

def load(path):
    w = wave.open(path, 'rb')
    ch, sw, fr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(n); w.close()
    assert sw == 2, f"{path}: expected 16-bit"
    smp = struct.unpack('<%dh' % (len(raw)//2), raw)
    if ch == 1:
        return [list(smp)], fr
    return [list(smp[0::ch]), list(smp[1::ch])], fr

def corr(x, y):
    n = min(len(x), len(y))
    if n == 0: return 0.0, 0.0
    x, y = x[:n], y[:n]
    mx = sum(x)/n; my = sum(y)/n
    sxx = sxy = syy = 0.0
    se = 0.0
    for i in range(n):
        dx = x[i]-mx; dy = y[i]-my
        sxx += dx*dx; syy += dy*dy; sxy += dx*dy
        d = x[i]-y[i]; se += d*d
    c = sxy/((sxx*syy)**0.5) if sxx>0 and syy>0 else 0.0
    rms = (se/n)**0.5
    dbfs = 20*__import__('math').log10(rms/32768.0) if rms>0 else -999.0
    return c, dbfs

def main():
    a, fa = load(sys.argv[1])
    b, fb = load(sys.argv[2])
    nch = min(len(a), len(b))
    for ci in range(nch):
        c, db = corr(a[ci], b[ci])
        print(f"  ch{ci}: corr={c:.4f}  rmserr={db:6.1f} dBFS")

if __name__ == '__main__':
    main()
