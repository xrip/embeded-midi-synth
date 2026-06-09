#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DLS_DRUM_BANK 0x80000000u
#define MAX_VOICES 96
#define MIDI_CHANNELS 16
#define DEFAULT_SAMPLE_RATE 44100u
#define DEFAULT_PITCH_BEND_RANGE_SEMITONES 2.0

typedef struct {
    uint8_t attack_shift;
    uint8_t decay_shift;
    uint8_t sustain_level;
} gm_envelope_t;

static const gm_envelope_t gm_envelopes[128] = {
    {0, 3, 20}, {0, 3, 20}, {0, 3, 25}, {0, 3, 20}, {0, 3, 15}, {0, 3, 20}, {0, 2, 10}, {0, 3, 20},
    {0, 2, 5},  {0, 2, 5},  {0, 2, 5},  {0, 2, 5},  {0, 2, 5},  {0, 2, 5},  {0, 2, 5},  {0, 2, 5},
    {0, 7, 240},{0, 7, 240},{0, 7, 240},{0, 7, 240},{0, 7, 240},{0, 7, 240},{0, 7, 240},{0, 7, 240},
    {0, 3, 30}, {0, 3, 30}, {0, 3, 25}, {0, 3, 25}, {0, 3, 30}, {0, 3, 20}, {0, 3, 20}, {0, 3, 20},
    {0, 4, 60}, {0, 4, 60}, {0, 4, 60}, {0, 4, 60}, {0, 3, 40}, {0, 3, 40}, {0, 4, 60}, {0, 4, 60},
    {3, 5, 180},{3, 5, 180},{3, 5, 180},{3, 5, 180},{3, 5, 180},{2, 4, 120},{2, 4, 100},{0, 2, 5},
    {2, 5, 200},{2, 5, 200},{2, 5, 180},{2, 5, 180},{2, 6, 220},{2, 5, 180},{2, 5, 200},{2, 5, 160},
    {1, 5, 160},{1, 5, 160},{1, 5, 160},{1, 5, 160},{1, 5, 140},{1, 5, 140},{1, 5, 160},{1, 5, 160},
    {0, 5, 180},{0, 5, 180},{0, 5, 180},{0, 5, 180},{0, 5, 160},{0, 5, 160},{0, 5, 180},{0, 5, 180},
    {1, 6, 200},{1, 6, 200},{1, 6, 200},{1, 6, 200},{1, 6, 220},{1, 6, 220},{1, 6, 200},{1, 6, 200},
    {0, 5, 200},{0, 5, 180},{1, 6, 220},{0, 5, 160},{0, 5, 180},{0, 5, 180},{0, 5, 200},{0, 5, 200},
    {3, 6, 220},{3, 6, 220},{3, 6, 220},{3, 6, 220},{3, 6, 220},{3, 6, 200},{3, 6, 220},{3, 6, 220},
    {1, 4, 100},{2, 5, 160},{0, 3, 30}, {3, 6, 220},{1, 4, 80}, {1, 4, 80}, {1, 4, 100},{1, 4, 100},
    {0, 4, 100},{0, 3, 40}, {0, 4, 100},{0, 4, 80}, {0, 3, 30}, {0, 3, 30}, {0, 4, 80}, {0, 4, 80},
    {0, 2, 10}, {0, 2, 5},  {0, 2, 10}, {0, 2, 5},  {0, 2, 10}, {0, 2, 5},  {0, 2, 10}, {0, 3, 20},
    {1, 4, 80}, {1, 4, 60}, {0, 3, 30}, {0, 3, 30}, {1, 4, 60}, {1, 4, 80}, {0, 3, 30}, {1, 4, 60},
};

typedef struct {
    uint8_t *data;
    size_t size;
} file_blob_t;

typedef struct {
    uint16_t format_tag;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    const uint8_t *data;
    uint32_t data_size;
    uint32_t frame_count;
    bool valid;

    bool has_wsmp;
    uint16_t unity_note;
    int16_t fine_tune;
    int32_t attenuation;
    bool looped;
    uint32_t loop_start;
    uint32_t loop_length;
} dls_wave_t;

typedef struct {
    uint16_t key_low;
    uint16_t key_high;
    uint16_t vel_low;
    uint16_t vel_high;
    uint16_t options;
    uint16_t key_group;
    uint32_t wave_index;

    bool has_wsmp;
    uint16_t unity_note;
    int16_t fine_tune;
    int32_t attenuation;
    bool looped;
    uint32_t loop_start;
    uint32_t loop_length;
} dls_region_t;

typedef struct {
    uint32_t bank;
    uint32_t program;
    char name[64];
    dls_region_t *regions;
    size_t region_count;
    size_t region_capacity;
} dls_instrument_t;

typedef struct {
    file_blob_t blob;
    uint32_t *pool_offsets;
    size_t pool_count;
    dls_wave_t *waves;
    size_t wave_count;
    dls_instrument_t *instruments;
    size_t instrument_count;
    size_t instrument_capacity;
} dls_bank_t;

typedef enum {
    MIDI_EVENT_CHANNEL,
    MIDI_EVENT_TEMPO,
} midi_event_type_t;

typedef struct {
    uint64_t tick;
    uint32_t order;
    midi_event_type_t type;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint32_t tempo_us_per_quarter;
} midi_event_t;

typedef struct {
    uint16_t format;
    uint16_t track_count;
    uint16_t division;
    midi_event_t *events;
    size_t event_count;
    size_t event_capacity;
} midi_file_t;

typedef struct {
    uint8_t program;
    uint8_t volume;
    uint8_t expression;
    uint8_t pan;
    uint8_t bank_msb;
    uint8_t bank_lsb;
    int pitch_bend;
    double pitch_bend_range;
    uint8_t rpn_msb;
    uint8_t rpn_lsb;
    bool sustain;
} midi_channel_t;

typedef struct {
    bool active;
    bool percussion;
    bool sustained;
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    uint8_t env_level;
    uint8_t sustain_level;
    uint8_t decay_shift;
    uint8_t attack_target;
    uint16_t key_group;
    uint64_t age;

    const dls_region_t *region;
    const dls_wave_t *wave;
    double position;
    double step;
    double attenuation_gain;
    double percussion_gain;
    double percussion_decay;
} synth_voice_t;

typedef struct {
    const dls_bank_t *bank;
    uint32_t sample_rate;
    midi_channel_t channels[MIDI_CHANNELS];
    synth_voice_t voices[MAX_VOICES];
    uint64_t next_age;
} synth_t;

typedef struct {
    FILE *file;
    uint32_t sample_rate;
    uint32_t frames_written;
} wav_writer_t;

static uint16_t rd_u16le(const uint8_t *p) {
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static int16_t rd_i16le(const uint8_t *p) {
    return (int16_t) rd_u16le(p);
}

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static int32_t rd_i32le(const uint8_t *p) {
    return (int32_t) rd_u32le(p);
}

static uint16_t rd_u16be(const uint8_t *p) {
    return ((uint16_t) p[0] << 8) | (uint16_t) p[1];
}

static uint32_t rd_u32be(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static bool fourcc_is(const uint8_t *p, const char id[4]) {
    return memcmp(p, id, 4) == 0;
}

static bool read_entire_file(const char *path, file_blob_t *out) {
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long end = ftell(f);
    if (end < 0) {
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    uint8_t *data = malloc((size_t) end);
    if (!data) {
        fclose(f);
        fprintf(stderr, "out of memory reading %s\n", path);
        return false;
    }

    if (fread(data, 1, (size_t) end, f) != (size_t) end) {
        free(data);
        fclose(f);
        fprintf(stderr, "read %s failed\n", path);
        return false;
    }

    fclose(f);
    out->data = data;
    out->size = (size_t) end;
    return true;
}

static void free_file_blob(file_blob_t *blob) {
    free(blob->data);
    blob->data = NULL;
    blob->size = 0;
}

static bool reserve_region(dls_instrument_t *instrument) {
    if (instrument->region_count < instrument->region_capacity) return true;
    size_t next_capacity = instrument->region_capacity ? instrument->region_capacity * 2 : 8;
    dls_region_t *next = realloc(instrument->regions, next_capacity * sizeof(*next));
    if (!next) return false;
    instrument->regions = next;
    instrument->region_capacity = next_capacity;
    return true;
}

static bool reserve_instrument(dls_bank_t *bank) {
    if (bank->instrument_count < bank->instrument_capacity) return true;
    size_t next_capacity = bank->instrument_capacity ? bank->instrument_capacity * 2 : 64;
    dls_instrument_t *next = realloc(bank->instruments, next_capacity * sizeof(*next));
    if (!next) return false;
    bank->instruments = next;
    bank->instrument_capacity = next_capacity;
    return true;
}

static size_t riff_next(size_t off, uint32_t size) {
    return off + 8u + (size_t) size + (size_t) (size & 1u);
}

static bool parse_wsmp(const uint8_t *data, size_t size, dls_region_t *region, dls_wave_t *wave) {
    if (size < 20) return false;

    uint16_t unity_note = rd_u16le(data + 4);
    int16_t fine_tune = rd_i16le(data + 6);
    int32_t attenuation = rd_i32le(data + 8);
    uint32_t loop_count = rd_u32le(data + 16);
    bool looped = false;
    uint32_t loop_start = 0;
    uint32_t loop_length = 0;

    if (loop_count > 0 && size >= 36) {
        uint32_t loop_type = rd_u32le(data + 24);
        loop_start = rd_u32le(data + 28);
        loop_length = rd_u32le(data + 32);
        looped = loop_type == 0 && loop_length > 1;
    }

    if (region) {
        region->has_wsmp = true;
        region->unity_note = unity_note;
        region->fine_tune = fine_tune;
        region->attenuation = attenuation;
        region->looped = looped;
        region->loop_start = loop_start;
        region->loop_length = loop_length;
    }

    if (wave) {
        wave->has_wsmp = true;
        wave->unity_note = unity_note;
        wave->fine_tune = fine_tune;
        wave->attenuation = attenuation;
        wave->looped = looped;
        wave->loop_start = loop_start;
        wave->loop_length = loop_length;
    }

    return true;
}

static bool parse_region(const uint8_t *file, size_t file_size, size_t region_off, dls_instrument_t *instrument) {
    if (region_off + 12 > file_size || !fourcc_is(file + region_off, "LIST")) return false;
    uint32_t list_size = rd_u32le(file + region_off + 4);
    size_t data_off = region_off + 8u;
    size_t end = data_off + list_size;
    if (end > file_size || list_size < 4) return false;
    if (!fourcc_is(file + data_off, "rgn ") && !fourcc_is(file + data_off, "rgn2")) return true;

    dls_region_t region = {
        .key_low = 0,
        .key_high = 127,
        .vel_low = 0,
        .vel_high = 127,
        .wave_index = UINT32_MAX,
        .unity_note = 60,
    };

    for (size_t off = data_off + 4u; off + 8u <= end;) {
        uint32_t size = rd_u32le(file + off + 4);
        size_t payload = off + 8u;
        size_t next = riff_next(off, size);
        if (payload + size > file_size || payload + size > end || next <= off) return false;

        if (fourcc_is(file + off, "rgnh") && size >= 12) {
            region.key_low = rd_u16le(file + payload);
            region.key_high = rd_u16le(file + payload + 2);
            region.vel_low = rd_u16le(file + payload + 4);
            region.vel_high = rd_u16le(file + payload + 6);
            region.options = rd_u16le(file + payload + 8);
            region.key_group = rd_u16le(file + payload + 10);
        } else if (fourcc_is(file + off, "wsmp")) {
            if (!parse_wsmp(file + payload, size, &region, NULL)) return false;
        } else if (fourcc_is(file + off, "wlnk") && size >= 12) {
            region.wave_index = rd_u32le(file + payload + 8);
        }

        off = next;
    }

    if (region.wave_index == UINT32_MAX) return true;
    if (!reserve_region(instrument)) return false;
    instrument->regions[instrument->region_count++] = region;
    return true;
}

static void parse_info_name(const uint8_t *file, size_t file_size, size_t info_off, char out[64]) {
    if (info_off + 12 > file_size || !fourcc_is(file + info_off, "LIST")) return;
    uint32_t list_size = rd_u32le(file + info_off + 4);
    size_t data_off = info_off + 8u;
    size_t end = data_off + list_size;
    if (end > file_size || list_size < 4 || !fourcc_is(file + data_off, "INFO")) return;

    for (size_t off = data_off + 4u; off + 8u <= end;) {
        uint32_t size = rd_u32le(file + off + 4);
        size_t payload = off + 8u;
        size_t next = riff_next(off, size);
        if (payload + size > file_size || payload + size > end || next <= off) return;

        if (fourcc_is(file + off, "INAM")) {
            size_t copy_len = size;
            if (copy_len >= 64) copy_len = 63;
            memcpy(out, file + payload, copy_len);
            out[copy_len] = '\0';
            for (size_t i = 0; out[i]; ++i) {
                if ((unsigned char) out[i] < 32) {
                    out[i] = '\0';
                    break;
                }
            }
            return;
        }

        off = next;
    }
}

static bool parse_instrument(const uint8_t *file, size_t file_size, size_t ins_off, dls_bank_t *bank) {
    if (ins_off + 12 > file_size || !fourcc_is(file + ins_off, "LIST")) return false;
    uint32_t list_size = rd_u32le(file + ins_off + 4);
    size_t data_off = ins_off + 8u;
    size_t end = data_off + list_size;
    if (end > file_size || list_size < 4 || !fourcc_is(file + data_off, "ins ")) return false;

    dls_instrument_t instrument = {0};
    instrument.program = UINT32_MAX;

    for (size_t off = data_off + 4u; off + 8u <= end;) {
        uint32_t size = rd_u32le(file + off + 4);
        size_t payload = off + 8u;
        size_t next = riff_next(off, size);
        if (payload + size > file_size || payload + size > end || next <= off) {
            free(instrument.regions);
            return false;
        }

        if (fourcc_is(file + off, "insh") && size >= 12) {
            instrument.bank = rd_u32le(file + payload + 4);
            instrument.program = rd_u32le(file + payload + 8) & 127u;
        } else if (fourcc_is(file + off, "LIST") && size >= 4 && fourcc_is(file + payload, "lrgn")) {
            for (size_t r = payload + 4u; r + 8u <= payload + size;) {
                uint32_t region_size = rd_u32le(file + r + 4);
                size_t next_region = riff_next(r, region_size);
                if (r + 8u + region_size > file_size || r + 8u + region_size > payload + size || next_region <= r) {
                    free(instrument.regions);
                    return false;
                }
                if (fourcc_is(file + r, "LIST")) {
                    if (!parse_region(file, file_size, r, &instrument)) {
                        free(instrument.regions);
                        return false;
                    }
                }
                r = next_region;
            }
        } else if (fourcc_is(file + off, "LIST") && size >= 4 && fourcc_is(file + payload, "INFO")) {
            parse_info_name(file, file_size, off, instrument.name);
        }

        off = next;
    }

    if (instrument.program == UINT32_MAX || instrument.region_count == 0) {
        free(instrument.regions);
        return true;
    }

    if (!reserve_instrument(bank)) {
        free(instrument.regions);
        return false;
    }
    bank->instruments[bank->instrument_count++] = instrument;
    return true;
}

static bool parse_lins(const uint8_t *file, size_t file_size, size_t lins_off, dls_bank_t *bank) {
    if (lins_off + 12 > file_size || !fourcc_is(file + lins_off, "LIST")) return false;
    uint32_t list_size = rd_u32le(file + lins_off + 4);
    size_t data_off = lins_off + 8u;
    size_t end = data_off + list_size;
    if (end > file_size || list_size < 4 || !fourcc_is(file + data_off, "lins")) return false;

    for (size_t off = data_off + 4u; off + 8u <= end;) {
        uint32_t size = rd_u32le(file + off + 4);
        size_t payload = off + 8u;
        size_t next = riff_next(off, size);
        if (payload + size > file_size || payload + size > end || next <= off) return false;
        if (fourcc_is(file + off, "LIST") && size >= 4 && fourcc_is(file + payload, "ins ")) {
            if (!parse_instrument(file, file_size, off, bank)) return false;
        }
        off = next;
    }

    return true;
}

static bool parse_ptbl(const uint8_t *file, size_t file_size, size_t ptbl_off, dls_bank_t *bank) {
    if (ptbl_off + 16 > file_size || !fourcc_is(file + ptbl_off, "ptbl")) return false;
    uint32_t size = rd_u32le(file + ptbl_off + 4);
    size_t payload = ptbl_off + 8u;
    if (payload + size > file_size || size < 8) return false;

    uint32_t cb_size = rd_u32le(file + payload);
    uint32_t cue_count = rd_u32le(file + payload + 4);
    if (cb_size < 8 || cue_count == 0 || cue_count > 65536 || size < 8u + cue_count * 4u) return false;

    bank->pool_offsets = calloc(cue_count, sizeof(*bank->pool_offsets));
    if (!bank->pool_offsets) return false;
    bank->pool_count = cue_count;

    for (uint32_t i = 0; i < cue_count; ++i) {
        bank->pool_offsets[i] = rd_u32le(file + payload + 8u + i * 4u);
    }

    return true;
}

static bool parse_wave(const uint8_t *file, size_t file_size, size_t wave_off, dls_wave_t *wave) {
    if (wave_off + 12 > file_size || !fourcc_is(file + wave_off, "LIST")) return false;
    uint32_t list_size = rd_u32le(file + wave_off + 4);
    size_t data_off = wave_off + 8u;
    size_t end = data_off + list_size;
    if (end > file_size || list_size < 4 || !fourcc_is(file + data_off, "wave")) return false;

    memset(wave, 0, sizeof(*wave));

    for (size_t off = data_off + 4u; off + 8u <= end;) {
        uint32_t size = rd_u32le(file + off + 4);
        size_t payload = off + 8u;
        size_t next = riff_next(off, size);
        if (payload + size > file_size || payload + size > end || next <= off) return false;

        if (fourcc_is(file + off, "fmt ") && size >= 16) {
            wave->format_tag = rd_u16le(file + payload);
            wave->channels = rd_u16le(file + payload + 2);
            wave->sample_rate = rd_u32le(file + payload + 4);
            wave->block_align = rd_u16le(file + payload + 12);
            wave->bits_per_sample = rd_u16le(file + payload + 14);
        } else if (fourcc_is(file + off, "data")) {
            wave->data = file + payload;
            wave->data_size = size;
        } else if (fourcc_is(file + off, "wsmp")) {
            if (!parse_wsmp(file + payload, size, NULL, wave)) return false;
        }

        off = next;
    }

    if (wave->format_tag != 1 || wave->channels == 0 || wave->block_align == 0 ||
        (wave->bits_per_sample != 8 && wave->bits_per_sample != 16) || !wave->data) {
        return true;
    }

    wave->frame_count = wave->data_size / wave->block_align;
    wave->valid = wave->frame_count > 1;
    if (wave->loop_start >= wave->frame_count) wave->looped = false;
    if (wave->loop_start + wave->loop_length > wave->frame_count) {
        wave->loop_length = wave->frame_count - wave->loop_start;
    }
    if (wave->loop_length <= 1) wave->looped = false;

    return true;
}

static bool parse_wvpl(const uint8_t *file, size_t file_size, size_t wvpl_off, dls_bank_t *bank) {
    if (wvpl_off + 12 > file_size || !fourcc_is(file + wvpl_off, "LIST")) return false;
    uint32_t list_size = rd_u32le(file + wvpl_off + 4);
    size_t data_off = wvpl_off + 8u;
    size_t end = data_off + list_size;
    if (end > file_size || list_size < 4 || !fourcc_is(file + data_off, "wvpl")) return false;
    if (!bank->pool_offsets || bank->pool_count == 0) return false;

    size_t wave_base = data_off + 4u;
    bank->waves = calloc(bank->pool_count, sizeof(*bank->waves));
    if (!bank->waves) return false;
    bank->wave_count = bank->pool_count;

    for (size_t i = 0; i < bank->pool_count; ++i) {
        size_t wave_off = wave_base + bank->pool_offsets[i];
        if (wave_off + 12 > end) continue;
        if (!parse_wave(file, file_size, wave_off, &bank->waves[i])) return false;
    }

    return true;
}

static void dls_bank_free(dls_bank_t *bank) {
    for (size_t i = 0; i < bank->instrument_count; ++i) {
        free(bank->instruments[i].regions);
    }
    free(bank->instruments);
    free(bank->pool_offsets);
    free(bank->waves);
    free_file_blob(&bank->blob);
    memset(bank, 0, sizeof(*bank));
}

static bool dls_bank_load(const char *path, dls_bank_t *bank) {
    memset(bank, 0, sizeof(*bank));
    if (!read_entire_file(path, &bank->blob)) return false;

    const uint8_t *file = bank->blob.data;
    size_t file_size = bank->blob.size;
    if (file_size < 12 || !fourcc_is(file, "RIFF") || !fourcc_is(file + 8, "DLS ")) {
        fprintf(stderr, "%s is not a RIFF DLS file\n", path);
        dls_bank_free(bank);
        return false;
    }

    uint32_t riff_size = rd_u32le(file + 4);
    size_t riff_end = 8u + (size_t) riff_size;
    if (riff_end > file_size) riff_end = file_size;

    size_t lins_off = 0;
    size_t ptbl_off = 0;
    size_t wvpl_off = 0;

    for (size_t off = 12u; off + 8u <= riff_end;) {
        uint32_t size = rd_u32le(file + off + 4);
        size_t payload = off + 8u;
        size_t next = riff_next(off, size);
        if (payload + size > file_size || payload + size > riff_end || next <= off) {
            fprintf(stderr, "bad RIFF chunk in %s at 0x%zx\n", path, off);
            dls_bank_free(bank);
            return false;
        }

        if (fourcc_is(file + off, "ptbl")) {
            ptbl_off = off;
        } else if (fourcc_is(file + off, "LIST") && size >= 4) {
            if (fourcc_is(file + payload, "lins")) lins_off = off;
            if (fourcc_is(file + payload, "wvpl")) wvpl_off = off;
        }

        off = next;
    }

    if (!lins_off || !ptbl_off || !wvpl_off) {
        fprintf(stderr, "%s misses required DLS chunks\n", path);
        dls_bank_free(bank);
        return false;
    }

    if (!parse_ptbl(file, file_size, ptbl_off, bank) ||
        !parse_wvpl(file, file_size, wvpl_off, bank) ||
        !parse_lins(file, file_size, lins_off, bank)) {
        fprintf(stderr, "failed to parse %s\n", path);
        dls_bank_free(bank);
        return false;
    }

    fprintf(stderr, "DLS: %zu instruments, %zu waves loaded from %s\n",
            bank->instrument_count, bank->wave_count, path);
    return true;
}

static bool midi_reserve_event(midi_file_t *midi) {
    if (midi->event_count < midi->event_capacity) return true;
    size_t next_capacity = midi->event_capacity ? midi->event_capacity * 2 : 4096;
    midi_event_t *next = realloc(midi->events, next_capacity * sizeof(*next));
    if (!next) return false;
    midi->events = next;
    midi->event_capacity = next_capacity;
    return true;
}

static bool midi_add_event(midi_file_t *midi, const midi_event_t *event) {
    if (!midi_reserve_event(midi)) return false;
    midi->events[midi->event_count++] = *event;
    return true;
}

static bool read_vlq(const uint8_t *data, size_t end, size_t *pos, uint32_t *out) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        if (*pos >= end) return false;
        uint8_t b = data[(*pos)++];
        value = (value << 7) | (uint32_t) (b & 0x7f);
        if ((b & 0x80) == 0) {
            *out = value;
            return true;
        }
    }
    return false;
}

static int midi_channel_data_len(uint8_t status) {
    switch (status & 0xf0u) {
        case 0xc0:
        case 0xd0:
            return 1;
        case 0x80:
        case 0x90:
        case 0xa0:
        case 0xb0:
        case 0xe0:
            return 2;
        default:
            return 0;
    }
}

static bool parse_midi_track(const uint8_t *data, size_t file_size, size_t track_off,
                             uint32_t track_size, uint32_t *order, midi_file_t *midi) {
    size_t pos = track_off;
    size_t end = track_off + track_size;
    if (end > file_size) return false;

    uint64_t tick = 0;
    uint8_t running_status = 0;

    while (pos < end) {
        uint32_t delta = 0;
        if (!read_vlq(data, end, &pos, &delta)) return false;
        tick += delta;
        if (pos >= end) return false;

        uint8_t b = data[pos++];
        if (b == 0xff) {
            if (pos >= end) return false;
            uint8_t meta_type = data[pos++];
            uint32_t length = 0;
            if (!read_vlq(data, end, &pos, &length) || pos + length > end) return false;

            if (meta_type == 0x51 && length == 3) {
                midi_event_t event = {
                    .tick = tick,
                    .order = (*order)++,
                    .type = MIDI_EVENT_TEMPO,
                    .tempo_us_per_quarter = ((uint32_t) data[pos] << 16) |
                                            ((uint32_t) data[pos + 1] << 8) |
                                            (uint32_t) data[pos + 2],
                };
                if (!midi_add_event(midi, &event)) return false;
            } else if (meta_type == 0x2f) {
                break;
            }

            pos += length;
            continue;
        }

        if (b == 0xf0 || b == 0xf7) {
            uint32_t length = 0;
            if (!read_vlq(data, end, &pos, &length) || pos + length > end) return false;
            pos += length;
            running_status = 0;
            continue;
        }

        uint8_t status = b;
        uint8_t data1 = 0;
        uint8_t data2 = 0;
        if (b & 0x80u) {
            if (b < 0x80 || b > 0xef) return false;
            running_status = b;
            int data_len = midi_channel_data_len(status);
            if (data_len == 0 || pos + (size_t) data_len > end) return false;
            data1 = data[pos++];
            if (data_len == 2) data2 = data[pos++];
        } else {
            if (!running_status) return false;
            status = running_status;
            int data_len = midi_channel_data_len(status);
            if (data_len == 0) return false;
            data1 = b;
            if (data_len == 2) {
                if (pos >= end) return false;
                data2 = data[pos++];
            }
        }

        midi_event_t event = {
            .tick = tick,
            .order = (*order)++,
            .type = MIDI_EVENT_CHANNEL,
            .status = status,
            .data1 = data1,
            .data2 = data2,
        };
        if (!midi_add_event(midi, &event)) return false;
    }

    return true;
}

static int compare_midi_events(const void *a, const void *b) {
    const midi_event_t *ea = a;
    const midi_event_t *eb = b;
    if (ea->tick < eb->tick) return -1;
    if (ea->tick > eb->tick) return 1;
    if (ea->order < eb->order) return -1;
    if (ea->order > eb->order) return 1;
    return 0;
}

static void midi_file_free(midi_file_t *midi) {
    free(midi->events);
    memset(midi, 0, sizeof(*midi));
}

static bool midi_file_load(const char *path, midi_file_t *midi) {
    memset(midi, 0, sizeof(*midi));
    file_blob_t blob = {0};
    if (!read_entire_file(path, &blob)) return false;

    const uint8_t *data = blob.data;
    size_t size = blob.size;
    if (size < 14 || !fourcc_is(data, "MThd")) {
        fprintf(stderr, "%s is not a Standard MIDI File\n", path);
        free_file_blob(&blob);
        return false;
    }

    uint32_t header_len = rd_u32be(data + 4);
    if (header_len < 6 || 8u + header_len > size) {
        free_file_blob(&blob);
        return false;
    }

    midi->format = rd_u16be(data + 8);
    midi->track_count = rd_u16be(data + 10);
    midi->division = rd_u16be(data + 12);
    if (midi->division & 0x8000u) {
        fprintf(stderr, "SMPTE time division is not supported\n");
        free_file_blob(&blob);
        return false;
    }
    if (midi->format > 1) {
        fprintf(stderr, "MIDI format %u is not supported\n", midi->format);
        free_file_blob(&blob);
        return false;
    }

    size_t pos = 8u + header_len;
    uint32_t order = 0;
    for (uint16_t track = 0; track < midi->track_count; ++track) {
        if (pos + 8u > size || !fourcc_is(data + pos, "MTrk")) {
            fprintf(stderr, "missing MTrk chunk %u\n", track);
            midi_file_free(midi);
            free_file_blob(&blob);
            return false;
        }

        uint32_t track_size = rd_u32be(data + pos + 4);
        pos += 8u;
        if (pos + track_size > size) {
            midi_file_free(midi);
            free_file_blob(&blob);
            return false;
        }

        if (!parse_midi_track(data, size, pos, track_size, &order, midi)) {
            fprintf(stderr, "failed to parse MIDI track %u\n", track);
            midi_file_free(midi);
            free_file_blob(&blob);
            return false;
        }
        pos += track_size;
    }

    qsort(midi->events, midi->event_count, sizeof(*midi->events), compare_midi_events);
    fprintf(stderr, "MIDI: format %u, %u tracks, %u TPQ, %zu events loaded from %s\n",
            midi->format, midi->track_count, midi->division, midi->event_count, path);
    free_file_blob(&blob);
    return true;
}

static const dls_instrument_t *dls_find_instrument(const dls_bank_t *bank, uint32_t dls_bank, uint32_t program) {
    const dls_instrument_t *fallback_program = NULL;
    const dls_instrument_t *fallback_piano = NULL;
    for (size_t i = 0; i < bank->instrument_count; ++i) {
        const dls_instrument_t *instrument = &bank->instruments[i];
        if (instrument->bank == dls_bank && instrument->program == program) return instrument;
        if (instrument->bank == 0 && instrument->program == program) fallback_program = instrument;
        if (instrument->bank == 0 && instrument->program == 0) fallback_piano = instrument;
    }

    if (dls_bank & DLS_DRUM_BANK) {
        for (size_t i = 0; i < bank->instrument_count; ++i) {
            const dls_instrument_t *instrument = &bank->instruments[i];
            if (instrument->bank == DLS_DRUM_BANK && instrument->program == 0) return instrument;
        }
    }

    return fallback_program ? fallback_program : fallback_piano;
}

static const dls_region_t *dls_find_region(const dls_instrument_t *instrument, uint8_t note, uint8_t velocity) {
    const dls_region_t *nearest = NULL;
    int nearest_distance = 1000;

    for (size_t i = 0; i < instrument->region_count; ++i) {
        const dls_region_t *region = &instrument->regions[i];
        if (note >= region->key_low && note <= region->key_high &&
            velocity >= region->vel_low && velocity <= region->vel_high) {
            return region;
        }

        int distance = 0;
        if (note < region->key_low) distance = (int) region->key_low - note;
        if (note > region->key_high) distance = note - (int) region->key_high;
        if (!nearest || distance < nearest_distance) {
            nearest = region;
            nearest_distance = distance;
        }
    }

    return nearest;
}

static double dls_attenuation_gain(int32_t attenuation) {
    return pow(10.0, (double) attenuation / (655360.0 * 20.0));
}

static double wave_read_channel(const dls_wave_t *wave, uint32_t frame, uint16_t channel) {
    if (frame >= wave->frame_count) return 0.0;
    if (channel >= wave->channels) channel = 0;

    const uint8_t *p = wave->data + (size_t) frame * wave->block_align;
    if (wave->bits_per_sample == 8) {
        return ((double) p[channel] - 128.0) / 128.0;
    }

    const uint8_t *sample = p + (size_t) channel * 2u;
    return (double) rd_i16le(sample) / 32768.0;
}

static double wave_sample_mono(const dls_wave_t *wave, double position) {
    if (!wave->valid || position < 0.0) return 0.0;
    uint32_t i0 = (uint32_t) position;
    if (i0 >= wave->frame_count) return 0.0;
    uint32_t i1 = i0 + 1u;
    if (i1 >= wave->frame_count) i1 = i0;
    double frac = position - (double) i0;

    double s0 = 0.0;
    double s1 = 0.0;
    for (uint16_t ch = 0; ch < wave->channels; ++ch) {
        s0 += wave_read_channel(wave, i0, ch);
        s1 += wave_read_channel(wave, i1, ch);
    }
    s0 /= (double) wave->channels;
    s1 /= (double) wave->channels;
    return s0 + (s1 - s0) * frac;
}

static void synth_init(synth_t *synth, const dls_bank_t *bank, uint32_t sample_rate) {
    memset(synth, 0, sizeof(*synth));
    synth->bank = bank;
    synth->sample_rate = sample_rate;
    for (int i = 0; i < MIDI_CHANNELS; ++i) {
        synth->channels[i].program = 0;
        synth->channels[i].volume = 100;
        synth->channels[i].expression = 127;
        synth->channels[i].pan = 64;
        synth->channels[i].pitch_bend = 8192;
        synth->channels[i].pitch_bend_range = DEFAULT_PITCH_BEND_RANGE_SEMITONES;
        synth->channels[i].rpn_msb = 127;
        synth->channels[i].rpn_lsb = 127;
    }
}

static bool synth_has_active_voices(const synth_t *synth) {
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (synth->voices[i].active) return true;
    }
    return false;
}

static synth_voice_t *synth_alloc_voice(synth_t *synth) {
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (!synth->voices[i].active) return &synth->voices[i];
    }

    synth_voice_t *oldest = &synth->voices[0];
    for (int i = 1; i < MAX_VOICES; ++i) {
        if (synth->voices[i].env_level < oldest->env_level ||
            (synth->voices[i].env_level == oldest->env_level && synth->voices[i].age < oldest->age)) {
            oldest = &synth->voices[i];
        }
    }
    return oldest;
}

static double channel_pitch_bend_semitones(const midi_channel_t *channel) {
    double centered = ((double) channel->pitch_bend - 8192.0) / 8192.0;
    if (centered < -1.0) centered = -1.0;
    if (centered > 1.0) centered = 1.0;
    return centered * channel->pitch_bend_range;
}

static double percussion_decay_seconds(uint8_t note) {
    switch (note) {
        case 35:
        case 36:
            return 0.55;
        case 37:
        case 42:
        case 44:
        case 70:
        case 74:
            return 0.30;
        case 38:
        case 39:
        case 40:
        case 54:
            return 0.75;
        case 41:
        case 43:
        case 45:
        case 47:
        case 48:
        case 50:
        case 60:
        case 61:
        case 62:
        case 63:
        case 64:
        case 65:
        case 66:
        case 67:
        case 68:
        case 69:
        case 75:
        case 76:
        case 77:
        case 81:
            return 1.00;
        case 46:
        case 58:
        case 71:
        case 73:
        case 78:
            return 1.60;
        case 49:
        case 52:
        case 55:
        case 57:
            return 4.00;
        case 51:
        case 53:
        case 59:
        case 80:
            return 3.00;
        default:
            return 1.25;
    }
}

static void synth_release_voice(synth_voice_t *voice) {
    if (!voice->active || voice->percussion) return;
    voice->sustained = false;
    voice->attack_target = 0;
    voice->sustain_level = 0;
    voice->decay_shift = 2;
}

static void synth_note_off(synth_t *synth, uint8_t channel_index, uint8_t note) {
    if (channel_index == 9) return;
    for (int i = 0; i < MAX_VOICES; ++i) {
        synth_voice_t *voice = &synth->voices[i];
        if (!voice->active || voice->channel != channel_index || voice->note != note) continue;
        if (synth->channels[channel_index].sustain) {
            voice->sustained = true;
        } else {
            synth_release_voice(voice);
        }
    }
}

static void synth_all_notes_off(synth_t *synth, uint8_t channel_index, bool immediate) {
    for (int i = 0; i < MAX_VOICES; ++i) {
        synth_voice_t *voice = &synth->voices[i];
        if (!voice->active || voice->channel != channel_index) continue;
        if (immediate || voice->percussion) {
            voice->active = false;
        } else {
            synth_release_voice(voice);
        }
    }
}

static void synth_note_on(synth_t *synth, uint8_t channel_index, uint8_t note, uint8_t velocity) {
    midi_channel_t *channel = &synth->channels[channel_index];
    uint32_t bank_number = ((uint32_t) channel->bank_msb << 8) | (uint32_t) channel->bank_lsb;
    uint32_t dls_bank = channel_index == 9 ? (DLS_DRUM_BANK | bank_number) : bank_number;
    uint32_t program = channel->program;

    const dls_instrument_t *instrument = dls_find_instrument(synth->bank, dls_bank, program);
    if (!instrument) return;
    const dls_region_t *region = dls_find_region(instrument, note, velocity);
    if (!region || region->wave_index >= synth->bank->wave_count) return;
    const dls_wave_t *wave = &synth->bank->waves[region->wave_index];
    if (!wave->valid) return;

    for (int i = 0; i < MAX_VOICES; ++i) {
        synth_voice_t *voice = &synth->voices[i];
        if (!voice->active || voice->channel != channel_index) continue;
        if (voice->note == note && !voice->percussion) {
            voice->active = false;
        } else if (channel_index == 9 && region->key_group != 0 && voice->key_group == region->key_group) {
            voice->active = false;
        }
    }

    uint16_t unity_note = region->has_wsmp ? region->unity_note : (wave->has_wsmp ? wave->unity_note : note);
    int16_t fine_tune = region->has_wsmp ? region->fine_tune : (wave->has_wsmp ? wave->fine_tune : 0);
    int32_t attenuation = region->has_wsmp ? region->attenuation : (wave->has_wsmp ? wave->attenuation : 0);
    double semitones = (double) note - (double) unity_note + (double) fine_tune / 100.0 + channel_pitch_bend_semitones(channel);

    synth_voice_t *voice = synth_alloc_voice(synth);
    memset(voice, 0, sizeof(*voice));
    voice->active = true;
    voice->percussion = channel_index == 9;
    voice->channel = channel_index;
    voice->note = note;
    voice->velocity = velocity;
    voice->key_group = region->key_group;
    voice->age = synth->next_age++;
    voice->region = region;
    voice->wave = wave;
    voice->position = 0.0;
    voice->step = ((double) wave->sample_rate / (double) synth->sample_rate) * pow(2.0, semitones / 12.0);
    voice->attenuation_gain = dls_attenuation_gain(attenuation);

    if (voice->percussion) {
        voice->env_level = velocity;
        voice->sustain_level = 0;
        voice->decay_shift = 0;
        voice->attack_target = 0;
        voice->percussion_gain = 1.0;
        voice->percussion_decay = pow(0.001, 1.0 / ((double) synth->sample_rate * percussion_decay_seconds(note)));
    } else {
        const gm_envelope_t *env = &gm_envelopes[program & 127u];
        voice->sustain_level = (uint8_t) (((uint32_t) velocity * env->sustain_level) / 255u);
        if (env->attack_shift) {
            voice->env_level = 0;
            voice->attack_target = velocity;
            voice->decay_shift = env->attack_shift;
        } else {
            voice->env_level = velocity;
            voice->attack_target = 0;
            voice->decay_shift = env->decay_shift;
        }
    }
}

static void synth_apply_pitch_to_channel(synth_t *synth, uint8_t channel_index) {
    midi_channel_t *channel = &synth->channels[channel_index];
    for (int i = 0; i < MAX_VOICES; ++i) {
        synth_voice_t *voice = &synth->voices[i];
        if (!voice->active || voice->channel != channel_index) continue;
        uint16_t unity_note = voice->region->has_wsmp ? voice->region->unity_note :
                              (voice->wave->has_wsmp ? voice->wave->unity_note : voice->note);
        int16_t fine_tune = voice->region->has_wsmp ? voice->region->fine_tune :
                            (voice->wave->has_wsmp ? voice->wave->fine_tune : 0);
        double semitones = (double) voice->note - (double) unity_note +
                           (double) fine_tune / 100.0 + channel_pitch_bend_semitones(channel);
        voice->step = ((double) voice->wave->sample_rate / (double) synth->sample_rate) * pow(2.0, semitones / 12.0);
    }
}

static bool channel_uses_pitch_bend_range_rpn(const midi_channel_t *channel) {
    return channel->rpn_msb == 0 && channel->rpn_lsb == 0;
}

static void synth_set_pitch_bend_range(synth_t *synth, uint8_t channel_index, double semitones) {
    if (semitones < 0.0) semitones = 0.0;
    if (semitones > 127.99) semitones = 127.99;
    synth->channels[channel_index].pitch_bend_range = semitones;
    synth_apply_pitch_to_channel(synth, channel_index);
}

static void synth_handle_event(synth_t *synth, const midi_event_t *event) {
    if (event->type != MIDI_EVENT_CHANNEL) return;
    uint8_t status = event->status & 0xf0u;
    uint8_t channel_index = event->status & 0x0fu;
    midi_channel_t *channel = &synth->channels[channel_index];

    switch (status) {
        case 0x80:
            synth_note_off(synth, channel_index, event->data1);
            break;
        case 0x90:
            if (event->data2 == 0) {
                synth_note_off(synth, channel_index, event->data1);
            } else {
                synth_note_on(synth, channel_index, event->data1, event->data2);
            }
            break;
        case 0xb0:
            switch (event->data1) {
                case 0x00:
                    channel->bank_msb = event->data2 & 0x7f;
                    break;
                case 0x20:
                    channel->bank_lsb = event->data2 & 0x7f;
                    break;
                case 0x07:
                    channel->volume = event->data2 & 0x7f;
                    break;
                case 0x0a:
                    channel->pan = event->data2 & 0x7f;
                    break;
                case 0x0b:
                    channel->expression = event->data2 & 0x7f;
                    break;
                case 0x06:
                    if (channel_uses_pitch_bend_range_rpn(channel)) {
                        double cents = channel->pitch_bend_range - floor(channel->pitch_bend_range);
                        synth_set_pitch_bend_range(synth, channel_index, (double) (event->data2 & 0x7f) + cents);
                    }
                    break;
                case 0x26:
                    if (channel_uses_pitch_bend_range_rpn(channel)) {
                        double whole = floor(channel->pitch_bend_range);
                        uint8_t cents = event->data2 & 0x7f;
                        if (cents > 99) cents = 99;
                        synth_set_pitch_bend_range(synth, channel_index, whole + (double) cents / 100.0);
                    }
                    break;
                case 0x40:
                    if (event->data2 >= 64) {
                        channel->sustain = true;
                    } else {
                        channel->sustain = false;
                        for (int i = 0; i < MAX_VOICES; ++i) {
                            synth_voice_t *voice = &synth->voices[i];
                            if (voice->active && voice->channel == channel_index && voice->sustained) {
                                synth_release_voice(voice);
                            }
                        }
                    }
                    break;
                case 0x78:
                    synth_all_notes_off(synth, channel_index, true);
                    break;
                case 0x79:
                    channel->volume = 100;
                    channel->expression = 127;
                    channel->pan = 64;
                    channel->bank_msb = 0;
                    channel->bank_lsb = 0;
                    channel->pitch_bend = 8192;
                    channel->pitch_bend_range = DEFAULT_PITCH_BEND_RANGE_SEMITONES;
                    channel->rpn_msb = 127;
                    channel->rpn_lsb = 127;
                    channel->sustain = false;
                    synth_apply_pitch_to_channel(synth, channel_index);
                    break;
                case 0x7b:
                    synth_all_notes_off(synth, channel_index, false);
                    break;
                case 0x60:
                    if (channel_uses_pitch_bend_range_rpn(channel)) {
                        synth_set_pitch_bend_range(synth, channel_index, floor(channel->pitch_bend_range) + 1.0);
                    }
                    break;
                case 0x61:
                    if (channel_uses_pitch_bend_range_rpn(channel)) {
                        synth_set_pitch_bend_range(synth, channel_index, floor(channel->pitch_bend_range) - 1.0);
                    }
                    break;
                case 0x64:
                    channel->rpn_lsb = event->data2 & 0x7f;
                    break;
                case 0x65:
                    channel->rpn_msb = event->data2 & 0x7f;
                    break;
                default:
                    break;
            }
            break;
        case 0xc0:
            channel->program = event->data1 & 0x7f;
            break;
        case 0xe0:
            channel->pitch_bend = ((int) event->data2 << 7) | (int) event->data1;
            synth_apply_pitch_to_channel(synth, channel_index);
            break;
        default:
            break;
    }
}

static void synth_render_one(synth_t *synth, double *left, double *right) {
    double l = 0.0;
    double r = 0.0;

    for (int i = 0; i < MAX_VOICES; ++i) {
        synth_voice_t *voice = &synth->voices[i];
        if (!voice->active) continue;

        if ((voice->age & 255u) == 0 && !voice->percussion) {
            if (voice->attack_target) {
                uint8_t target = voice->attack_target;
                uint8_t delta = (uint8_t) (target - voice->env_level);
                uint8_t step = (uint8_t) ((delta >> voice->decay_shift) | 1u);
                if ((uint16_t) voice->env_level + step >= target) {
                    voice->env_level = target;
                    voice->attack_target = 0;
                    voice->decay_shift = gm_envelopes[synth->channels[voice->channel].program & 127u].decay_shift;
                } else {
                    voice->env_level = (uint8_t) (voice->env_level + step);
                }
            } else if (voice->env_level > voice->sustain_level) {
                uint8_t delta = (uint8_t) (voice->env_level - voice->sustain_level);
                uint8_t step = (uint8_t) ((delta >> voice->decay_shift) | 1u);
                voice->env_level = step >= voice->env_level ? 0 : (uint8_t) (voice->env_level - step);
                if (voice->env_level == 0 && voice->sustain_level == 0) {
                    voice->active = false;
                    continue;
                }
            }
        }

        double sample = wave_sample_mono(voice->wave, voice->position);
        midi_channel_t *channel = &synth->channels[voice->channel];
        double channel_gain = ((double) channel->volume / 127.0) * ((double) channel->expression / 127.0);
        double env_gain = (double) voice->env_level / 127.0;
        double gain = env_gain * channel_gain * voice->attenuation_gain;
        if (voice->percussion) gain *= voice->percussion_gain;
        double pan = (double) channel->pan / 127.0;
        double pan_l = cos(pan * M_PI * 0.5);
        double pan_r = sin(pan * M_PI * 0.5);

        l += sample * gain * pan_l;
        r += sample * gain * pan_r;

        const bool region_looped = voice->region->looped || (!voice->region->has_wsmp && voice->wave->looped);
        const uint32_t loop_start = voice->region->looped ? voice->region->loop_start : voice->wave->loop_start;
        const uint32_t loop_length = voice->region->looped ? voice->region->loop_length : voice->wave->loop_length;
        double loop_end = (double) loop_start + (double) loop_length;

        voice->position += voice->step;
        if (region_looped && loop_length > 1 && loop_end <= (double) voice->wave->frame_count) {
            while (voice->position >= loop_end) {
                voice->position = (double) loop_start + fmod(voice->position - loop_end, (double) loop_length);
            }
        } else if (voice->position >= (double) voice->wave->frame_count) {
            voice->active = false;
        }

        if (voice->percussion) {
            voice->percussion_gain *= voice->percussion_decay;
            if (voice->percussion_gain < 0.001) {
                voice->active = false;
            }
        }

        voice->age++;
    }

    *left = l;
    *right = r;
}

static void wr_u16le(FILE *f, uint16_t value) {
    uint8_t b[2] = {(uint8_t) value, (uint8_t) (value >> 8)};
    fwrite(b, 1, 2, f);
}

static void wr_u32le(FILE *f, uint32_t value) {
    uint8_t b[4] = {
        (uint8_t) value,
        (uint8_t) (value >> 8),
        (uint8_t) (value >> 16),
        (uint8_t) (value >> 24),
    };
    fwrite(b, 1, 4, f);
}

static bool wav_writer_open(wav_writer_t *writer, const char *path, uint32_t sample_rate) {
    memset(writer, 0, sizeof(*writer));
    writer->file = fopen(path, "wb");
    if (!writer->file) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return false;
    }
    writer->sample_rate = sample_rate;

    fwrite("RIFF", 1, 4, writer->file);
    wr_u32le(writer->file, 0);
    fwrite("WAVE", 1, 4, writer->file);
    fwrite("fmt ", 1, 4, writer->file);
    wr_u32le(writer->file, 16);
    wr_u16le(writer->file, 1);
    wr_u16le(writer->file, 2);
    wr_u32le(writer->file, sample_rate);
    wr_u32le(writer->file, sample_rate * 2u * 2u);
    wr_u16le(writer->file, 4);
    wr_u16le(writer->file, 16);
    fwrite("data", 1, 4, writer->file);
    wr_u32le(writer->file, 0);
    return true;
}

static int16_t float_to_i16(double x) {
    const double master_gain = 0.45;
    x *= master_gain;
    if (x > 1.0) x = 1.0;
    if (x < -1.0) x = -1.0;
    return (int16_t) lrint(x * 32767.0);
}

static void wav_writer_sample(wav_writer_t *writer, double left, double right) {
    int16_t l = float_to_i16(left);
    int16_t r = float_to_i16(right);
    wr_u16le(writer->file, (uint16_t) l);
    wr_u16le(writer->file, (uint16_t) r);
    writer->frames_written++;
}

static bool wav_writer_close(wav_writer_t *writer) {
    if (!writer->file) return true;
    uint32_t data_size = writer->frames_written * 4u;
    uint32_t riff_size = 36u + data_size;
    bool ok = true;

    if (fseek(writer->file, 4, SEEK_SET) != 0) ok = false;
    wr_u32le(writer->file, riff_size);
    if (fseek(writer->file, 40, SEEK_SET) != 0) ok = false;
    wr_u32le(writer->file, data_size);
    if (fclose(writer->file) != 0) ok = false;
    writer->file = NULL;
    return ok;
}

static bool render_samples(synth_t *synth, wav_writer_t *writer, uint64_t sample_count) {
    for (uint64_t i = 0; i < sample_count; ++i) {
        double left = 0.0;
        double right = 0.0;
        synth_render_one(synth, &left, &right);
        wav_writer_sample(writer, left, right);
    }
    return true;
}

static bool render_midi_to_wav(const midi_file_t *midi, const dls_bank_t *bank,
                               const char *wav_path, uint32_t sample_rate) {
    wav_writer_t writer = {0};
    if (!wav_writer_open(&writer, wav_path, sample_rate)) return false;

    synth_t synth;
    synth_init(&synth, bank, sample_rate);

    uint64_t current_tick = 0;
    uint64_t current_sample = 0;
    double exact_sample = 0.0;
    uint32_t tempo = 500000;

    for (size_t i = 0; i < midi->event_count; ++i) {
        const midi_event_t *event = &midi->events[i];
        if (event->tick > current_tick) {
            uint64_t delta_ticks = event->tick - current_tick;
            exact_sample += (double) delta_ticks * (double) tempo * (double) sample_rate /
                            (1000000.0 * (double) midi->division);
            uint64_t target_sample = (uint64_t) llround(exact_sample);
            if (target_sample > current_sample) {
                render_samples(&synth, &writer, target_sample - current_sample);
                current_sample = target_sample;
            }
            current_tick = event->tick;
        }

        if (event->type == MIDI_EVENT_TEMPO) {
            tempo = event->tempo_us_per_quarter ? event->tempo_us_per_quarter : 500000;
        } else {
            synth_handle_event(&synth, event);
        }
    }

    for (int i = 0; i < MAX_VOICES; ++i) {
        if (synth.voices[i].active && !synth.voices[i].percussion) {
            synth_release_voice(&synth.voices[i]);
        }
    }

    uint64_t tail_limit = (uint64_t) sample_rate * 4u;
    while (tail_limit-- && synth_has_active_voices(&synth)) {
        render_samples(&synth, &writer, 1);
    }

    bool ok = wav_writer_close(&writer);
    fprintf(stderr, "WAV: %u Hz stereo, %u frames written to %s\n",
            sample_rate, writer.frames_written, wav_path);
    return ok;
}

static const char *find_default_dls(void) {
    static const char *candidates[] = {
        "gm.dls",
        "GM.DLS",
        "C:\\Windows\\System32\\drivers\\gm.dls",
        "C:\\Windows\\SysWOW64\\drivers\\gm.dls",
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            return candidates[i];
        }
    }
    return NULL;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s input.mid output.wav [gm.dls] [sample-rate]\n"
            "renders a Standard MIDI File through a RIFF DLS GM bank to 16-bit stereo WAV\n",
            argv0);
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        usage(argv[0]);
        return 2;
    }

    const char *midi_path = argv[1];
    const char *wav_path = argv[2];
    const char *dls_path = argc >= 4 ? argv[3] : find_default_dls();
    uint32_t sample_rate = DEFAULT_SAMPLE_RATE;
    if (!dls_path) {
        fprintf(stderr, "gm.dls not found; pass the path explicitly\n");
        return 2;
    }
    if (argc >= 5) {
        char *end = NULL;
        unsigned long parsed = strtoul(argv[4], &end, 10);
        if (!end || *end || parsed < 8000 || parsed > 192000) {
            fprintf(stderr, "invalid sample rate: %s\n", argv[4]);
            return 2;
        }
        sample_rate = (uint32_t) parsed;
    }

    dls_bank_t bank;
    if (!dls_bank_load(dls_path, &bank)) return 1;

    midi_file_t midi;
    if (!midi_file_load(midi_path, &midi)) {
        dls_bank_free(&bank);
        return 1;
    }

    bool ok = render_midi_to_wav(&midi, &bank, wav_path, sample_rate);
    midi_file_free(&midi);
    dls_bank_free(&bank);
    return ok ? 0 : 1;
}
