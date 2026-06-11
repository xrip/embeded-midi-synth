// A/B two 16-bit PCM WAVs and report reconstruction SNR.
//
//   wav_ab <reference.wav> <test.wav>
//
// Used to validate the µ-law PCM port: render the same MIDI with the int16
// engine (master) and the µ-law engine (this branch), then measure how far the
// µ-law render sits below the reference. The expected floor is the codec's
// ~38 dB per-sample SNR, softened a little by interpolation and the master
// gain; a much lower number means a real bug, not just quantization.
//
// Minimal RIFF parser: expects canonical PCM (fmt tag 1), 16-bit, same channel
// count / length in both files (it compares the shorter common span).
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int16_t *s; uint32_t n; uint16_t ch; uint32_t rate; } wav_t;

static uint32_t rd_u32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24); }
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t) (p[0] | (p[1] << 8)); }

static int load_wav(const char *path, wav_t *w) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return 0; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t) sz);
    if (fread(buf, 1, (size_t) sz, f) != (size_t) sz) { fclose(f); free(buf); return 0; }
    fclose(f);
    if (sz < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a WAVE file\n", path); free(buf); return 0;
    }
    memset(w, 0, sizeof(*w));
    size_t off = 12;
    const uint8_t *data = NULL; uint32_t data_bytes = 0; uint16_t bits = 0;
    while (off + 8 <= (size_t) sz) {
        const uint8_t *ck = buf + off;
        uint32_t len = rd_u32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            w->ch = rd_u16(ck + 10);
            w->rate = rd_u32(ck + 12);
            bits = rd_u16(ck + 22);
        } else if (!memcmp(ck, "data", 4)) {
            data = ck + 8;
            data_bytes = len;
        }
        off += 8 + len + (len & 1);
    }
    if (!data || bits != 16) { fprintf(stderr, "%s: need 16-bit data chunk\n", path); free(buf); return 0; }
    w->n = data_bytes / 2;                       // total int16 samples (interleaved)
    w->s = malloc(w->n * sizeof(int16_t));
    for (uint32_t i = 0; i < w->n; ++i) w->s[i] = (int16_t) rd_u16(data + i * 2);
    free(buf);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <reference.wav> <test.wav>\n", argv[0]); return 2; }
    wav_t a, b;
    if (!load_wav(argv[1], &a) || !load_wav(argv[2], &b)) return 1;
    if (a.ch != b.ch) fprintf(stderr, "warning: channel count differs (%u vs %u)\n", a.ch, b.ch);

    uint32_t n = a.n < b.n ? a.n : b.n;
    if (a.n != b.n) fprintf(stderr, "warning: length differs (%u vs %u samples), comparing first %u\n", a.n, b.n, n);

    double sig = 0, err = 0, peak_err = 0;
    int32_t amax = 0;
    uint32_t exact = 0;
    for (uint32_t i = 0; i < n; ++i) {
        double x = a.s[i], y = b.s[i], e = x - y;
        sig += x * x;
        err += e * e;
        if (fabs(e) > peak_err) peak_err = fabs(e);
        int32_t ax = a.s[i] < 0 ? -a.s[i] : a.s[i];
        if (ax > amax) amax = ax;
        if (a.s[i] == b.s[i]) exact++;
    }
    double snr = (err > 0) ? 10.0 * log10(sig / err) : INFINITY;
    printf("compared %u samples (%u ch @ %u Hz)\n", n, a.ch, a.rate);
    printf("  reference peak: %d / 32768\n", amax);
    printf("  SNR: %.1f dB   peak abs error: %.0f LSB   bit-exact samples: %.1f%%\n",
           snr, peak_err, 100.0 * (double) exact / (double) n);

    free(a.s); free(b.s);
    return 0;
}
