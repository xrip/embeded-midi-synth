// Minimal Gravis UltraSound GF1 .pat + TiMidity cfg parser.
//
// Scope: classic GF1PATCH110 files and simple dgguspat-style mappings:
//   bank N
//   drumset N
//   <program-or-note> <patchname> [ignored options...]
#pragma once

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUS_MODE_16BIT     0x01u
#define GUS_MODE_UNSIGNED  0x02u
#define GUS_MODE_LOOPING   0x04u
#define GUS_MODE_ENVELOPE  0x40u

typedef struct {
    uint8_t *data;
    size_t size;
} gus_blob_t;

typedef struct {
    char name[8];
    uint32_t data_length;
    uint32_t loop_start;
    uint32_t loop_end;
    uint16_t sample_rate;
    uint32_t low_freq;
    uint32_t high_freq;
    uint32_t root_freq;
    int16_t tune;
    uint8_t balance;
    uint8_t env_rate[6];
    uint8_t env_offset[6];
    uint8_t modes;
    int16_t scale_frequency;
    uint16_t scale_factor;
    const uint8_t *pcm;
} gus_sample_t;

typedef struct {
    gus_blob_t blob;
    char path[512];
    char name[64];
    gus_sample_t *samples;
    size_t sample_count;
} gus_patch_t;

typedef struct {
    int present;
    uint32_t bank;
    char patch[128];
} gus_melodic_map_t;

typedef struct {
    int present;
    uint32_t drumset;
    char patch[128];
} gus_drum_map_t;

typedef struct {
    char dir[512];
    gus_melodic_map_t melodic[128];
    gus_drum_map_t drums[128];
    uint32_t warnings;
    uint32_t ignored_options;
} gus_cfg_t;

static uint16_t gus_rd_u16le(const uint8_t *p) {
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static int16_t gus_rd_i16le(const uint8_t *p) {
    return (int16_t) gus_rd_u16le(p);
}

static uint32_t gus_rd_u32le(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static bool gus_read_entire_file(const char *path, gus_blob_t *out) {
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

static void gus_blob_free(gus_blob_t *blob) {
    free(blob->data);
    blob->data = NULL;
    blob->size = 0;
}

static char *gus_trim(char *s) {
    while (*s && isspace((unsigned char) *s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char) e[-1])) *--e = '\0';
    return s;
}

static void gus_cfg_dirname(const char *path, char out[512]) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    if (!slash) {
        strcpy(out, ".");
        return;
    }
    size_t n = (size_t) (slash - path);
    if (n >= 511) n = 511;
    memcpy(out, path, n);
    out[n] = '\0';
}

static void gus_join_patch_path(const char *dir, const char *patch, char out[512]) {
    const char *base = strrchr(patch, '/');
    const char *base2 = strrchr(patch, '\\');
    if (!base || (base2 && base2 > base)) base = base2;
    base = base ? base + 1 : patch;
    int has_ext = strrchr(base, '.') != NULL;
    const char sep = (strchr(dir, '\\') != NULL) ? '\\' : '/';
    if (strcmp(dir, ".") == 0) {
        snprintf(out, 512, "%s%s", patch, has_ext ? "" : ".pat");
    } else {
        snprintf(out, 512, "%s%c%s%s", dir, sep, patch, has_ext ? "" : ".pat");
    }
}

static bool gus_cfg_load(const char *path, gus_cfg_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    gus_cfg_dirname(path, cfg->dir);

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return false;
    }

    uint32_t current_bank = 0;
    uint32_t current_drumset = 0;
    int in_drumset = 0;
    char line[1024];
    uint32_t line_no = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *s = gus_trim(line);
        if (!*s) continue;

        char *tok = strtok(s, " \t\r\n");
        if (!tok) continue;

        if (strcmp(tok, "bank") == 0) {
            char *n = strtok(NULL, " \t\r\n");
            if (!n) {
                fprintf(stderr, "%s:%u: missing bank number\n", path, line_no);
                cfg->warnings++;
                continue;
            }
            current_bank = (uint32_t) strtoul(n, NULL, 10);
            in_drumset = 0;
            continue;
        }
        if (strcmp(tok, "drumset") == 0) {
            char *n = strtok(NULL, " \t\r\n");
            if (!n) {
                fprintf(stderr, "%s:%u: missing drumset number\n", path, line_no);
                cfg->warnings++;
                continue;
            }
            current_drumset = (uint32_t) strtoul(n, NULL, 10);
            in_drumset = 1;
            continue;
        }
        if (!isdigit((unsigned char) tok[0])) {
            fprintf(stderr, "%s:%u: warning: ignored unsupported directive '%s'\n", path, line_no, tok);
            cfg->warnings++;
            continue;
        }

        char *endp = NULL;
        unsigned long number = strtoul(tok, &endp, 10);
        if (!endp || *endp || number > 127) {
            fprintf(stderr, "%s:%u: warning: ignored unsupported mapping '%s'\n", path, line_no, tok);
            cfg->warnings++;
            continue;
        }

        char *patch = strtok(NULL, " \t\r\n");
        if (!patch) {
            fprintf(stderr, "%s:%u: warning: mapping without patch name\n", path, line_no);
            cfg->warnings++;
            continue;
        }
        char *extra = strtok(NULL, " \t\r\n");
        if (extra) cfg->ignored_options++;

        if (in_drumset) {
            gus_drum_map_t *m = &cfg->drums[number];
            m->present = 1;
            m->drumset = current_drumset;
            snprintf(m->patch, sizeof(m->patch), "%s", patch);
        } else {
            gus_melodic_map_t *m = &cfg->melodic[number];
            m->present = 1;
            m->bank = current_bank;
            snprintf(m->patch, sizeof(m->patch), "%s", patch);
        }
    }

    fclose(f);
    return true;
}

static int gus_freq_to_key(uint32_t milli_hz) {
    if (milli_hz < 1000) return 60;
    double hz = (double) milli_hz / 1000.0;
    int key = (int) llround(69.0 + 12.0 * log2(hz / 440.0));
    if (key < 0) key = 0;
    if (key > 127) key = 127;
    return key;
}

static bool gus_patch_load(const char *path, gus_patch_t *patch) {
    memset(patch, 0, sizeof(*patch));
    snprintf(patch->path, sizeof(patch->path), "%s", path);
    if (!gus_read_entire_file(path, &patch->blob)) return false;

    const uint8_t *p = patch->blob.data;
    size_t size = patch->blob.size;
    if (size < 129 || memcmp(p, "GF1PATCH110", 11) != 0) {
        fprintf(stderr, "%s is not a GF1PATCH110 file\n", path);
        gus_blob_free(&patch->blob);
        return false;
    }

    uint8_t instruments = p[82];
    size_t off = 129;
    size_t cap = 0;
    for (uint8_t ins = 0; ins < instruments; ++ins) {
        if (off + 63 > size) break;
        if (ins == 0) {
            size_t n = 16;
            while (n && p[off + 2 + n - 1] == 0) n--;
            if (n >= sizeof(patch->name)) n = sizeof(patch->name) - 1;
            memcpy(patch->name, p + off + 2, n);
            patch->name[n] = '\0';
        }
        uint8_t layers = p[off + 22];
        off += 63;
        for (uint8_t layer = 0; layer < layers; ++layer) {
            if (off + 47 > size) break;
            uint8_t samples = p[off + 6];
            off += 47;
            if (patch->sample_count + samples > cap) {
                size_t next = cap ? cap * 2 : 8;
                while (next < patch->sample_count + samples) next *= 2;
                gus_sample_t *new_samples = realloc(patch->samples, next * sizeof(*new_samples));
                if (!new_samples) {
                    fprintf(stderr, "out of memory parsing %s\n", path);
                    gus_blob_free(&patch->blob);
                    free(patch->samples);
                    memset(patch, 0, sizeof(*patch));
                    return false;
                }
                patch->samples = new_samples;
                cap = next;
            }
            for (uint8_t s = 0; s < samples; ++s) {
                if (off + 96 > size) break;
                gus_sample_t *sm = &patch->samples[patch->sample_count++];
                memset(sm, 0, sizeof(*sm));
                memcpy(sm->name, p + off, 7);
                sm->name[7] = '\0';
                sm->data_length = gus_rd_u32le(p + off + 8);
                sm->loop_start = gus_rd_u32le(p + off + 12);
                sm->loop_end = gus_rd_u32le(p + off + 16);
                sm->sample_rate = gus_rd_u16le(p + off + 20);
                sm->low_freq = gus_rd_u32le(p + off + 22);
                sm->high_freq = gus_rd_u32le(p + off + 26);
                sm->root_freq = gus_rd_u32le(p + off + 30);
                sm->tune = gus_rd_i16le(p + off + 34);
                sm->balance = p[off + 36];
                memcpy(sm->env_rate, p + off + 37, 6);
                memcpy(sm->env_offset, p + off + 43, 6);
                sm->modes = p[off + 55];
                sm->scale_frequency = gus_rd_i16le(p + off + 56);
                sm->scale_factor = gus_rd_u16le(p + off + 58);
                off += 96;
                if (off + sm->data_length > size) {
                    fprintf(stderr, "%s: truncated sample data\n", path);
                    sm->data_length = (off < size) ? (uint32_t) (size - off) : 0;
                }
                sm->pcm = p + off;
                off += sm->data_length;
            }
        }
    }

    if (patch->sample_count == 0) {
        fprintf(stderr, "%s: no samples\n", path);
        gus_blob_free(&patch->blob);
        free(patch->samples);
        memset(patch, 0, sizeof(*patch));
        return false;
    }
    return true;
}

static void gus_patch_free(gus_patch_t *patch) {
    free(patch->samples);
    gus_blob_free(&patch->blob);
    memset(patch, 0, sizeof(*patch));
}
