// Compare the sine-synth `gm_envelopes` table against EG1 volume-envelope
// articulators and program gain stored in a GM.DLS bank.
//
//   dls_env_dump <gm.dls>
//
// This is diagnostic only: DLS envelopes describe sample playback, while the
// sine synth has infinite generated oscillators and a much cheaper ADSR.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dls_parse.c.inl"

#define __not_in_flash(x)
#include "../sine/general-midi.h"

static int attack_to_shift(double atk_s) {
    if (atk_s <= 0.012) return 0;
    if (atk_s <= 0.05)  return 1;
    if (atk_s <= 0.15)  return 2;
    return 3;
}

static int decay_to_shift(double dec_s) {
    if (dec_s <= 0.10) return 2;
    if (dec_s <= 0.25) return 3;
    if (dec_s <= 0.50) return 4;
    if (dec_s <= 1.0)  return 5;
    if (dec_s <= 2.0)  return 6;
    return 7;
}

static int sustain_to_level(double gain) {
    int v = (int) lround(gain * 200.0);
    if (v < 0) v = 0;
    if (v > 240) v = 240;
    return v;
}

static int gain_to_level(double gain) {
    int v = (int) lround(gain * 205.0);   // normalize GM.DLS median near 128
    if (v < 32) v = 32;
    if (v > 255) v = 255;
    return v;
}

static double instrument_gain(const dls_bank_t *bank, const dls_instrument_t *in) {
    double total = 0.0;
    int count = 0;
    for (size_t i = 0; i < in->region_count; ++i) {
        const dls_region_t *rg = &in->regions[i];
        if (rg->wave_index >= bank->wave_count) continue;
        const dls_wave_t *w = &bank->waves[rg->wave_index];
        int32_t attenuation = rg->has_wsmp ? rg->attenuation :
                              w->has_wsmp ? w->attenuation : 0;
        total += dls_attenuation_gain(attenuation);
        count++;
    }
    return count ? total / (double) count : 1.0;
}

static const char *family(int p) {
    static const char *f[16] = {
        "Piano","ChromPerc","Organ","Guitar","Bass","Strings","Ensemble","Brass",
        "Reed","Pipe","SynthLead","SynthPad","SynthFX","Ethnic","Percussive","SFX"};
    return f[(p >> 3) & 15];
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "gm.dls";
    dls_bank_t bank;
    if (!dls_bank_load(path, &bank)) { fprintf(stderr, "cannot load %s\n", path); return 1; }

    printf("prog fam        | DLS:  atk_s  dec_s  sus_g  rel_s gain | from DLS: A D Sus G | current: A D Sus G | name\n");
    int amatch = 0, dmatch = 0, smatch = 0, gmatch = 0, have = 0;
    for (uint32_t p = 0; p < 128; ++p) {
        const dls_instrument_t *in = dls_find_instrument(&bank, 0, p);
        if (!in) { printf("%3u  %-9s | (no instrument)\n", p, family(p)); continue; }
        const dls_articulation_t *a = &in->articulation;
        if (!a->has_eg1 && in->region_count > 0 && in->regions[0].has_articulation)
            a = &in->regions[0].articulation;

        double atk = a->has_attack  ? dls_timecents_to_seconds(a->attack_time)  : 0.0;
        double dec = a->has_decay   ? dls_timecents_to_seconds(a->decay_time)   : 0.0;
        double rel = a->has_release ? dls_timecents_to_seconds(a->release_time) : 0.0;
        double sus = a->has_sustain ? dls_sustain_to_gain(a->sustain_level)     : 1.0;

        double gain = instrument_gain(&bank, in);
        int dA = attack_to_shift(atk), dD = decay_to_shift(dec), dS = sustain_to_level(sus);
        int dG = gain_to_level(gain);
        gm_envelope_t c = gm_envelopes[p];
        have++;
        if (dA == c.attack_shift) amatch++;
        if (dD == c.decay_shift)  dmatch++;
        if (abs(dS - c.sustain_level) <= 30) smatch++;
        if (abs(dG - c.gain) <= 16) gmatch++;

        printf("%3u  %-9s | %6.3f %6.3f  %4.2f  %6.3f %4.2f |        %d %d %3d %3d |        %d %d %3d %3d | %s\n",
               p, family(p), atk, dec, sus, rel, gain, dA, dD, dS, dG,
               c.attack_shift, c.decay_shift, c.sustain_level, c.gain, in->name);
    }
    printf("\nmatch vs current (of %d): attack_shift %d, decay_shift %d, sustain(+-30) %d, gain(+-16) %d\n",
           have, amatch, dmatch, smatch, gmatch);
    dls_bank_free(&bank);
    return 0;
}
