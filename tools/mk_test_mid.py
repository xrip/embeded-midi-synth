#!/usr/bin/env python3
# Emit a minimal SMF: program change + one long sustained note, to exercise
# DLS LFO vibrato / EG2 pitch (which only engage on held notes past the LFO
# delay).  python tools/mk_test_mid.py out.mid <program> <note> <seconds>
import sys, struct

def vlq(n):
    b = [n & 0x7f]; n >>= 7
    while n: b.insert(0, (n & 0x7f) | 0x80); n >>= 7
    return bytes(b)

out, prog, note, secs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4])
mod = int(sys.argv[5]) if len(sys.argv) > 5 else 0   # CC1 mod-wheel (drives DLS LFO)
div = 480; tempo = 500000                      # 120 bpm -> 480 ticks = 0.5 s
ticks = int(secs * div * 1e6 / tempo)
trk = b''
trk += b'\x00' + bytes([0xC0, prog & 0x7f])    # program change ch0
if mod:
    trk += b'\x00' + bytes([0xB0, 0x01, mod & 0x7f])  # CC1 modulation
trk += b'\x00' + bytes([0x90, note & 0x7f, 100])  # note on
trk += vlq(ticks) + bytes([0x80, note & 0x7f, 0]) # note off
trk += b'\x00' + bytes([0xFF, 0x2F, 0x00])     # end of track
hdr = b'MThd' + struct.pack('>IHHH', 6, 0, 1, div)
chunk = b'MTrk' + struct.pack('>I', len(trk)) + trk
open(out, 'wb').write(hdr + chunk)
print(f"wrote {out}: prog={prog} note={note} {secs}s ({ticks} ticks)")
