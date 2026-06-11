# Bank census: real per-wave / per-instrument PCM sizes from a packed gm_bank.bin.
# Answers the RAM-cache feasibility question with numbers, not guesses.
#   python tools/bank_census.py build/gm_bank.bin
import sys, struct

path = sys.argv[1] if len(sys.argv) > 1 else "build/gm_bank.bin"
blob = open(path, "rb").read()

# header (see gm_bank.h gm_bank_header_t, 44 bytes, little-endian)
(magic, ver, rate, ninst, nreg, nwav,
 off_i, off_r, off_w, off_pcm, pcm_samples) = struct.unpack_from("<4sIIIIIIIIII", blob, 0)
assert magic == b"GMWB", magic
print(f"bank: ver{ver} rate={rate} inst={ninst} reg={nreg} wav={nwav} "
      f"pcm={pcm_samples} samp ({pcm_samples*2} B)")

# waves: {pcm_offset, frame_count, base_step_q16} 12B
waves = []
for i in range(nwav):
    po, fc, bs = struct.unpack_from("<III", blob, off_w + i*12)
    waves.append((po, fc, bs))

# regions: 80B; we need wave_index(@offset 64,u16), loop fields, flags(@79,u8),
# loop_start(@20 u32), loop_length(@24 u32). Recompute offsets from gm_region_t.
# layout: 13*u32 (0..51) then 5*u32? -> re-derive precisely:
# u32 gain,att,dec,rel,sus,loop_start,loop_length,lfo_phase_inc,lfo_delay (9 u32 =36)
# i32 lfo_depth,lfo_mod,lfo_gain (3 =12) -> 48
# u32 eg2_att,eg2_dec,eg2_rel,eg2_sus (4 =16) -> 64
# i32 eg2_pitch (4) -> 68
# i16 fine; u16 wave_index -> 72 ; then 8 bytes of u8 fields incl flags last -> 80
REG = 80
def region(i):
    base = off_r + i*REG
    loop_start  = struct.unpack_from("<I", blob, base+20)[0]
    loop_length = struct.unpack_from("<I", blob, base+24)[0]
    wave_index  = struct.unpack_from("<H", blob, base+70)[0]
    flags       = blob[base+79]
    return wave_index, loop_start, loop_length, flags

GM_RGN_LOOPED = 0x01

# per-wave bytes; classify looped vs one-shot by ANY region referencing it
wave_bytes = [fc*2 for (po,fc,bs) in waves]
wave_looped = [False]*nwav
wave_loopwin = [0]*nwav   # bytes of 0..loop_end (what a sustained voice actually re-reads)
for i in range(nreg):
    wi, ls, ll, fl = region(i)
    if wi < nwav and (fl & GM_RGN_LOOPED):
        wave_looped[wi] = True
        loop_end = ls + ll
        win = min(loop_end, waves[wi][1]) * 2
        wave_loopwin[wi] = max(wave_loopwin[wi], win)

def stats(xs):
    xs = sorted(xs)
    n = len(xs)
    tot = sum(xs)
    def pct(p): return xs[min(n-1, int(p*n))]
    return n, tot, xs[0], pct(.5), pct(.9), xs[-1]

def show(label, xs):
    n,tot,mn,med,p90,mx = stats(xs)
    print(f"{label:22} n={n:4} tot={tot/1024:7.1f}KB  min={mn:6} "
          f"med={med:6} p90={p90:7} max={mx:7} (bytes)")

print("\n-- per-wave full PCM size --")
show("all waves", wave_bytes)
show("looped waves", [wave_bytes[i] for i in range(nwav) if wave_looped[i]])
show("one-shot waves", [wave_bytes[i] for i in range(nwav) if not wave_looped[i]])
print("\n-- looped: loop-window (0..loop_end), the re-read region --")
show("loop windows", [wave_loopwin[i] for i in range(nwav) if wave_looped[i]])

# per-instrument: sum of UNIQUE waves it can address (across its regions)
print("\n-- per-instrument total wave bytes (unique waves) --")
inst_tot = []
inst_nwav = []
for i in range(ninst):
    bank, region_first, prog, rcount = struct.unpack_from("<IIHH", blob, off_i + i*12)
    wset = set()
    for r in range(region_first, region_first+rcount):
        wi = region(r)[0]
        if wi < nwav: wset.add(wi)
    inst_tot.append(sum(wave_bytes[w] for w in wset))
    inst_nwav.append(len(wset))
show("instrument PCM", inst_tot)
n,tot,mn,med,p90,mx = stats(inst_nwav)
print(f"{'waves/instrument':22} n={n:4}            min={mn:6} med={med:6} p90={p90:7} max={mx:7}")

# RAM budget scenarios
print("\n-- RAM budget for N most-resident waves (full PCM) --")
sw = sorted(wave_bytes)
for N in (16, 24, 32, 48, 64):
    big = sum(sw[-N:]) if N <= nwav else sum(sw)
    print(f"  top-{N:3} largest waves = {big/1024:7.1f}KB ; "
          f"any-{N:3} avg-case ~ {sum(sw)/nwav*N/1024:7.1f}KB")
