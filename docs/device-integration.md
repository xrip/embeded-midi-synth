# RP2040 device integration & profiling (wavetable engine)

Execute this in the emulator repo (general-midi.c.inl + I2S/DMA + CMake). The
host side (wavetable.inl, gm_bank.bin, dls_pack) is done and A/B-validated.

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

## Step 1 — Wire the engine in (DONE in general-midi.c.inl)

`general-midi.c.inl` now drives the wavetable engine and keeps the existing
`mpu401.c.inl` contract: `parse_midi(midi_command_t*)` (from wavetable.inl) and a
mono `int16_t midi_sample(void)` wrapper over `midi_sample_stereo`. It also
RAM-places the hot path (Step 2) and binds the bank via a `constructor` +
lazy-init. It needs nothing from the emulator beyond `INLINE` (and, optionally,
`__not_in_flash_func`), both of which emulator.h already provides.

What **you** do in the firmware build:
1. Generate the bank for the playback rate:
   `dls_pack gm.dls gm_bank.bin 22050` (host tool, already built).
2. The bank is embedded by an inline-asm `IMPORT_BIN(...)` `.incbin` already in
   `general-midi.c.inl` (no separate `.S`, no `enable_language(ASM)`). You only
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

Sanity: it should sound like the host `wt_render` output (mono sum).

Notes:
- `midi_command_t` is supplied by wavetable.inl (command/note/velocity/other),
  matching the `uint32` `mpu401.c.inl` casts. If the emulator defines its own,
  set `WT_MIDI_COMMAND_T_DEFINED` and match the layout.
- Rate: the engine does not resample, so pitch/tempo are correct only when the
  output rate equals the bank's `output_rate`. Repack at your rate when you tune
  it (no code change).
- For true stereo + DLS pan, switch the emulator's audio mix from `midi_sample()`
  to `midi_sample_stereo(&l,&r)`.

## Step 2 — RAM-place the hot path (DONE; the #1 win)

`general-midi.c.inl` defines `WT_RAMFUNC(name) -> __not_in_flash_func(name)` when
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

## Step 4 — PCM cache pressure (CONDITIONAL on Step 3)

Only if measured. Options, cheapest first:
- **Loop-region RAM cache**: when a *looped* voice starts, copy its loop window
  (loop_start..loop_end, often small) into a RAM pool; point `v->pcm` at it.
  Fixes the sustained-instrument thrash without holding the whole bank.
- **Hot-instrument relocation**: copy the few most-used programs (piano, drum
  kit) to RAM at boot/first-use. ~150–200 KB SRAM free; bank is 3 MB so this is
  partial by design.
- Re-measure hit-rate after.

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
DMA-fed I2S consumes audio. Can be added to wavetable.inl now and A/B-checked on
the host (must stay bit-exact vs per-sample `midi_sample_stereo`).
