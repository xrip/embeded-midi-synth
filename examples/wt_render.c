// Host A/B harness for the fixed-point wavetable engine.
//
//   wt_render <input.mid> <output.wav> <gm_bank.bin> [sample-rate]
//
// Loads the packed soundbank, drives the SAME engine that targets the RP2040
// (wavetable.inl) over a Standard MIDI File using the same tick->sample timing
// as the golden reference (examples/gm_dls_player.c), and writes 16-bit stereo WAV. Used
// to validate the fixed-point engine on the desktop before flashing.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../dls_parse.inl"   // file_blob_t, read_entire_file, rd_*, fourcc_is
#include "../smf_parse.inl"   // midi_file_t, midi_file_load
#include "../gm_bank.h"

#define INLINE static inline
#ifndef SOUND_FREQUENCY
#define SOUND_FREQUENCY 22050   // GM.DLS samples are natively 22050 Hz
#endif
#define WT_MAX_VOICES 32   // match the RP2040 target so A/B stealing is identical
#include "../wavetable.inl"

// ---- minimal WAV writer ------------------------------------------------------

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
    wr_u16(w->file, 1); wr_u16(w->file, 2);
    wr_u32(w->file, rate); wr_u32(w->file, rate * 4u);
    wr_u16(w->file, 4); wr_u16(w->file, 16);
    fwrite("data", 1, 4, w->file); wr_u32(w->file, 0);
    return 1;
}

static void wav_write(wav_t *w, int16_t l, int16_t r) {
    wr_u16(w->file, (uint16_t) l);
    wr_u16(w->file, (uint16_t) r);
    w->frames++;
}

static int wav_close(wav_t *w) {
    uint32_t data = w->frames * 4u;
    int ok = 1;
    if (fseek(w->file, 4, SEEK_SET) != 0) ok = 0;
    wr_u32(w->file, 36u + data);
    if (fseek(w->file, 40, SEEK_SET) != 0) ok = 0;
    wr_u32(w->file, data);
    if (fclose(w->file) != 0) ok = 0;
    return ok;
}

// ---- sequencer (same timing math as gm_dls_player render_midi_to_wav) --------

static void render_block(wav_t *wav, uint64_t samples) {
    extern int g_probe_max; extern long g_probe_sum, g_probe_n;
#if defined(WT_TEST_RELEASE) && defined(WT_WAVE_CACHE)
    wt_cache_release();   // stress: drop the cache mid-playback every block; output
                          // must stay bit-exact (proves the voice->flash repoint).
#endif
    for (uint64_t i = 0; i < samples; ++i) {
        int16_t l, r;
        midi_sample_stereo(&l, &r);
        wav_write(wav, l, r);
        if ((wav->frames & 1023)==0){int a=0;for(int v=0;v<WT_MAX_VOICES;++v)if(g_voices[v].active)a++; if(a>g_probe_max)g_probe_max=a; g_probe_sum+=a; g_probe_n++;}
    }
}
int g_probe_max=0; long g_probe_sum=0,g_probe_n=0; //VOICEPROBE

int main(int argc, char **argv) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "usage: %s <input.mid> <output.wav> <gm_bank.bin> [sample-rate]\n", argv[0]);
        return 2;
    }
    const char *midi_path = argv[1];
    const char *wav_path = argv[2];
    const char *bank_path = argv[3];
    uint32_t sample_rate = SOUND_FREQUENCY;
    if (argc == 5) sample_rate = (uint32_t) strtoul(argv[4], NULL, 10);

    file_blob_t bank_blob = {0};
    if (!read_entire_file(bank_path, &bank_blob)) return 1;
    if (!gm_bank_view(bank_blob.data, &g_bank)) {
        fprintf(stderr, "%s is not a GMWB bank\n", bank_path);
        return 1;
    }
    if (g_bank.header->output_rate != sample_rate) {
        fprintf(stderr, "warning: bank baked for %u Hz, rendering at %u Hz\n",
                g_bank.header->output_rate, sample_rate);
    }
    wt_set_bank(bank_blob.data);

    midi_file_t midi;
    if (!midi_file_load(midi_path, &midi)) { free_file_blob(&bank_blob); return 1; }

    wav_t wav;
    if (!wav_open(&wav, wav_path, sample_rate)) { midi_file_free(&midi); free_file_blob(&bank_blob); return 1; }

    uint64_t current_tick = 0, current_sample = 0;
    double exact_sample = 0.0;
    uint32_t tempo = 500000;

    for (size_t i = 0; i < midi.event_count; ++i) {
        const midi_event_t *ev = &midi.events[i];
        if (ev->tick > current_tick) {
            uint64_t delta = ev->tick - current_tick;
            exact_sample += (double) delta * (double) tempo * (double) sample_rate /
                            (1000000.0 * (double) midi.division);
            uint64_t target = (uint64_t) llround(exact_sample);
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

    // Release and render tail.
    for (int i = 0; i < WT_MAX_VOICES; ++i)
        if (g_voices[i].active && !g_voices[i].percussion) wt_release_voice(&g_voices[i]);
    uint64_t tail = (uint64_t) sample_rate * 4u;
    while (tail-- && wt_has_active_voices()) render_block(&wav, 1);

    int ok = wav_close(&wav);
    fprintf(stderr, "WT: %u Hz stereo, %u frames, WT_BLOCK=%d -> %s\n", sample_rate, wav.frames, WT_BLOCK, wav_path);
    fprintf(stderr, "VOICES peak=%d avg=%.1f\n", g_probe_max, g_probe_n ? (double) g_probe_sum / g_probe_n : 0.0);
#ifdef WT_WAVE_CACHE
    {
        uint32_t kb = 0; int n = 0;
        for (int i = 0; i < WT_WAVE_CACHE_SLOTS; ++i)
            if (g_wave_ram[i]) { ++n; kb += g_bank.waves[i].frame_count * 2u / 1024; }
        fprintf(stderr, "WAVECACHE resident=%uKB in %d waves (end-of-render)\n", kb, n);
    }
#ifdef WT_WAVE_CACHE_PROFILE
    {
        uint64_t reads = g_pcm_ram_reads + g_pcm_flash_reads;
        fprintf(stderr, "  PER-SAMPLE pcm reads from RAM: %.1f%% (RAM=%llu flash=%llu)\n",
                reads ? 100.0 * (double) g_pcm_ram_reads / (double) reads : 0.0,
                (unsigned long long) g_pcm_ram_reads, (unsigned long long) g_pcm_flash_reads);
    }
#endif
#endif
    midi_file_free(&midi);
    free_file_blob(&bank_blob);
    return ok ? 0 : 1;
}
