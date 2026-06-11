// Offline GUS .pat / TiMidity cfg -> flash soundbank packer.
//
//   gus_pack <timidity.cfg> <out.bin> <output_rate>
//
// Produces the same GMWB v4 blob as dls_pack, without changing the runtime
// synth. This is intentionally a dgguspat-oriented MVP, not a full TiMidity or
// UltraMID emulator.
#include <math.h>

#include "gus_parse.c.inl"
#include "../gm_bank.h"

typedef struct {
    void *data;
    size_t count;
    size_t cap;
    size_t elem;
} vec_t;

static void vec_init(vec_t *v, size_t elem) {
    v->data = NULL;
    v->count = 0;
    v->cap = 0;
    v->elem = elem;
}

static void *vec_push(vec_t *v) {
    if (v->count == v->cap) {
        size_t next = v->cap ? v->cap * 2 : 1024;
        void *p = realloc(v->data, next * v->elem);
        if (!p) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        v->data = p;
        v->cap = next;
    }
    void *slot = (uint8_t *) v->data + v->count * v->elem;
    v->count++;
    return slot;
}

static uint32_t coef_to_q16(double coef) {
    if (coef < 0.0) coef = 0.0;
    if (coef > 0.99998) coef = 0.99998;
    return (uint32_t) llround(coef * (double) GM_ONE_Q16);
}

static uint32_t unit_to_q16(double value) {
    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;
    return (uint32_t) llround(value * (double) GM_ONE_Q16);
}

static double decay_coef_for_seconds(double seconds, uint32_t sample_rate) {
    if (seconds <= 0.0) return 0.0;
    return pow(10.0, -4.8 / (seconds * (double) sample_rate));
}

static int16_t clamp_i16(long v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t) v;
}

static int key_low_for_sample(const gus_patch_t *patch, size_t index) {
    const gus_sample_t *sm = &patch->samples[index];
    if (sm->low_freq >= 1000 && sm->low_freq <= sm->high_freq) return gus_freq_to_key(sm->low_freq);
    (void) patch;
    return 0;
}

static int key_high_for_sample(const gus_patch_t *patch, size_t index) {
    const gus_sample_t *sm = &patch->samples[index];
    if (sm->high_freq >= 1000 && sm->low_freq <= sm->high_freq) return gus_freq_to_key(sm->high_freq);
    (void) patch;
    return 127;
}

static double env_segment_seconds(uint8_t from, uint8_t to, uint8_t rate) {
    if (rate == 0) return 0.0;
    double delta = fabs((double) to - (double) from) / 255.0;
    double seconds = delta * 64.0 / (double) rate;
    if (seconds < 0.002) seconds = 0.002;
    if (seconds > 8.0) seconds = 8.0;
    return seconds;
}

static void bake_gus_env(const gus_sample_t *sm, uint32_t output_rate, gm_region_t *out) {
    double attack_s = 0.005;
    double decay_s = 0.35;
    double release_s = 0.25;
    double sustain = 0.85;

    if (sm->modes & GUS_MODE_ENVELOPE) {
        attack_s = env_segment_seconds(sm->env_offset[0], sm->env_offset[1], sm->env_rate[0]);
        decay_s = env_segment_seconds(sm->env_offset[1], sm->env_offset[2], sm->env_rate[1]);
        release_s = env_segment_seconds(sm->env_offset[3], sm->env_offset[5], sm->env_rate[4]);
        sustain = (double) sm->env_offset[2] / 255.0;
        if (sustain < 0.02) sustain = 0.02;
    }

    double attack_step = attack_s <= 0.0 ? 1.0 : 1.0 / (attack_s * (double) output_rate);
    out->attack_step_q16 = unit_to_q16(attack_step);
    out->decay_coef_q16 = coef_to_q16(decay_coef_for_seconds(decay_s, output_rate));
    out->release_coef_q16 = coef_to_q16(decay_coef_for_seconds(release_s, output_rate));
    out->sustain_q16 = unit_to_q16(sustain);
}

static uint32_t append_wave(const gus_sample_t *sm, uint32_t output_rate, vec_t *waves, vec_t *pcm) {
    gm_wave_t *w = vec_push(waves);
    memset(w, 0, sizeof(*w));
    w->pcm_offset = (uint32_t) pcm->count;
    w->base_step_q16 = (uint32_t) llround((double) sm->sample_rate / (double) output_rate * (double) GM_ONE_Q16);

    int is_16 = (sm->modes & GUS_MODE_16BIT) != 0;
    int is_unsigned = (sm->modes & GUS_MODE_UNSIGNED) != 0;
    uint32_t frames = is_16 ? sm->data_length / 2u : sm->data_length;
    w->frame_count = frames;

    for (uint32_t i = 0; i < frames; ++i) {
        long v;
        if (is_16) {
            const uint8_t *p = sm->pcm + (size_t) i * 2u;
            if (is_unsigned) v = (long) gus_rd_u16le(p) - 32768L;
            else v = (long) gus_rd_i16le(p);
        } else {
            uint8_t b = sm->pcm[i];
            v = is_unsigned ? ((long) b - 128L) << 8 : ((int8_t) b) << 8;
        }
        int16_t *dst = vec_push(pcm);
        *dst = clamp_i16(v);
    }

    return (uint32_t) (waves->count - 1);
}

typedef struct {
    uint32_t instruments;
    uint32_t melodic_regions;
    uint32_t drum_regions;
    uint32_t waves;
    uint32_t unsupported_bidir;
    uint32_t unsupported_backward;
    uint32_t ignored_extra_samples;
    uint32_t failed_patches;
} gus_pack_stats_t;

static void append_patch_regions(const gus_patch_t *patch, uint8_t fixed_key, int drum,
                                 uint32_t output_rate, vec_t *waves, vec_t *regions,
                                 vec_t *pcm, uint16_t *region_count, gus_pack_stats_t *stats) {
    size_t playable = 0;
    for (size_t i = 0; i < patch->sample_count; ++i) {
        const gus_sample_t *sm = &patch->samples[i];
        int is_16 = (sm->modes & GUS_MODE_16BIT) != 0;
        uint32_t frames = is_16 ? sm->data_length / 2u : sm->data_length;
        if (frames > 1 && sm->sample_rate > 1000) playable++;
    }

    for (size_t i = 0; i < patch->sample_count; ++i) {
        const gus_sample_t *sm = &patch->samples[i];
        int is_16 = (sm->modes & GUS_MODE_16BIT) != 0;
        uint32_t frames = is_16 ? sm->data_length / 2u : sm->data_length;
        if (frames <= 1 || sm->sample_rate <= 1000) continue;
        if (!drum && playable > 1 && sm->low_freq < 1000 && sm->high_freq < 1000 && *region_count != 0) {
            stats->ignored_extra_samples++;
            continue;
        }

        uint32_t wave_index = append_wave(sm, output_rate, waves, pcm);
        gm_region_t *rg = vec_push(regions);
        memset(rg, 0, sizeof(*rg));
        rg->wave_index = (uint16_t) wave_index;
        rg->vel_low = 0;
        rg->vel_high = 127;
        rg->gain_q16 = GM_ONE_Q16;
        rg->root_key = (uint8_t) gus_freq_to_key(sm->root_freq);
        rg->fine_cents = sm->tune;
        int pan = (int) llround(((double) sm->balance - 7.0) / 7.0 * 63.0);
        if (pan < -64) pan = -64;
        if (pan > 63) pan = 63;
        rg->pan = (int8_t) pan;

        if (drum) {
            rg->key_low = fixed_key;
            rg->key_high = fixed_key;
        } else {
            int lo = key_low_for_sample(patch, i);
            int hi = key_high_for_sample(patch, i);
            if (hi < lo) {
                int t = lo;
                lo = hi;
                hi = t;
            }
            rg->key_low = (uint8_t) lo;
            rg->key_high = (uint8_t) hi;
        }

        if (sm->root_freq < 1000) rg->flags |= GM_RGN_ROOT_FROM_NOTE;

        uint32_t loop_start = sm->loop_start;
        uint32_t loop_end = sm->loop_end;
        if (is_16) {
            loop_start /= 2u;
            loop_end /= 2u;
        }
        if ((sm->modes & GUS_MODE_LOOPING) && loop_start < frames && loop_end > loop_start + 1) {
            if (loop_end > frames) loop_end = frames;
            rg->loop_start = loop_start;
            rg->loop_length = loop_end - loop_start;
            rg->flags |= GM_RGN_LOOPED;
        }

        bake_gus_env(sm, output_rate, rg);
        (*region_count)++;
        if (drum) stats->drum_regions++;
        else stats->melodic_regions++;
        if (sm->modes & 0x08u) stats->unsupported_bidir++;
        if (sm->modes & 0x10u) stats->unsupported_backward++;
    }
}

static void write_bank(const char *out_path, uint32_t output_rate,
                       const vec_t *instruments, const vec_t *regions,
                       const vec_t *waves, const vec_t *pcm) {
    gm_bank_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic[0] = GM_BANK_MAGIC0;
    hdr.magic[1] = GM_BANK_MAGIC1;
    hdr.magic[2] = GM_BANK_MAGIC2;
    hdr.magic[3] = GM_BANK_MAGIC3;
    hdr.version = GM_BANK_VERSION;
    hdr.output_rate = output_rate;
    hdr.instrument_count = (uint32_t) instruments->count;
    hdr.region_count = (uint32_t) regions->count;
    hdr.wave_count = (uint32_t) waves->count;
    hdr.off_instruments = (uint32_t) sizeof(hdr);
    hdr.off_regions = hdr.off_instruments + (uint32_t) (instruments->count * sizeof(gm_instrument_t));
    hdr.off_waves = hdr.off_regions + (uint32_t) (regions->count * sizeof(gm_region_t));
    hdr.off_pcm = hdr.off_waves + (uint32_t) (waves->count * sizeof(gm_wave_t));
    hdr.pcm_samples = (uint32_t) pcm->count;

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "open %s: %s\n", out_path, strerror(errno));
        exit(1);
    }
    int ok = 1;
    ok &= fwrite(&hdr, sizeof(hdr), 1, f) == 1;
    ok &= fwrite(instruments->data, sizeof(gm_instrument_t), instruments->count, f) == instruments->count;
    ok &= fwrite(regions->data, sizeof(gm_region_t), regions->count, f) == regions->count;
    ok &= fwrite(waves->data, sizeof(gm_wave_t), waves->count, f) == waves->count;
    ok &= fwrite(pcm->data, sizeof(int16_t), pcm->count, f) == pcm->count;
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        fprintf(stderr, "write %s failed\n", out_path);
        exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <timidity.cfg> <out.bin> <output_rate>\n", argv[0]);
        return 2;
    }

    const char *cfg_path = argv[1];
    const char *out_path = argv[2];
    char *endp = NULL;
    unsigned long rate_ul = strtoul(argv[3], &endp, 10);
    if (!endp || *endp || rate_ul < 8000 || rate_ul > 192000) {
        fprintf(stderr, "invalid output_rate: %s\n", argv[3]);
        return 2;
    }
    uint32_t output_rate = (uint32_t) rate_ul;

    gus_cfg_t cfg;
    if (!gus_cfg_load(cfg_path, &cfg)) return 1;

    vec_t waves, regions, instruments, pcm;
    vec_init(&waves, sizeof(gm_wave_t));
    vec_init(&regions, sizeof(gm_region_t));
    vec_init(&instruments, sizeof(gm_instrument_t));
    vec_init(&pcm, sizeof(int16_t));

    gus_pack_stats_t stats = {0};

    for (uint32_t program = 0; program < 128; ++program) {
        const gus_melodic_map_t *map = &cfg.melodic[program];
        if (!map->present) continue;
        char path[512];
        gus_join_patch_path(cfg.dir, map->patch, path);
        gus_patch_t patch;
        if (!gus_patch_load(path, &patch)) {
            stats.failed_patches++;
            continue;
        }

        gm_instrument_t *ins = vec_push(&instruments);
        memset(ins, 0, sizeof(*ins));
        ins->bank = map->bank;
        ins->program = (uint16_t) program;
        ins->region_first = (uint32_t) regions.count;

        append_patch_regions(&patch, 0, 0, output_rate, &waves, &regions, &pcm,
                             &ins->region_count, &stats);
        if (ins->region_count == 0) instruments.count--;
        else stats.instruments++;
        gus_patch_free(&patch);
    }

    uint32_t drum_first = (uint32_t) regions.count;
    uint16_t drum_count = 0;
    for (uint32_t note = 0; note < 128; ++note) {
        const gus_drum_map_t *map = &cfg.drums[note];
        if (!map->present) continue;
        char path[512];
        gus_join_patch_path(cfg.dir, map->patch, path);
        gus_patch_t patch;
        if (!gus_patch_load(path, &patch)) {
            stats.failed_patches++;
            continue;
        }
        append_patch_regions(&patch, (uint8_t) note, 1, output_rate, &waves, &regions, &pcm,
                             &drum_count, &stats);
        gus_patch_free(&patch);
    }
    if (drum_count > 0) {
        gm_instrument_t *ins = vec_push(&instruments);
        memset(ins, 0, sizeof(*ins));
        ins->bank = DLS_DRUM_BANK;
        ins->program = 0;
        ins->region_first = drum_first;
        ins->region_count = drum_count;
        stats.instruments++;
    }

    if (waves.count > 65535) {
        fprintf(stderr, "too many waves (%zu) for uint16 wave_index\n", waves.count);
        return 1;
    }

    write_bank(out_path, output_rate, &instruments, &regions, &waves, &pcm);

    stats.waves = (uint32_t) waves.count;
    double pcm_mb = (double) (pcm.count * sizeof(int16_t)) / (1024.0 * 1024.0);
    double total_mb = (double) (sizeof(gm_bank_header_t) +
                                instruments.count * sizeof(gm_instrument_t) +
                                regions.count * sizeof(gm_region_t) +
                                waves.count * sizeof(gm_wave_t) +
                                pcm.count * sizeof(int16_t)) / (1024.0 * 1024.0);
    fprintf(stderr,
            "GUS: %u instruments, %u melodic regions, %u drum regions, %u waves, "
            "%u samples (%.2f MB PCM, %.2f MB total) @ %u Hz -> %s\n",
            stats.instruments, stats.melodic_regions, stats.drum_regions, stats.waves,
            (uint32_t) pcm.count, pcm_mb, total_mb, output_rate, out_path);
    fprintf(stderr,
            "  diagnostics: %u cfg warnings, %u ignored cfg option lines, "
            "%u failed patches, %u extra samples ignored, %u bidir loops ignored, "
            "%u backward loops ignored\n",
            cfg.warnings, cfg.ignored_options, stats.failed_patches, stats.ignored_extra_samples,
            stats.unsupported_bidir, stats.unsupported_backward);

    free(waves.data);
    free(regions.data);
    free(instruments.data);
    free(pcm.data);
    return stats.failed_patches ? 1 : 0;
}
