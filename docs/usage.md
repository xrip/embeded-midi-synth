# GM.DLS fixed-point wavetable synthesizer — usage

A small, portable General-MIDI wavetable synth. It plays a pre-packed GM.DLS
sound bank with **no floating point on the audio path** (integer/fixed-point
only), so it runs anywhere from a desktop to a microcontroller. This document
describes the interfaces and how to integrate it, independent of any particular
platform. (For one concrete embedded port, see `docs/device-integration.md`.)

---

## 1. What it is

- **Input:** a stream of standard MIDI channel-voice messages (note on/off,
  control change, program change, pitch bend, RPN).
- **Output:** 16-bit stereo PCM, one frame at a time, at a fixed sample rate.
- **Data:** a single position-independent **sound-bank blob** you produce offline
  from a `GM.DLS` file. The engine reads samples straight out of this blob.
- **Footprint:** integer-only hot path; the voice/channel state is a few KB; the
  bank is a few MB and can live in read-only memory (flash, mmap, ROM).

This project does **not** include `GM.DLS`, `gm.dls`, generated `gm_bank.bin`, or
any Microsoft sound data. Bring your own legally usable RIFF DLS bank and keep
generated banks out of public commits unless you have redistribution rights.

Two layers, use whichever fits:

| Layer | File | What it gives you |
|-------|------|-------------------|
| **Engine** | `wavetable.inl` | The synth itself: `parse_midi`, `midi_sample_stereo`, bank binding. Portable, no platform assumptions. |
| **Glue (optional)** | `general-midi.c.inl` | A ready-made wrapper: embeds the bank, adapts naming, runs init, and exposes a mono `midi_sample()` + a `midi_cache_release()`. A convenience/example, not required. |

---

## 2. Architecture / data flow

```
   offline (build machine)              runtime (your app)
   ┌────────────────────┐               ┌──────────────────────────────┐
   │ gm.dls             │   dls_pack    │ bank blob in memory          │
   │  (DLS sound set)   │ ───────────►  │  (flash / file / ROM)        │
   └────────────────────┘  gm_bank.bin  └──────────────┬───────────────┘
                                                        │ wt_set_bank(blob)
   MIDI bytes ──► midi_command_t ──► parse_midi() ──►  engine state
                                                        │
   audio callback ◄── int16 L/R ◄── midi_sample_stereo()┘  (call per frame)
```

1. **Offline:** pack `gm.dls` into a blob for your target output rate.
2. **Init:** make the blob reachable in memory, then `wt_set_bank(blob)` once.
3. **MIDI in:** for every channel-voice message, fill a `midi_command_t` and call
   `parse_midi`.
4. **Audio out:** call `midi_sample_stereo(&l, &r)` once per output frame and push
   the samples to your sink (DAC, ring buffer, file, …).

---

## 3. The sound bank

The engine never parses `GM.DLS` at runtime; it plays a compact pre-baked blob.

Produce it with the offline packer (host tool):

```
dls_pack <gm.dls> <out.bin> <output_rate>
# e.g. dls_pack gm.dls gm_bank.bin 22050
```

- The blob is **position-independent** (every reference is a byte offset), so you
  can place it at any address: embed it in the binary, `mmap` a file, point at a
  ROM region, etc.
- It is **baked for one output rate**. The engine does **not** resample, so your
  audio sink must run at that same rate. Re-pack to change the rate.
- Place it at a **4-byte-aligned** address (fields are naturally aligned; some
  CPUs fault on unaligned access).

How you get the blob into memory is entirely up to you — read a file into a
buffer, reference a linker-embedded symbol, etc. The engine only needs a
pointer to the first byte.

Optional validation before binding:

```c
#include "gm_bank.h"
gm_bank_view_t view;
if (!gm_bank_view(blob, &view)) {
    /* wrong magic or version — do not use this blob */
}
```

---

## 4. Integrating the engine

This is a **single-translation-unit** library: include the `.inl` into the **one**
source file that owns your audio/MIDI loop, after defining a few macros.

Before the include you must provide:

| Macro | Meaning |
|-------|---------|
| `INLINE` | How the engine should mark its functions, e.g. `#define INLINE static inline`. |
| `SOUND_FREQUENCY` | Your output sample rate (part of the include contract). The engine renders at the **bank's** rate and does not resample, so pack the bank at this same rate. |
| `#include "gm_bank.h"` | The bank format/types (include it before `wavetable.inl`). |

Minimal, fully platform-agnostic integration:

```c
#include <stdint.h>
#include "gm_bank.h"

#define INLINE          static inline
#define SOUND_FREQUENCY 22050        /* must match the bank's packed rate */
#include "wavetable.inl"

/* --- call these from your application --- */

void synth_init(const void *bank_blob) {
    wt_set_bank(bank_blob);          /* bind once; also resets all state */
}

void synth_midi(uint8_t status, uint8_t data1, uint8_t data2) {
    midi_command_t m = { status, data1, data2, 0 };
    parse_midi(&m);
}

/* Fill an interleaved stereo buffer (L,R,L,R,...) at SOUND_FREQUENCY. */
void synth_render(int16_t *buf, int frames) {
    for (int i = 0; i < frames; ++i) {
        int16_t l, r;
        midi_sample_stereo(&l, &r);
        *buf++ = l;
        *buf++ = r;
    }
}
```

Because the engine's symbols have internal linkage (they follow your `INLINE`),
**everything lives in this one TU**. If other translation units need to reach the
synth, expose small non-static wrappers from this file (exactly what
`general-midi.c.inl` does, e.g. for `midi_cache_release`).

---

## 5. Public interface

All declared by `wavetable.inl` (in the including TU):

| Function | Purpose |
|----------|---------|
| `void wt_set_bank(const void *blob)` | Bind the engine to a bank blob and reset all voices/channels. Call once at startup (and again to switch banks). |
| `void parse_midi(const midi_command_t *m)` | Feed one MIDI channel-voice message. Updates engine state; produces no audio. |
| `void midi_sample_stereo(int16_t *l, int16_t *r)` | Render exactly one stereo output frame. Call once per output sample. |
| `int  wt_has_active_voices(void)` | Non-zero while any voice is sounding (useful to gate/idle the audio sink). |
| `void wt_cache_release(void)` | Free the optional RAM sample cache back to the heap (see §8). No-op if the cache is compiled out. |

Types:

```c
/* One MIDI channel-voice message. `command` is the raw status byte
   (type<<4 | channel); `note`/`velocity` are data byte 1 / 2. */
typedef struct { uint8_t command, note, velocity, other; } midi_command_t;
```

You may supply your own identically-laid-out `midi_command_t` by defining
`WT_MIDI_COMMAND_T_DEFINED` before the include.

---

## 6. MIDI input

`parse_midi` consumes one **channel-voice** message at a time. Pack the status
byte into `command`, data bytes into `note` and `velocity`:

| Message | `command` | `note` | `velocity` | Notes |
|---------|-----------|--------|-----------|-------|
| Note On | `0x90 \| ch` | key | velocity | velocity 0 ⇒ treated as Note Off |
| Note Off | `0x80 \| ch` | key | velocity | |
| Control Change | `0xB0 \| ch` | controller | value | volume, expression, pan, sustain, modulation, bank select, RPN, all-notes-off, … |
| Program Change | `0xC0 \| ch` | program | — | |
| Pitch Bend | `0xE0 \| ch` | LSB | MSB | 14-bit, centered at 8192 |

Notes:
- Channel `9` (zero-based) is the **percussion** channel (drum kit), per GM.
- **Running status / SysEx / realtime bytes:** `parse_midi` expects a complete
  channel-voice message. If your transport delivers a raw byte stream (e.g. a
  serial MIDI cable), reconstruct full messages — re-applying the last status
  byte for running status — before calling it. Forward only channel-voice
  messages; ignore/strip SysEx and realtime bytes (or handle them yourself).
- Data bytes are masked to 7 bits internally, so stray high bits are harmless.

---

## 7. Audio output

- One call to `midi_sample_stereo` = **one stereo frame**. Drive it from your
  audio clock: a DAC/I2S DMA refill, a sound-card callback, a file-writer loop,
  etc.
- Output is signed 16-bit, already mixed and master-attenuated; clamp-safe.
- For a **mono** sink, average the two channels: `(l + r) >> 1`.
- The output rate is fixed by the bank (§3). There is no internal resampler.
- Keep MIDI timing by spacing `parse_midi` calls correctly between render runs:
  advance the renderer by the number of frames that elapse between events (your
  sequencer/clock decides how many frames a tick is worth).

Sketch of an event-driven loop:

```c
for (each event in time order) {
    int frames = frames_until(event);     /* your tick→frame conversion */
    while (frames--) { int16_t l, r; midi_sample_stereo(&l, &r); sink(l, r); }
    if (event is MIDI) synth_midi(event.status, event.d1, event.d2);
    /* tempo/meta events: update your own clock */
}
```

---

## 8. The RAM sample cache (optional)

Samples are read directly from the bank blob. If that blob lives in **slow**
memory (e.g. memory-mapped flash), the per-sample reads can dominate at high
polyphony. To hide that, the engine can keep RAM copies of the sample data it is
actively using:

- On first use of a looped wave it copies it into RAM and reads from RAM
  thereafter. It is **opportunistic**: it caches as much as currently fits in the
  heap and, when an allocation fails, frees its least-recently-used wave that no
  voice is reading, then retries; otherwise that read just stays in the bank.
- It is **transparent**: the copy is byte-identical, so output is bit-for-bit the
  same whether the cache is on or off.
- It needs a heap (`malloc`/`free`). If your platform has none, or the bank
  already lives in fast RAM, compile it out:

```c
#define WT_NO_WAVE_CACHE   /* before including the engine: no cache, no malloc */
```

**Reclaiming the memory** — the cache will use spare heap and only releases it
when its own next allocation fails. If another subsystem needs RAM, call:

```c
wt_cache_release();   /* or the exported midi_cache_release() wrapper */
```

This frees the whole cache immediately. Playback is **not** interrupted — any
voice currently reading a RAM copy is repointed to the identical bank sample, and
the cache simply re-fills on demand. Call it from the same context as
`parse_midi` (see §9).

Caveat: with the cache on, allocate your other subsystems' buffers **up front**,
since the cache will otherwise occupy spare heap until something forces it to
yield.

---

## 9. Concurrency and real-time

- `wt_set_bank`, `parse_midi`, `midi_sample_stereo`, and `wt_cache_release` all
  touch shared engine state. **Serialize them** — call from one thread, or guard
  with your own lock. The common pattern is to run MIDI handling and rendering on
  the same core/thread, or to hand MIDI messages to the render thread via a queue.
- `midi_sample_stereo` does no allocation and is bounded-time. The only
  potentially heavier work is at note-on (sample lookup, and a cache copy if the
  cache is on); size your output buffer to absorb that latency.
- Nothing here uses global mutable singletons beyond the engine's own static
  state, so one engine instance per program.

---

## 10. Configuration macros (define before the include)

| Macro | Default | Effect |
|-------|---------|--------|
| `INLINE` | — (required) | Linkage/inlining qualifier for engine functions. |
| `SOUND_FREQUENCY` | — (required) | Output rate; match the bank's packed rate. |
| `WT_MAX_VOICES` | `32` | Maximum simultaneous voices (≤ 32). |
| `WT_BLOCK` | `8` | Control-rate block for pitch/LFO/envelope modulation (power of two). |
| `WT_NO_WAVE_CACHE` | (cache on) | Compile out the RAM cache entirely (no `malloc`). |
| `WT_WAVE_CACHE_SLOTS` | `512` | Size of the cache's wave-pointer table (≥ bank wave count). |
| `WT_MIDI_COMMAND_T_DEFINED` | unset | Supply your own `midi_command_t` type. |
| `WT_RAMFUNC(fn)` | identity | Hook to place the hot functions in fast memory on MCUs. |
| `WT_RUNTIME_LUTS` | unset | Build the math tables at startup instead of using the baked ones (needs libm; for validation only). |

---

## 11. Quick checklist

1. `dls_pack gm.dls gm_bank.bin <rate>` — pack the bank for your output rate.
2. Get the blob into memory; optionally validate with `gm_bank_view`.
3. In one source file: define `INLINE`, `SOUND_FREQUENCY`, include `gm_bank.h`
   then `wavetable.inl`.
4. `wt_set_bank(blob)` once.
5. `parse_midi(&msg)` for every channel-voice message.
6. `midi_sample_stereo(&l, &r)` once per output frame → your audio sink.
7. (Optional) `wt_cache_release()` when something else needs RAM;
   `-DWT_NO_WAVE_CACHE` to drop the cache at compile time.
