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

## Step 1 — Wire the engine in (functional first, no opt)

Includer contract (general-midi.c.inl):
```c
#define INLINE static inline
#define SOUND_FREQUENCY 22050           // must equal the bank's output_rate
#define WT_MAX_VOICES   32
// #define WT_BLOCK 8                    // default; control-rate modulation
#include "gm_bank.h"
#include "wavetable.inl"
```
- `.incbin` `gm_bank.bin` at a **4-byte-aligned flash address**, NOT
  `__not_in_flash` (it's 3 MB — must stay in flash):
  ```c
  extern const uint8_t gm_bank_blob[];  // from an .S: .incbin, .balign 4
  ```
- Boot once: `wt_set_bank(gm_bank_blob);` (fills the static `g_bank`, builds/uses
  LUTs). Verify `gm_bank_view()` magic/version match (v4).
- MIDI in: route each MPU-401/USB-MIDI message to `parse_midi(&cmd)`. Reconcile
  `midi_command_t` — wavetable.inl defines its own (command/note/velocity/other)
  unless `WT_MIDI_COMMAND_T_DEFINED`; if the emulator already has one, define that
  macro and match the layout.
- Audio out: from the I2S DMA refill, call `midi_sample_stereo(&l,&r)` per frame
  (or the block API in Step 4) and write the buffer.

Sanity: play a MIDI, confirm it sounds like the host `wt_render` output.

## Step 2 — RAM-place the hot path (the #1 win, do unconditionally)

```c
#define WT_RAMFUNC(name) __not_in_flash_func(name)   // before #include "wavetable.inl"
```
This already wraps `midi_sample_stereo` and `parse_midi`. Also mark the I2S
refill/callback `__not_in_flash_func`. `g_voices`/`g_channels` are plain statics
(already RAM). LUTs are `const` (flash) but read only at control/note-on rate —
leave them in flash; relocate to RAM only if Step 3 shows it matters (costs ~8 KB
SRAM).

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
