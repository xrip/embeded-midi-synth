# Embedded GM.DLS Wavetable Synth

Fixed-point General MIDI wavetable synthesis for embedded targets.

This repository contains a small C wavetable engine that plays a pre-packed RIFF
DLS General MIDI bank. The runtime audio path is integer/fixed-point only: no
floating point, no DLS parsing, and no dynamic instrument metadata work during
rendering. The intended flow is:

```text
GM-compatible .dls bank -> dls_pack -> compact bank blob -> embedded synth
```

The current integration target is RP2040-class hardware, but the engine itself is
plain single-translation-unit C and only needs a pointer to a packed bank blob.

## Why It Exists

- Use a real wavetable bank instead of simple generated waveforms.
- Keep the real-time path small enough for microcontrollers.
- Store the bank in flash/ROM and render directly from it.
- Optionally cache looped waves in RAM to reduce XIP flash pressure.
- Validate output on a host before flashing firmware.

## Repository Layout

| Path | Purpose |
|------|---------|
| `wavetable.c.inl` | Fixed-point MIDI wavetable engine. Include this in one C translation unit. |
| `gm_bank.h` | Packed bank format and validation helper. |
| `wt_luts.h` | Baked lookup tables used by the fixed-point engine. |
| `tools/dls_pack.c` | Host tool: converts a RIFF DLS bank into the packed runtime blob. |
| `examples/wt_render.c` | Host renderer for validating the fixed-point engine against a packed bank. |
| `examples/rp2040/` | Optional glue for an existing RP2040/emulator integration. |
| `docs/usage.md` | Integration guide and public API notes. |
| `docs/device-integration.md` | RP2040-oriented integration and profiling notes. |
| `tools/` | Validation and analysis helpers. |

## Sound Bank Licensing

This project does not include `GM.DLS`, `gm.dls`, `gm_bank.bin`, or any Microsoft
sound data. Users must provide their own legally usable RIFF DLS General MIDI
bank and build the packed blob locally.

Buying or owning Windows should not be treated as permission to redistribute
Microsoft's `gm.dls` or to use it outside the rights granted by the applicable
Windows license. Microsoft's Windows license terms describe Windows as licensed,
not sold, apply to included sound files, and reserve rights not expressly granted.
See the current Microsoft license terms page:
https://www.microsoft.com/en-us/useterms

Practical rule for this repo: do not commit or publish `GM.DLS`, `gm.dls`, or a
packed `gm_bank.bin` derived from a bank you cannot redistribute.

## Build Host Tools

The PowerShell build script expects a C compiler. Put `gcc` in `PATH`, or set
`CC` to a compiler path.

```powershell
./build.ps1 -Target dls_pack
./build.ps1 -Target wt_render
./build.ps1 -Target midi_selfcheck
```

Pack a bank for your target sample rate:

```powershell
./build/dls_pack.exe path/to/gm.dls build/gm_bank.bin 22050
```

Render a MIDI file through the fixed-point engine on the host:

```powershell
./build.ps1 -Target wt_render
./build/wt_render.exe song.mid build/song.wav build/gm_bank.bin
```

## Embedded Integration

At runtime, bind the packed bank once and feed MIDI channel-voice messages:

```c
#include <stdint.h>
#include "gm_bank.h"

#define INLINE static inline
#define SOUND_FREQUENCY 22050
#include "wavetable.c.inl"

void synth_init(const void *bank_blob) {
    wt_set_bank(bank_blob);
}

void synth_midi(uint8_t status, uint8_t data1, uint8_t data2) {
    midi_command_t m = { status, data1, data2, 0 };
    parse_midi(&m);
}

void synth_render(int16_t *stereo, int frames) {
    for (int i = 0; i < frames; ++i) {
        midi_sample_stereo(&stereo[0], &stereo[1]);
        stereo += 2;
    }
}
```

See `docs/usage.md` for the complete API and configuration macros.
