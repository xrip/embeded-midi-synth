// Offline analysis: what would µ-law 8-bit cost us on the real bank?
//
//   mulaw_probe <gm_bank.bin>
//
// Runs every wave's PCM through 16 -> µ-law(8-bit) -> 16 and reports the
// reconstruction SNR (energy-weighted overall, plus the per-wave distribution
// with the worst offenders called out), then the exact flash delta vs the
// current 16-bit PCM block. Read-only: it does not write a new bank.
//
// µ-law here is standard G.711 (mu=255) working in its native 14-bit domain,
// bridged to 16-bit by >>2 on encode and <<2 on decode -- i.e. the bottom 2
// bits are dropped, which is inherent to 8-bit companding. The on-device decode
// would be a single 256-entry int16 LUT load (no state, random access intact).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dls_parse.c.inl"   // file_blob_t, read_entire_file
#include "../gm_bank.h"

// ---- standard G.711 µ-law (Sun/CCITT reference, 14-bit internal) -------------
#define ULAW_BIAS 0x84
#define ULAW_CLIP 8159

static int ulaw_seg_end[8] = {0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF};

static int ulaw_search(int val) {
    for (int i = 0; i < 8; ++i) if (val <= ulaw_seg_end[i]) return i;
    return 8;
}

// 16-bit signed -> 8-bit µ-law (drops the low 2 bits via the >>2 prescale).
static uint8_t linear2ulaw(int16_t pcm16) {
    int pcm_val = pcm16 >> 2;   // 16-bit -> 14-bit domain
    int mask;
    if (pcm_val < 0) { pcm_val = -pcm_val; mask = 0x7F; } else { mask = 0xFF; }
    if (pcm_val > ULAW_CLIP) pcm_val = ULAW_CLIP;
    pcm_val += (ULAW_BIAS >> 2);
    int seg = ulaw_search(pcm_val);
    if (seg >= 8) return (uint8_t) (0x7F ^ mask);
    uint8_t uval = (uint8_t) ((seg << 4) | ((pcm_val >> (seg + 1)) & 0xF));
    return (uint8_t) (uval ^ mask);
}

// 8-bit µ-law -> 16-bit signed. The larger BIAS-shift here expands the 14-bit
// code straight back into the 16-bit range (±~32124), so NO extra <<2 -- the
// encode side already dropped the low 2 bits via its >>2 prescale.
static int16_t ulaw2linear(uint8_t u_val) {
    u_val = (uint8_t) ~u_val;
    int t = (((u_val & 0x0F) << 3) + ULAW_BIAS) << ((u_val & 0x70) >> 4);
    t = (u_val & 0x80) ? (ULAW_BIAS - t) : (t - ULAW_BIAS);
    return (int16_t) t;
}

typedef struct { uint32_t wave; double snr; uint32_t frames; double rms; } wave_snr_t;

static int cmp_snr(const void *a, const void *b) {
    double x = ((const wave_snr_t *) a)->snr, y = ((const wave_snr_t *) b)->snr;
    return (x > y) - (x < y);
}

// Distribution summary + worst-10 for a filled per-wave SNR array.
static void report_dist(const char *name, const wave_snr_t *per, uint32_t n) {
    wave_snr_t *s = calloc(n, sizeof(*s));
    uint32_t nf = 0;
    for (uint32_t i = 0; i < n; ++i) if (isfinite(per[i].snr)) s[nf++] = per[i];
    qsort(s, nf, sizeof(*s), cmp_snr);
    double median = nf ? s[nf / 2].snr : 0.0;
    double p10 = nf ? s[nf / 10].snr : 0.0;
    uint32_t b40 = 0, b50 = 0;
    for (uint32_t i = 0; i < nf; ++i) { if (s[i].snr < 40) b40++; if (s[i].snr < 50) b50++; }
    printf("  [%s] per-wave SNR: median %.1f dB, p10 %.1f dB, <40 dB: %u, <50 dB: %u\n",
           name, median, p10, b40, b50);
    printf("        worst 5:");
    for (uint32_t i = 0; i < nf && i < 5; ++i) printf(" w%u=%.1f", s[i].wave, s[i].snr);
    printf("\n");
    free(s);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <gm_bank.bin>\n", argv[0]); return 2; }

    file_blob_t blob;
    if (!read_entire_file(argv[1], &blob)) return 1;
    gm_bank_view_t bank;
    if (!gm_bank_view(blob.data, &bank)) {
        fprintf(stderr, "%s: not a GMWB v%u bank\n", argv[1], GM_BANK_VERSION);
        return 1;
    }

    // Verify the codec is self-consistent (round-trip stable): encode->decode->
    // encode must give the same code, else the SNR below would be meaningless.
    for (int s = -32768; s <= 32767; ++s) {
        uint8_t u = linear2ulaw((int16_t) s);
        if (linear2ulaw(ulaw2linear(u)) != u) {
            fprintf(stderr, "codec not idempotent at %d\n", s);
            return 1;
        }
    }

    uint32_t wave_count = bank.header->wave_count;
    wave_snr_t *per_u = calloc(wave_count, sizeof(*per_u));   // mu-law
    wave_snr_t *per_l = calloc(wave_count, sizeof(*per_l));   // per-wave normalized linear 8-bit

    double tot_sig = 0.0, tot_err_u = 0.0, tot_err_l = 0.0;   // energy-weighted overall
    double peak_err_u = 0.0;
    uint64_t total_frames = 0;
    uint32_t silent = 0;

    for (uint32_t w = 0; w < wave_count; ++w) {
        const gm_wave_t *wv = &bank.waves[w];
        const int16_t *pcm = bank.pcm + wv->pcm_offset;

        // Per-wave peak for the normalized-linear codec (store a per-wave scale,
        // quantize to int8 over [-peak,peak]). peak==0 => silent wave.
        int peak = 1;
        for (uint32_t n = 0; n < wv->frame_count; ++n) {
            int a = pcm[n] < 0 ? -pcm[n] : pcm[n];
            if (a > peak) peak = a;
        }

        double sig = 0.0, err_u = 0.0, err_l = 0.0;
        for (uint32_t n = 0; n < wv->frame_count; ++n) {
            int16_t x = pcm[n];
            sig += (double) x * (double) x;

            // mu-law round trip
            int16_t yu = ulaw2linear(linear2ulaw(x));
            double eu = (double) x - (double) yu;
            err_u += eu * eu;
            double aeu = fabs(eu);
            if (aeu > peak_err_u) peak_err_u = aeu;

            // normalized linear 8-bit round trip: q in [-127,127], reconstruct.
            int q = (int) lround((double) x * 127.0 / peak);
            if (q > 127) q = 127; else if (q < -127) q = -127;
            double yl = (double) q * (double) peak / 127.0;
            double el = (double) x - yl;
            err_l += el * el;
        }
        double rms = wv->frame_count ? sqrt(sig / wv->frame_count) : 0.0;
        per_u[w].wave = per_l[w].wave = w;
        per_u[w].frames = per_l[w].frames = wv->frame_count;
        per_u[w].rms = per_l[w].rms = rms;
        per_u[w].snr = (sig > 0 && err_u > 0) ? 10.0 * log10(sig / err_u) : (err_u == 0 ? INFINITY : -INFINITY);
        per_l[w].snr = (sig > 0 && err_l > 0) ? 10.0 * log10(sig / err_l) : (err_l == 0 ? INFINITY : -INFINITY);
        if (sig == 0.0) silent++;
        tot_sig += sig;
        tot_err_u += err_u;
        tot_err_l += err_l;
        total_frames += wv->frame_count;
    }

    printf("== 16->8->16 reconstruction on %u waves (%llu frames), silent: %u ==\n",
           wave_count, (unsigned long long) total_frames, silent);
    printf("  mu-law            overall SNR %.1f dB (peak err %.0f LSB)\n",
           10.0 * log10(tot_sig / tot_err_u), peak_err_u);
    report_dist("mu-law", per_u, wave_count);
    printf("  norm. linear 8b   overall SNR %.1f dB (per-wave scale field)\n",
           10.0 * log10(tot_sig / tot_err_l));
    report_dist("lin8", per_l, wave_count);

    // Exact flash delta. PCM is one contiguous int16 block; mu-law is 1 byte per
    // sample, so the delta is exactly pcm_samples bytes regardless of how the
    // frames split across (short vs long) waves -- there is no per-wave padding.
    uint32_t pcm_samples = bank.header->pcm_samples;
    double pcm16_mb = pcm_samples * 2.0 / (1024.0 * 1024.0);
    double pcm8_mb  = pcm_samples * 1.0 / (1024.0 * 1024.0);
    double off_pcm  = bank.header->off_pcm;
    double blob16_mb = (off_pcm + pcm_samples * 2.0) / (1024.0 * 1024.0);
    double blob8_mb  = (off_pcm + pcm_samples * 1.0 + 256.0 * 2.0) / (1024.0 * 1024.0); // +256-entry LUT

    printf("== flash delta ==\n");
    printf("  PCM 16-bit: %.2f MB  ->  mu-law 8-bit: %.2f MB  (saved %.2f MB, -%.0f%%)\n",
           pcm16_mb, pcm8_mb, pcm16_mb - pcm8_mb, 100.0 * (pcm16_mb - pcm8_mb) / pcm16_mb);
    printf("  whole blob: %.2f MB -> %.2f MB (incl. 512 B decode LUT), saved %.2f MB\n",
           blob16_mb, blob8_mb, blob16_mb - blob8_mb);
    printf("  (norm. linear 8b is the same 1 byte/sample; +%u B for per-wave scale fields)\n",
           wave_count * 2u);

    free(per_u); free(per_l); free(blob.data);
    return 0;
}
