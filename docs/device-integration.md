# RP2040 device integration & profiling (wavetable engine)

Execute this in the emulator repo by copy/adapting
`examples/rp2040/general-midi.c.inl` alongside the I2S/DMA and CMake integration.
The host side (wavetable.c.inl, gm_bank.bin, dls_pack) is done and A/B-validated.

## Premise (why this list is short)

Target: 400 MHz, 32 voices, 22050 Hz stereo. That is **567 cycles/voice/sample**
against ~55 cycles of cache-hit compute → ~10x headroom. After the int64/division
removal, **compute is not the constraint**. The only thing that can break
real-time is **flash (XIP) stalls**. So: place hot code in RAM, then measure;
ignore arithmetic micro-opts.

Key hardware fact: the 16 KB XIP cache is **shared between instruction fetch and
flash data** (our PCM is `.incbin`'d in flash). 32 voices streaming PCM evict the
render code from cache and vice-versa. Moving code to RAM removes that contention
*and* dedicates the whole cache to PCM.

## Step 1 — Wire the engine in (DONE in examples/rp2040/general-midi.c.inl)

`examples/rp2040/general-midi.c.inl` drives the wavetable engine and keeps the
existing `mpu401.c.inl` contract: `parse_midi(midi_command_t*)` (from
wavetable.c.inl) and a mono `int16_t midi_sample(void)` wrapper over
`midi_sample_stereo`. It also RAM-places the hot path (Step 2) and binds the bank
via a `constructor` + lazy-init. It needs nothing from the emulator beyond
`INLINE` (and, optionally, `__not_in_flash_func`), both of which emulator.h
already provides.

What **you** do in the firmware build:
1. Generate the bank for the playback rate:
   `dls_pack gm.dls gm_bank.bin 22050` (host tool, already built).
2. The bank is embedded by an inline-asm `IMPORT_BIN(...)` `.incbin` already in
   `examples/rp2040/general-midi.c.inl` (no separate `.S`, no `enable_language(ASM)`). You only
   need to make `gm_bank.bin` reachable by the assembler when that TU compiles —
   pass the directory via `-Wa,-I`, and relink when the bank changes:
   ```cmake
   set_source_files_properties(emulator/audio/mpu401.c.inl PROPERTIES
       COMPILE_OPTIONS "-Wa,-I${CMAKE_CURRENT_SOURCE_DIR}/emulator/audio"
       OBJECT_DEPENDS  "${CMAKE_CURRENT_SOURCE_DIR}/emulator/audio/gm_bank.bin")
   ```
   (`mpu401.c.inl` is the TU that includes `general-midi.c.inl`; adjust to whatever
   `.c` actually compiles it. Alternatively put an absolute path in the
   `IMPORT_BIN("gm_bank.bin", ...)` call.)
3. **Flash size**: the bank is ~3 MB; ensure the board/linker allows it
   (`PICO_FLASH_SIZE_BYTES` / a >=4 MB flash). Default Pico is 2 MB — too small.
4. Build, flash, play. The `constructor` calls `wt_set_bank(gm_bank_blob)` before
   `main`; audio comes out mono via the existing `midi_sample()` caller.

To supply the bank some other way (separate `.c`/`.S`, or the host self-check),
define `WT_BANK_EXTERN` before the include and provide `gm_bank_blob` yourself.

Sanity: it should sound like the host `examples/wt_render.c` output (mono sum).

Notes:
- `midi_command_t` is supplied by wavetable.c.inl (command/note/velocity/other),
  matching the `uint32` `examples/rp2040/mpu401.c.inl` casts. If the emulator defines its own,
  set `WT_MIDI_COMMAND_T_DEFINED` and match the layout.
- Rate: the engine does not resample, so pitch/tempo are correct only when the
  output rate equals the bank's `output_rate`. Repack at your rate when you tune
  it (no code change).
- For true stereo + DLS pan, switch the emulator's audio mix from `midi_sample()`
  to `midi_sample_stereo(&l,&r)`.

## Step 2 — RAM-place the hot path (DONE; the #1 win)

`examples/rp2040/general-midi.c.inl` defines `WT_RAMFUNC(name) -> __not_in_flash_func(name)` when
the SDK macro exists, so `midi_sample_stereo` and `parse_midi` are RAM-resident.
Also mark the **I2S refill/callback** `__not_in_flash_func` on your side.
`g_voices`/`g_channels` are plain statics (already RAM). LUTs are `const` (flash)
but read only at control/note-on rate — leave them in flash; relocate to RAM only
if Step 3 shows it matters (~8 KB SRAM).

Result: the per-sample loop runs entirely from SRAM except the two PCM reads, and
the XIP cache is dedicated to PCM.

## Step 3 — Measure before doing anything else

Two numbers decide whether you're already done:

1. **Cycles/sample** (headroom). M0+ has no DWT cycle counter, and SysTick is
   only 24-bit (wraps in ~42k frames at 400 MHz), so use the free-running 1 MHz
   timer and convert via the clock:
   ```c
   uint64_t us0 = time_us_64();
   for (int i=0;i<20000;i++) midi_sample_stereo(&l,&r);  // with N voices held
   uint64_t us = time_us_64() - us0;
   float cyc_per_frame = (float)us * (clock_hz/1e6f) / 20000.0f;
   ```
   Budget at 400 MHz = 18140 cyc/frame. Report cyc at 8/16/24/32 held voices.
   (Run this from RAM and with PCM actually streaming, or the number is fiction.)

2. **XIP PCM hit-rate** (the real question): RP2040 `XIP_CTRL` has hit/access
   counters.
   ```c
   xip_ctrl_hw->ctr_hit = 0; xip_ctrl_hw->ctr_acc = 0;   // reset
   /* render a few thousand samples at high polyphony */
   float hit = (float)xip_ctrl_hw->ctr_hit / xip_ctrl_hw->ctr_acc;
   ```

Decision gates:
- cyc/frame comfortably under budget AND hit-rate high (say >90%) → **stop, you're
  done.** Don't build Step 4.
- hit-rate low / cyc near budget at 32 voices → Step 4.

## Step 4 — RAM wave cache (IMPLEMENTED, on by default)

`wavetable.c.inl` keeps one pointer per `wave_index`: the first time a **looped**
wave plays it `malloc`s a RAM copy, stores the pointer and reads from RAM ever
after, so the two per-sample `pcm[]` reads hit RAM instead of XIP flash. It is
**opportunistic with no budget** — it caches as many waves as fit in the heap
*right now*; when a `malloc` fails it `free`s the **least-recently-used wave that
no sounding voice is still reading** ("in use" is read live from the active-voice
mask — no refcount, no teardown hooks) and retries, and only if nothing is
evictable does that read stay on flash. So a fresh song transparently replaces a
stale instrument set, and RAM use scales with the live working set with **no fixed
reservation**. **Transparent and bit-exact** — the copy is byte-identical; a
cache-off build renders the same samples. One-shot (drum) waves are never cached.

One switch, nothing to tune:
- **on by default** — `examples/rp2040/general-midi.c.inl` needs no define.
- `-DWT_NO_WAVE_CACHE` — **compile it out entirely**: no pointer table, no LRU
  code, no `<stdlib.h>`/`malloc`; every read goes straight to flash. For targets
  with no spare RAM — not a single allocation is ever attempted. Verified: zero
  cache symbols in the binary, output bit-identical to cache-off.
- (`-DWT_WAVE_CACHE_SLOTS=N`, default 512, only sizes the pointer table; raise it
  only for a bank with more than 512 waves. Not a memory tuning knob.)

Caveats of the no-budget model:
- **Greedy heap.** The cache will use all currently-free heap, freeing waves only
  when its own next `malloc` fails. Allocate every *other* subsystem's buffers
  (USB, DMA, etc.) up front, before the synth runs, so the cache only consumes
  what is genuinely spare. A later large allocation by another subsystem can fail
  if the cache holds the RAM (the cache does not pre-emptively yield) — for that,
  call the release valve below to reclaim it.
- **Fragmentation** of the shared heap is possible on a long-running device; the
  failure mode is graceful (a wave stays on flash), never a crash.
- The **eviction path only triggers under real memory pressure**, so it is not
  exercised by the host A/B (host `malloc` never fails — it just caches the whole
  looped set); it is small and the in-use check is its only safety-critical part.

**Release valve** — `void midi_cache_release(void)` (exported from
`examples/rp2040/general-midi.c.inl`) frees the entire cache back to the heap on demand, e.g. when
another subsystem needs RAM. MIDI keeps playing: any voice on a RAM copy is
repointed to the byte-identical flash PCM (same position, no click) and the cache
re-fills on demand. No-op under `-DWT_NO_WAVE_CACHE`. Call it from the MIDI/audio
context (not concurrently with the render). Proven seamless on host:
`wt_render -DWT_TEST_RELEASE` drops the cache every render block during playback
and stays bit-exact vs cache-off.

Measured (host A/B census, `tools/bank_census.py` + `wt_render` with
`-DWT_WAVE_CACHE_PROFILE`): bank 495 waves, 89 % looped, median 4 KB / max 36 KB.
With the cache on, **per-sample reads served from RAM: 90–94 %** (Doom-E1M1 /
d_dm2int / dott / omf); the residual ~6–10 % are one-shot drums. On the device the
resident set is bounded by free heap (LRU churns the rest); host has room for the
whole ~485 KB looped set so it caches all of it.

Validate any change on host (must stay bit-exact + report RAM share):
```
powershell -Command "& ./build.ps1 -Target wt_render \
  -Define WT_NO_WAVE_CACHE -OutName wt_render_off"            # reference (cache off)
powershell -Command "& ./build.ps1 -Target wt_render \
  -Define WT_WAVE_CACHE_PROFILE -OutName wt_render_cache"     # cache on (default) + profile
# render both, cmp -s the WAVs (bit-exact), read the WAVECACHE line on stderr
```

### Real-time budget: copy keeps up with the MIDI wire

A miss does a `malloc` + synchronous `memcpy` flash→RAM in note-on handling (the
`malloc` is a cheap cold-path cost dwarfed by the copy); this must not fall behind
the input. MIDI is 31250 baud / 8N1 = 3125 B/s = **320 µs per byte**;
the fastest note-on (running-status, 2 B) is **640 µs** apart, a program change +
note (5 B) 1600 µs. RP2040 quad XIP reads ~33–45 MB/s; take a conservative
**24 MB/s**. Then **during one 2-byte note-on's own 640 µs we can copy 15 KB** —
which is the 90th-percentile wave. So ≥90 % of waves finish copying within the
note-on that requested them; no backlog accumulates even at full wire speed. Copy
times: median 4 KB → 167 µs, p90 15 KB → 629 µs, **max 36 KB → 1.5 ms** (worst
single-copy latency, absorbed by any DMA audio buffer ≥ 1.5 ms; a 64-frame buffer
@22050 is 2.9 ms). Aggregate: filling the whole 2.7 MB looped set = 115 ms of
copying, but requesting all 439 distinct waves takes ≥ 281 ms of wire → ~41 % copy
duty, then everything is resident. The only stream that can outpace copy is a
deliberate loop over the ~44 largest (>15 KB) waves forcing eviction every hit
(~31 MB/s demand) — not musical, and it degrades gracefully (UART RX buffer + the
flash fallback in `wt_wave_base`, no glitch). Hardening lever if ever needed:
issue the copy on a DMA channel (RP2040 does flash→RAM DMA) and let the voice read
from flash until the DMA completes, then swap `v->pcm` — removes copy latency from
the audio path entirely.

Still device-conditional, if Step 3 shows pressure even with the cache:
- **Whole-bank in PSRAM**: copy the 3 MB bank to PSRAM at boot and `wt_set_bank()`
  that copy — bypasses the cache, makes XIP irrelevant.

## Explicitly NOT doing (measured low/no value)

- **INTERP0 lerp** — saves ~2 single-cycle MULS/voice (noise vs a flash miss) and
  its 8-bit alpha is coarser than our 16-bit frac (quality loss).
- **SIO hardware divider** — every divisor is a compile-time constant the compiler
  already reciprocates; nothing variable to accelerate.
- **midi_sample_mono** — output is stereo; mono would drop the DLS region pan
  (433 regions) to save ~2 cycles.
- **Dual-core (core1 render)** — real lever, but premature at 400 MHz where one
  core has ~10x headroom. Revisit only if Step 3 shows core0 starved by
  USB/sequencing.

## Recommended portable addition (host-validatable, optional)

`midi_render_block(int16_t *buf, int frames)` — fill an interleaved stereo buffer
in one call instead of per-sample. Better register/cache locality and matches how
DMA-fed I2S consumes audio. Can be added to wavetable.c.inl now and A/B-checked on
the host (must stay bit-exact vs per-sample `midi_sample_stereo`).
