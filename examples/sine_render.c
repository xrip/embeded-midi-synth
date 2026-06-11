// Host renderer for the lightweight generated-waveform "sine" GM synth.
//
//   sine_render <input.mid> <output.wav> [sample-rate]
//
// Drives the SAME engine that runs on the RP2040 (sine/general-midi.c.inl) over
// a Standard MIDI File and writes a 16-bit mono WAV. No sound bank is needed:
// every voice is synthesised from the built-in sine table and noise LFSR. Used
// to audition / validate the generator on the desktop before flashing.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/dls_parse.c.inl"   // file_blob_t, read_entire_file, free_file_blob, fourcc_is, rd_*
#include "smf_parse.c.inl"            // midi_file_t, midi_file_load

// ---- engine glue (mirrors what the device includer supplies) -----------------
#define INLINE inline                // engine writes `static INLINE` (like the device)
#ifndef SOUND_FREQUENCY
#define SOUND_FREQUENCY 22050        // device default output rate
#endif
#define __not_in_flash(x)            // host has no flash sections
#define __fast_mul(a, b) ((a) * (b)) // device maps this to a single-cycle MUL
#include "../sine/general-midi.c.inl"

// ---- minimal mono WAV writer -------------------------------------------------

typedef struct {
    FILE *file;
    uint32_t frames;
} wav_t;

static void wr_u16(FILE *f, uint16_t v) { uint8_t b[2] = {(uint8_t) v, (uint8_t) (v >> 8)}; fwrite(b, 1, 2, f); }
static void wr_u32(FILE *f, uint32_t v) {
    uint8_t b[4] = {(uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24)};
    fwrite(b, 1, 4, f);
}

static int wav_open(wav_t *w, const char *path, uint32_t rate) {
    w->file = fopen(path, "wb");
    if (!w->file) { fprintf(stderr, "open %s failed\n", path); return 0; }
    w->frames = 0;
    fwrite("RIFF", 1, 4, w->file); wr_u32(w->file, 0);
    fwrite("WAVE", 1, 4, w->file);
    fwrite("fmt ", 1, 4, w->file); wr_u32(w->file, 16);
    wr_u16(w->file, 1); wr_u16(w->file, 1);          // PCM, mono
    wr_u32(w->file, rate); wr_u32(w->file, rate * 2u); // byte rate
    wr_u16(w->file, 2); wr_u16(w->file, 16);          // block align, bits
    fwrite("data", 1, 4, w->file); wr_u32(w->file, 0);
    return 1;
}

static void wav_write(wav_t *w, int16_t s) {
    wr_u16(w->file, (uint16_t) s);
    w->frames++;
}

static int wav_close(wav_t *w) {
    uint32_t data = w->frames * 2u;
    int ok = 1;
    if (fseek(w->file, 4, SEEK_SET) != 0) ok = 0;
    wr_u32(w->file, 36u + data);
    if (fseek(w->file, 40, SEEK_SET) != 0) ok = 0;
    wr_u32(w->file, data);
    if (fclose(w->file) != 0) ok = 0;
    return ok;
}

// ---- sequencer ---------------------------------------------------------------

static void render_block(wav_t *wav, uint64_t samples) {
    for (uint64_t i = 0; i < samples; ++i)
        wav_write(wav, midi_sample());
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s <input.mid> <output.wav> [sample-rate]\n", argv[0]);
        return 2;
    }
    const char *midi_path = argv[1];
    const char *wav_path = argv[2];
    uint32_t sample_rate = SOUND_FREQUENCY;
    if (argc == 4) sample_rate = (uint32_t) strtoul(argv[3], NULL, 10);
    if (sample_rate != SOUND_FREQUENCY) {
        // The pitch tables are baked for SOUND_FREQUENCY; rendering at another
        // rate would detune everything. Keep the harness honest.
        fprintf(stderr, "note: engine is baked for %d Hz; ignoring %u Hz request\n",
                SOUND_FREQUENCY, sample_rate);
        sample_rate = SOUND_FREQUENCY;
    }

    midi_file_t midi;
    if (!midi_file_load(midi_path, &midi)) return 1;

    wav_t wav;
    if (!wav_open(&wav, wav_path, sample_rate)) { midi_file_free(&midi); return 1; }

    uint64_t current_tick = 0, current_sample = 0;
    double exact_sample = 0.0;
    uint32_t tempo = 500000;

    for (size_t i = 0; i < midi.event_count; ++i) {
        const midi_event_t *ev = &midi.events[i];
        if (ev->tick > current_tick) {
            uint64_t delta = ev->tick - current_tick;
            exact_sample += (double) delta * (double) tempo * (double) sample_rate /
                            (1000000.0 * (double) midi.division);
            uint64_t target = (uint64_t) (exact_sample + 0.5);
            if (target > current_sample) {
                render_block(&wav, target - current_sample);
                current_sample = target;
            }
            current_tick = ev->tick;
        }
        if (ev->type == MIDI_EVENT_TEMPO) {
            tempo = ev->tempo_us_per_quarter ? ev->tempo_us_per_quarter : 500000;
        } else {
            midi_command_t cmd = {ev->status, ev->data1, ev->data2, 0};
            parse_midi(&cmd);
        }
    }

    // Force every still-sounding melodic voice into a fast release, then render
    // the tail until the mix falls silent (capped so a stuck pad can't hang).
    for (int v = 0; v < MAX_MIDI_VOICES; ++v) {
        if (IS_ACTIVE_VOICE(v) && midi_voices[v].channel != 9) {
            midi_voices[v].attack_target = 0;
            midi_voices[v].sustain_level = 0;
            midi_voices[v].decay_shift = 2;
        }
    }
    uint64_t tail = (uint64_t) sample_rate * 4u;
    while (tail-- && active_voice_bitmask) render_block(&wav, 1);

    int ok = wav_close(&wav);
    fprintf(stderr, "SINE: %u Hz mono, %u frames -> %s\n", sample_rate, wav.frames, wav_path);
    midi_file_free(&midi);
    return ok ? 0 : 1;
}
