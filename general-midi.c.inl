#pragma GCC optimize("Ofast")
#pragma once
//
// General MIDI synthesizer — GM.DLS fixed-point wavetable engine.
//
// Replaces the old synthesized voices (sine + LFSR noise) with the real GM.DLS
// wavetable bank. All the DSP lives in wavetable.inl (float-free, Cortex-M0+
// tuned: no int64/division on the per-sample path, baked LUTs, voice bitmask).
// This file is only the glue mpu401.c.inl / the emulator expect:
//
//   * void    parse_midi(const midi_command_t *)   - from wavetable.inl
//   * int16_t midi_sample(void)                     - mono wrapper, defined here
//
// Provided by the includer (emulator.h): INLINE, and optionally
// __not_in_flash_func (RAM-places the hot path; big win since the 16 KB XIP
// cache is shared between code and the flash-resident PCM).
//
// The packed bank is embedded in flash by gm_bank.S (.incbin gm_bank.bin) and
// must be generated for the output rate:  dls_pack gm.dls gm_bank.bin <rate>.
// The engine does NOT resample to SOUND_FREQUENCY — pack the bank at the actual
// playback rate (default 22050) or pitch/tempo will be off. See
// docs/device-integration.md.

#include "gm_bank.h"

#define WT_MAX_VOICES 32
#ifndef WT_RAMFUNC
#ifdef __not_in_flash_func
#define WT_RAMFUNC(name) __not_in_flash_func(name)   // keep the render loop in RAM
#endif
#endif

// wavetable.inl marks its functions `INLINE foo` expecting INLINE == `static
// inline`. The emulator's INLINE is just `inline` (its code writes `static
// INLINE`), which would give wavetable external-linkage inlines. Bridge it for
// the wavetable include only, then restore the emulator's INLINE so the rest of
// the TU (mpu401.c.inl's `static INLINE ...`) keeps compiling.
#pragma push_macro("INLINE")
#undef INLINE
#define INLINE static inline
#include "wavetable.inl"   // parse_midi, midi_sample_stereo, wt_set_bank, midi_command_t
#pragma pop_macro("INLINE")

// The packed soundbank, embedded in flash by gm_bank.S (.incbin, 4-byte aligned).
extern const uint8_t gm_bank_blob[];

static int wt_bank_ready = 0;

// Bind the engine to the flash bank. Runs before main() via the C runtime init
// array on the Pico; the lazy guard in midi_sample() is a fallback for runtimes
// that do not run constructors.
static void __attribute__((constructor)) gm_wavetable_init(void) {
    wt_set_bank(gm_bank_blob);
    wt_bank_ready = 1;
}

// Mono sink kept for the current caller: sum the stereo render back to center.
// For true stereo + DLS pan, switch the emulator's audio mix to
// midi_sample_stereo(&l, &r).
static INLINE int16_t midi_sample(void) {
    if (__builtin_expect(!wt_bank_ready, 0)) {
        wt_set_bank(gm_bank_blob);
        wt_bank_ready = 1;
    }
    int16_t l, r;
    midi_sample_stereo(&l, &r);
    return (int16_t) (((int32_t) l + (int32_t) r) >> 1);
}
