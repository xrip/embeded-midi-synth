// Host compile/link check for the device GM glue (general-midi.c.inl).
// Verifies the wavetable engine wires up and the parse_midi / midi_sample
// contract that mpu401.c.inl depends on still resolves, with INLINE supplied by
// the includer (as emulator.h does on device). It is NOT meant to run (needs a
// real bank); a clean compile + link is the test.
//
//   powershell -File build.ps1 -Target midi_selfcheck
#include <stdint.h>

#define INLINE inline           // emulator convention: code writes `static INLINE`
#define SOUND_FREQUENCY 22050
#include "../general-midi.c.inl"

// Stand-in for the .incbin'd bank (gm_bank.S provides the real one on device).
const uint8_t gm_bank_blob[64] = {0};

int main(void) {
    midi_command_t c = {0x90, 60, 100, 0};   // note-on
    parse_midi(&c);
    volatile int16_t s = midi_sample();
    (void) s;
    return 0;
}
