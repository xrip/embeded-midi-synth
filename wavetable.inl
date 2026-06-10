// Fixed-point GM wavetable voice engine.
//
// Plays the packed flash soundbank (gm_bank.h) with no floating point on the
// hot path: interpolated sample playback, per-region pitch/loops, a single
// EG1-style amplitude envelope (Q16), and stereo panning. Designed to drop into
// the emulator's general-midi.c.inl in place of the synthesized voices and to
// be compiled on the host by wt_render.c for A/B validation against the
// double-precision golden reference (gm_dls_player.c).
//
// The includer must provide before including:
//   * gm_bank.h
//   * INLINE            (e.g. `static inline`)
//   * SOUND_FREQUENCY   (output sample rate; must equal the bank's output_rate)
// and must define the global g_bank and call wt_set_bank() once.
#pragma once

#include <math.h>   // init-time LUT build only (never on the audio path)
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef WT_MAX_VOICES
#define WT_MAX_VOICES 32
#endif
#define WT_MIDI_CHANNELS 16
#define WT_PITCH_BEND_RANGE_SEMITONES 2

#ifndef WT_MIDI_COMMAND_T_DEFINED
#define WT_MIDI_COMMAND_T_DEFINED
typedef struct __attribute__((packed)) {
    uint8_t command;
    uint8_t note;
    uint8_t velocity;
    uint8_t other;
} midi_command_t;
#endif

enum { WT_ATTACK, WT_DECAY, WT_SUSTAIN, WT_RELEASE };

typedef struct {
    uint8_t  active;
    uint8_t  channel;
    uint8_t  note;
    uint8_t  velocity;
    uint8_t  percussion;
    uint8_t  sustained;
    uint8_t  key_group;
    uint8_t  amp_stage;

    const int16_t *pcm;       // wave PCM base (g_bank.pcm + wave->pcm_offset)
    uint32_t frame_count;
    uint32_t loop_start;
    uint32_t loop_end;        // loop_start + loop_length
    uint8_t  looped;

    uint32_t frame_pos;       // integer frame index
    uint32_t frac;            // Q16 fractional position
    uint32_t step_q16;        // frames per output sample, Q16.16
    uint32_t base_step_q16;   // pre-pitch wave step (for re-pitch on bend)
    uint8_t  root;            // resolved root key
    int16_t  fine_cents;      // region fine tune
    int32_t  static_cents;    // note/fine/bend pitch offset (vibrato added on top)

    uint8_t  has_lfo;
    uint32_t lfo_phase;       // current phase (full turn = 2^32)
    uint32_t lfo_phase_inc;
    uint32_t lfo_delay;       // samples before LFO starts
    int32_t  lfo_depth_q8;    // cents Q8
    int32_t  lfo_mod_depth_q8;// cents Q8 (scaled by mod wheel)
    int32_t  trem_depth_q8;   // tremolo depth, log2-amplitude cents Q8 (0 = none)
    uint32_t samples_played;

    uint8_t  has_eg2;         // EG2 pitch envelope active
    uint8_t  eg2_stage;
    int32_t  eg2_q16;         // current EG2 level (Q16, 0..65536)
    int32_t  eg2_sustain_q16;
    int32_t  eg2_attack_step_q16;
    int32_t  eg2_decay_coef_q16;
    int32_t  eg2_release_coef_q16;
    int32_t  eg2_pitch_cents; // pitch offset at full EG2 level

    int32_t  env_q16;         // current envelope amplitude (Q16, 0..65536)
    int32_t  sustain_q16;
    int32_t  attack_step_q16;
    int32_t  decay_coef_q16;
    int32_t  release_coef_q16;

    int32_t  region_gain_q16; // baked region attenuation gain (Q16)
    int32_t  amp_q16;         // velocity * region gain * channel vol/expr (Q16)
    int16_t  pan_l_q15;
    int16_t  pan_r_q15;
    uint32_t age;
} wt_voice_t;

typedef struct {
    uint8_t program;
    uint8_t volume;
    uint8_t expression;
    uint8_t pan;
    uint8_t bank_msb;
    uint8_t bank_lsb;
    uint8_t sustain;
    uint8_t modulation;       // CC1 mod wheel, 0..127
    uint8_t rpn_msb;          // 127 = none selected
    uint8_t rpn_lsb;
    int     pitch_bend;       // 0..16383, center 8192
    int     bend_range_cents; // pitch bend range, in cents (default 200 = ±2 st)
} wt_channel_t;

static gm_bank_view_t g_bank;
static wt_voice_t  g_voices[WT_MAX_VOICES];
static wt_channel_t g_channels[WT_MIDI_CHANNELS];
static uint32_t g_next_age;

// 2^(cents/1200) in Q16 for one octave of cents; octaves applied by shifting.
#define WT_SIN_BITS 10
#define WT_SIN_SIZE (1 << WT_SIN_BITS)
static uint32_t g_pow2_cents_q16[1200];
static int16_t  g_pan_l_q15[128];
static int16_t  g_pan_r_q15[128];
static int16_t  g_sin_q15[WT_SIN_SIZE];
static uint32_t g_cc_gain_q16[128];   // GM concave curve for velocity/CC7/CC11

static void wt_build_luts(void) {
    for (int c = 0; c < 1200; ++c) {
        g_pow2_cents_q16[c] = (uint32_t) lround(pow(2.0, (double) c / 1200.0) * 65536.0);
    }
    for (int p = 0; p < 128; ++p) {
        double a = (double) p / 127.0 * (M_PI * 0.5);
        g_pan_l_q15[p] = (int16_t) lround(cos(a) * 32767.0);
        g_pan_r_q15[p] = (int16_t) lround(sin(a) * 32767.0);
    }
    for (int i = 0; i < WT_SIN_SIZE; ++i) {
        g_sin_q15[i] = (int16_t) lround(sin(2.0 * M_PI * (double) i / (double) WT_SIN_SIZE) * 32767.0);
    }
    for (int v = 0; v < 128; ++v) {
        // GM/DLS concave gain curve: 40*log10(v/127) dB == amplitude (v/127)^2.
        g_cc_gain_q16[v] = (uint32_t) lround((double) (v * v) / (127.0 * 127.0) * 65536.0);
    }
}

static void wt_engine_reset(void) {
    memset(g_voices, 0, sizeof(g_voices));
    g_next_age = 0;
    for (int i = 0; i < WT_MIDI_CHANNELS; ++i) {
        g_channels[i].program = 0;
        g_channels[i].volume = 100;
        g_channels[i].expression = 127;
        g_channels[i].pan = 64;
        g_channels[i].bank_msb = 0;
        g_channels[i].bank_lsb = 0;
        g_channels[i].sustain = 0;
        g_channels[i].modulation = 0;
        g_channels[i].rpn_msb = 127;
        g_channels[i].rpn_lsb = 127;
        g_channels[i].pitch_bend = 8192;
        g_channels[i].bend_range_cents = WT_PITCH_BEND_RANGE_SEMITONES * 100;
    }
}

static void wt_set_bank(const void *blob) {
    gm_bank_view(blob, &g_bank);
    wt_build_luts();
    wt_engine_reset();
}

// ---- bank lookup (integer ports of dls_find_instrument / dls_find_region) ----

static const gm_instrument_t *wt_find_instrument(uint32_t dls_bank, uint32_t program) {
    const gm_instrument_t *fb_program = NULL;
    const gm_instrument_t *fb_piano = NULL;
    uint32_t n = g_bank.header->instrument_count;
    for (uint32_t i = 0; i < n; ++i) {
        const gm_instrument_t *ins = &g_bank.instruments[i];
        if (ins->bank == dls_bank && ins->program == program) return ins;
        if (ins->bank == 0 && ins->program == program) fb_program = ins;
        if (ins->bank == 0 && ins->program == 0) fb_piano = ins;
    }
    if (dls_bank & DLS_DRUM_BANK) {
        for (uint32_t i = 0; i < n; ++i) {
            const gm_instrument_t *ins = &g_bank.instruments[i];
            if (ins->bank == DLS_DRUM_BANK && ins->program == 0) return ins;
        }
    }
    return fb_program ? fb_program : fb_piano;
}

// DLS region selection: first region whose key AND velocity ranges match.
// No "nearest" fallback — per the spec a note with no matching region simply
// does not sound (the legacy reference invented a nearest-key fallback, which
// could pick a wrong velocity layer or a wildly re-pitched neighbor sample).
static const gm_region_t *wt_find_region(const gm_instrument_t *ins, uint8_t note, uint8_t velocity) {
    for (uint32_t i = 0; i < ins->region_count; ++i) {
        const gm_region_t *rg = &g_bank.regions[ins->region_first + i];
        if (note >= rg->key_low && note <= rg->key_high &&
            velocity >= rg->vel_low && velocity <= rg->vel_high) {
            return rg;
        }
    }
    return NULL;
}

// ---- voice helpers -----------------------------------------------------------

// Pitch factor for a cents offset given in Q8 (cents*256), with sub-cent
// precision via linear interpolation of the per-cent 2^(c/1200) LUT. Sub-cent
// accuracy keeps long sustained / vibrato notes from drifting audibly.
#define WT_CENTS_PER_OCT_Q8 (1200 * 256)
static uint32_t wt_pitch_step(uint32_t base_step_q16, int total_cents_q8) {
    int oct = total_cents_q8 >= 0
                  ? total_cents_q8 / WT_CENTS_PER_OCT_Q8
                  : -(((-total_cents_q8) + WT_CENTS_PER_OCT_Q8 - 1) / WT_CENTS_PER_OCT_Q8);
    int rem = total_cents_q8 - oct * WT_CENTS_PER_OCT_Q8; // 0..307199
    int idx = rem >> 8;                                   // 0..1199
    int frac = rem & 255;
    uint32_t f0 = g_pow2_cents_q16[idx];
    uint32_t f1 = idx < 1199 ? g_pow2_cents_q16[idx + 1] : (g_pow2_cents_q16[0] << 1); // 2^1
    uint32_t f = f0 + (uint32_t) (((int64_t) (f1 - f0) * frac) >> 8);
    if (oct > 8) oct = 8;
    if (oct < -8) oct = -8;
    if (oct >= 0) f <<= oct; else f >>= (-oct);
    uint64_t step = ((uint64_t) base_step_q16 * f) >> 16;
    if (step > 0xFFFFFFFFu) step = 0xFFFFFFFFu;
    return (uint32_t) step;
}

// Pitch-bend contribution in Q8 cents. range_cents is integer cents.
static int wt_channel_bend_cents_q8(const wt_channel_t *ch) {
    // (bend-8192)/8192 * range, in Q8: == (bend-8192)*range_cents*256/8192.
    return (ch->pitch_bend - 8192) * ch->bend_range_cents / 32;
}

static int wt_rpn_is_bend_range(const wt_channel_t *ch) {
    return ch->rpn_msb == 0 && ch->rpn_lsb == 0;
}

// amp = curve(velocity) * region_gain * curve(volume) * curve(expression), Q16.
// Velocity and CC7/CC11 use the GM/DLS concave curve (amplitude (x/127)^2), not
// a linear ramp. Control-rate path only; the int64 products never hit the
// per-sample loop.
static void wt_update_amp(wt_voice_t *v) {
    const wt_channel_t *ch = &g_channels[v->channel];
    int64_t amp = g_cc_gain_q16[v->velocity];
    amp = (amp * v->region_gain_q16) >> 16;
    amp = (amp * g_cc_gain_q16[ch->volume]) >> 16;
    amp = (amp * g_cc_gain_q16[ch->expression]) >> 16;
    // Cap the composite amp just under unity (Q16). The census shows 0 of 1498
    // regions boost (all wsmp gains are cuts), so this never attenuates real
    // content; it guarantees env*amp stays < 2^32 and s*gain stays < 2^31, so
    // both per-sample products are single 32x32->32 MULs (Cortex-M0 has no
    // UMULL; an int64 product would call __aeabi_lmul on the hot path).
    if (amp > GM_ONE_Q16 - 1) amp = GM_ONE_Q16 - 1;
    v->amp_q16 = (int32_t) amp;
}

static void wt_update_pitch(wt_voice_t *v) {
    // static pitch offset in Q8 cents; vibrato is added on top in the render loop.
    v->static_cents = ((v->note - v->root) * 100 + v->fine_cents) * 256
                      + wt_channel_bend_cents_q8(&g_channels[v->channel]);
    v->step_q16 = wt_pitch_step(v->base_step_q16, v->static_cents);
}

static wt_voice_t *wt_alloc_voice(void) {
    for (int i = 0; i < WT_MAX_VOICES; ++i) {
        if (!g_voices[i].active) return &g_voices[i];
    }
    // Steal the quietest voice.
    wt_voice_t *q = &g_voices[0];
    for (int i = 1; i < WT_MAX_VOICES; ++i) {
        if (g_voices[i].env_q16 < q->env_q16 ||
            (g_voices[i].env_q16 == q->env_q16 && g_voices[i].age < q->age)) {
            q = &g_voices[i];
        }
    }
    return q;
}

static void wt_release_voice(wt_voice_t *v) {
    if (!v->active) return;
    v->sustained = 0;
    v->amp_stage = WT_RELEASE;
    if (v->has_eg2) v->eg2_stage = WT_RELEASE;
}

static void wt_note_off(uint8_t channel, uint8_t note) {
    for (int i = 0; i < WT_MAX_VOICES; ++i) {
        wt_voice_t *v = &g_voices[i];
        if (!v->active || v->channel != channel || v->note != note) continue;
        // One-shot drums play to the end (ignore note-off); looped/sustained
        // drums and all melodic voices release per their DLS envelope.
        if (v->percussion && !v->looped) continue;
        if (g_channels[channel].sustain) v->sustained = 1;
        else wt_release_voice(v);
    }
}

static void wt_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    wt_channel_t *ch = &g_channels[channel];
    uint32_t bank_number = ((uint32_t) ch->bank_msb << 8) | ch->bank_lsb;
    uint32_t dls_bank = channel == 9 ? (DLS_DRUM_BANK | bank_number) : bank_number;

    const gm_instrument_t *ins = wt_find_instrument(dls_bank, ch->program);
    if (!ins) return;
    const gm_region_t *rg = wt_find_region(ins, note, velocity);
    if (!rg || rg->wave_index >= g_bank.header->wave_count) return;
    const gm_wave_t *w = &g_bank.waves[rg->wave_index];
    if (w->frame_count < 2) return;

    // Voice stealing: retrigger same note; drum exclusive key groups.
    for (int i = 0; i < WT_MAX_VOICES; ++i) {
        wt_voice_t *v = &g_voices[i];
        if (!v->active || v->channel != channel) continue;
        if (v->note == note && !v->percussion) v->active = 0;
        else if (channel == 9 && rg->key_group != 0 && v->key_group == rg->key_group) v->active = 0;
    }

    wt_voice_t *v = wt_alloc_voice();
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->percussion = channel == 9;
    v->channel = channel;
    v->note = note;
    v->velocity = velocity;
    v->key_group = rg->key_group;
    v->age = g_next_age++;

    v->pcm = g_bank.pcm + w->pcm_offset;
    v->frame_count = w->frame_count;
    if (rg->flags & GM_RGN_LOOPED) {  // honor DLS loop (EG1 decay governs ring time)
        v->looped = 1;
        v->loop_start = rg->loop_start;
        v->loop_end = rg->loop_start + rg->loop_length;
    }
    v->base_step_q16 = w->base_step_q16;
    v->root = (rg->flags & GM_RGN_ROOT_FROM_NOTE) ? note : rg->root_key;
    v->fine_cents = rg->fine_cents;
    v->region_gain_q16 = rg->gain_q16;
    if (rg->flags & GM_RGN_HAS_LFO) {
        v->has_lfo = 1;
        v->lfo_phase = 0;
        v->lfo_phase_inc = rg->lfo_phase_inc;
        v->lfo_delay = rg->lfo_delay;
        v->lfo_depth_q8 = rg->lfo_depth_q8;
        v->lfo_mod_depth_q8 = rg->lfo_mod_depth_q8;
        v->trem_depth_q8 = rg->lfo_gain_depth_q8;
    }
    if (rg->flags & GM_RGN_HAS_EG2) {
        v->has_eg2 = 1;
        v->eg2_sustain_q16 = (int32_t) rg->eg2_sustain_q16;
        v->eg2_attack_step_q16 = (int32_t) rg->eg2_attack_step_q16;
        v->eg2_decay_coef_q16 = (int32_t) rg->eg2_decay_coef_q16;
        v->eg2_release_coef_q16 = (int32_t) rg->eg2_release_coef_q16;
        v->eg2_pitch_cents = rg->eg2_pitch_cents;
        if (v->eg2_attack_step_q16 >= GM_ONE_Q16) {
            v->eg2_q16 = GM_ONE_Q16;
            v->eg2_stage = WT_DECAY;
        } else {
            v->eg2_q16 = 0;
            v->eg2_stage = WT_ATTACK;
        }
    }
    wt_update_pitch(v);
    wt_update_amp(v);

    // Channel pan (CC10) plus the region's DLS art1 pan offset (drum kits pan
    // toms/cymbals across the stereo field this way).
    int pan = (int) ch->pan + rg->pan;
    if (pan < 0) pan = 0; else if (pan > 127) pan = 127;
    v->pan_l_q15 = g_pan_l_q15[pan];
    v->pan_r_q15 = g_pan_r_q15[pan];

    v->sustain_q16 = rg->sustain_q16;
    v->attack_step_q16 = rg->attack_step_q16;
    v->decay_coef_q16 = rg->decay_coef_q16;
    v->release_coef_q16 = rg->release_coef_q16;
    if (rg->attack_step_q16 >= GM_ONE_Q16) {
        v->env_q16 = GM_ONE_Q16;
        v->amp_stage = WT_DECAY;
    } else {
        v->env_q16 = 0;
        v->amp_stage = WT_ATTACK;
    }
}

// Recompute pitch for all sounding voices on a channel (pitch bend).
static void wt_channel_repitch(uint8_t channel) {
    for (int i = 0; i < WT_MAX_VOICES; ++i) {
        wt_voice_t *v = &g_voices[i];
        if (v->active && v->channel == channel) wt_update_pitch(v);
    }
}

// Recompute amplitude for all sounding voices on a channel (volume/expression).
static void wt_channel_reamp(uint8_t channel) {
    for (int i = 0; i < WT_MAX_VOICES; ++i) {
        wt_voice_t *v = &g_voices[i];
        if (v->active && v->channel == channel) wt_update_amp(v);
    }
}

static void parse_midi(const midi_command_t *m) {
    uint8_t channel = m->command & 0x0f;
    wt_channel_t *ch = &g_channels[channel];
    switch (m->command >> 4) {
        case 0x9:
            // Mask to 7 bits at the boundary: velocity indexes g_cc_gain_q16,
            // and on-device bytes arrive from a raw MPU-401 stream.
            if (m->velocity & 0x7f) wt_note_on(channel, m->note & 0x7f, m->velocity & 0x7f);
            else wt_note_off(channel, m->note & 0x7f);
            break;
        case 0x8:
            wt_note_off(channel, m->note & 0x7f);
            break;
        case 0xb:
            switch (m->note) {
                case 0x00: ch->bank_msb = m->velocity & 0x7f; break;
                case 0x01: ch->modulation = m->velocity & 0x7f; break;
                case 0x20: ch->bank_lsb = m->velocity & 0x7f; break;
                case 0x07: ch->volume = m->velocity & 0x7f; wt_channel_reamp(channel); break;
                case 0x0a: ch->pan = m->velocity & 0x7f; break;
                case 0x0b: ch->expression = m->velocity & 0x7f; wt_channel_reamp(channel); break;
                case 0x65: ch->rpn_msb = m->velocity & 0x7f; break; // RPN MSB
                case 0x64: ch->rpn_lsb = m->velocity & 0x7f; break; // RPN LSB
                case 0x06: // data entry MSB -> whole semitones of bend range
                    if (wt_rpn_is_bend_range(ch)) {
                        ch->bend_range_cents = (m->velocity & 0x7f) * 100 + (ch->bend_range_cents % 100);
                        wt_channel_repitch(channel);
                    }
                    break;
                case 0x26: // data entry LSB -> cents of bend range
                    if (wt_rpn_is_bend_range(ch)) {
                        int cents = m->velocity & 0x7f; if (cents > 99) cents = 99;
                        ch->bend_range_cents = (ch->bend_range_cents / 100) * 100 + cents;
                        wt_channel_repitch(channel);
                    }
                    break;
                case 0x60: // data increment (whole semitone)
                    if (wt_rpn_is_bend_range(ch)) { ch->bend_range_cents += 100; wt_channel_repitch(channel); }
                    break;
                case 0x61: // data decrement
                    if (wt_rpn_is_bend_range(ch) && ch->bend_range_cents >= 100) {
                        ch->bend_range_cents -= 100; wt_channel_repitch(channel);
                    }
                    break;
                case 0x40:
                    if (m->velocity >= 64) {
                        ch->sustain = 1;
                    } else {
                        ch->sustain = 0;
                        for (int i = 0; i < WT_MAX_VOICES; ++i) {
                            wt_voice_t *v = &g_voices[i];
                            if (v->active && v->channel == channel && v->sustained) wt_release_voice(v);
                        }
                    }
                    break;
                case 0x78:
                    for (int i = 0; i < WT_MAX_VOICES; ++i)
                        if (g_voices[i].channel == channel) g_voices[i].active = 0;
                    break;
                case 0x7b:
                    for (int i = 0; i < WT_MAX_VOICES; ++i) {
                        wt_voice_t *v = &g_voices[i];
                        if (v->active && v->channel == channel) {
                            if (v->percussion) v->active = 0; else wt_release_voice(v);
                        }
                    }
                    break;
                case 0x79:
                    ch->volume = 100; ch->expression = 127; ch->pan = 64;
                    ch->bank_msb = 0; ch->bank_lsb = 0; ch->pitch_bend = 8192; ch->sustain = 0;
                    ch->modulation = 0;
                    ch->rpn_msb = 127; ch->rpn_lsb = 127;
                    ch->bend_range_cents = WT_PITCH_BEND_RANGE_SEMITONES * 100;
                    wt_channel_reamp(channel);
                    wt_channel_repitch(channel);
                    break;
                default: break;
            }
            break;
        case 0xc:
            ch->program = m->note & 0x7f;
            break;
        case 0xe:
            ch->pitch_bend = ((int) m->velocity << 7) | (int) m->note;
            wt_channel_repitch(channel);
            break;
        default: break;
    }
}

// ---- audio path --------------------------------------------------------------

INLINE void wt_advance_env(wt_voice_t *v) {
    switch (v->amp_stage) {
        case WT_ATTACK:
            v->env_q16 += v->attack_step_q16;
            if (v->env_q16 >= GM_ONE_Q16) {
                v->env_q16 = GM_ONE_Q16;
                v->amp_stage = WT_DECAY;
            }
            break;
        case WT_DECAY:
            // DLS decay: linear-in-dB ramp (constant per-sample multiplier)
            // that clamps at the sustain level. env<=65536 and coef<65536, so
            // the product is < 2^32 and fits a single unsigned 32x32->32 MUL.
            v->env_q16 = (int32_t) (((uint32_t) v->env_q16 * (uint32_t) v->decay_coef_q16) >> 16);
            if (v->env_q16 <= v->sustain_q16) {
                v->env_q16 = v->sustain_q16;
                v->amp_stage = WT_SUSTAIN;
            }
            break;
        case WT_SUSTAIN:
            // A voice that decayed to (near) silence is done; free it so looped
            // samples with zero sustain don't ring forever.
            if (v->sustain_q16 < 8) v->active = 0;
            break;
        case WT_RELEASE:
            v->env_q16 = (int32_t) (((uint32_t) v->env_q16 * (uint32_t) v->release_coef_q16) >> 16);
            if (v->env_q16 < 6) v->active = 0;
            break;
        default: break;
    }
}

// EG2 (pitch envelope): same ADSR shapes as the amplitude EG1, but it never
// kills the voice — EG1 owns voice lifetime.
INLINE void wt_advance_eg2(wt_voice_t *v) {
    switch (v->eg2_stage) {
        case WT_ATTACK:
            v->eg2_q16 += v->eg2_attack_step_q16;
            if (v->eg2_q16 >= GM_ONE_Q16) {
                v->eg2_q16 = GM_ONE_Q16;
                v->eg2_stage = WT_DECAY;
            }
            break;
        case WT_DECAY:
            // Same linear-in-dB ramp as EG1, clamped at the EG2 sustain level.
            v->eg2_q16 = (int32_t) (((uint32_t) v->eg2_q16 * (uint32_t) v->eg2_decay_coef_q16) >> 16);
            if (v->eg2_q16 <= v->eg2_sustain_q16) {
                v->eg2_q16 = v->eg2_sustain_q16;
                v->eg2_stage = WT_SUSTAIN;
            }
            break;
        case WT_SUSTAIN:
            break;
        case WT_RELEASE:
            v->eg2_q16 = (int32_t) (((uint32_t) v->eg2_q16 * (uint32_t) v->eg2_release_coef_q16) >> 16);
            break;
        default: break;
    }
}

INLINE void midi_sample_stereo(int16_t *out_l, int16_t *out_r) {
    int32_t l = 0, r = 0;

    for (int i = 0; i < WT_MAX_VOICES; ++i) {
        wt_voice_t *v = &g_voices[i];
        if (!v->active) continue;

        wt_advance_env(v);
        if (!v->active) continue;

        // Per-sample modulation: DLS LFO (pitch vibrato + gain tremolo) and the
        // EG2 pitch envelope. Recompute the step only while something moves.
        int32_t amp_q16 = v->amp_q16;
        int32_t dyn_cents_q8 = 0;
        int repitch = 0;
        if (v->has_lfo && v->samples_played >= v->lfo_delay) {
            int32_t s = g_sin_q15[v->lfo_phase >> (32 - WT_SIN_BITS)];
            // mod-depth (<=185600) * modulation (<=127) fits int32.
            int32_t depth_q8 = v->lfo_depth_q8 +
                               (v->lfo_mod_depth_q8 * (int32_t) g_channels[v->channel].modulation) / 127;
            if (depth_q8) {
                // s (<=32767) * depth_q8 (<=185600 for the deep SFX vibrato)
                // can exceed 2^31, so this product stays 64-bit.
                dyn_cents_q8 += (int32_t) (((int64_t) s * depth_q8) >> 15); // Q15 * Q8 -> Q8 cents
                repitch = 1;
            }
            if (v->trem_depth_q8) {
                // Tremolo: gain factor 2^(c/1200) via the pitch LUT applied to amp.
                // s*depth (<=32767*4352) fits int32.
                int32_t gain_cents_q8 = (s * v->trem_depth_q8) >> 15;
                amp_q16 = (int32_t) wt_pitch_step((uint32_t) amp_q16, gain_cents_q8);
                if (amp_q16 > GM_ONE_Q16 - 1) amp_q16 = GM_ONE_Q16 - 1; // keep gain*s in int32
            }
            v->lfo_phase += v->lfo_phase_inc;
        }
        if (v->has_eg2) {
            wt_advance_eg2(v);
            // level Q16 (<=65536) * cents (<=1200) -> Q8 cents, fits int32.
            dyn_cents_q8 += (v->eg2_q16 * v->eg2_pitch_cents) >> 8;
            repitch = 1;
        }
        if (repitch) v->step_q16 = wt_pitch_step(v->base_step_q16, v->static_cents + dyn_cents_q8);
        v->samples_played++;

        // Linear interpolation. frac is reduced to Q15 so the 32x32->32 product
        // never overflows: |s1-s0| <= 65535 and frac <= 32767 keep it under
        // INT32_MAX. With full Q16 frac, loud transients (|s1-s0| > 32768, e.g.
        // saw-edge basses, cymbal noise) overflowed into full-scale spikes.
        uint32_t i0 = v->frame_pos;
        int32_t s0 = v->pcm[i0];
        uint32_t i1 = i0 + 1;
        int32_t s1 = (i1 < v->frame_count) ? v->pcm[i1] : s0;
        int32_t s = s0 + (((s1 - s0) * (int32_t) (v->frac >> 1)) >> 15);

        // env<=65536, amp<=65535 -> product < 2^32 (single unsigned MUL); the
        // resulting gain<=65535, so s (<=32767) * gain stays inside int32.
        int32_t gain = (int32_t) (((uint32_t) v->env_q16 * (uint32_t) amp_q16) >> 16); // Q16
        int32_t val = (s * gain) >> 16;

        l += (val * v->pan_l_q15) >> 15;
        r += (val * v->pan_r_q15) >> 15;

        // Advance position.
        uint32_t acc = v->frac + (v->step_q16 & 0xFFFF);
        v->frame_pos += (v->step_q16 >> 16) + (acc >> 16);
        v->frac = acc & 0xFFFF;

        if (v->looped) {
            while (v->frame_pos >= v->loop_end) v->frame_pos -= (v->loop_end - v->loop_start);
        } else if (v->frame_pos + 1 >= v->frame_count) {
            v->active = 0;
        }
    }

    // Master gain ~0.45 (29491/65536). Clamp the accumulator BEFORE the gain
    // multiply so it stays a single 32x32->32 mul (no __aeabi_lmul on M0):
    // 72818 * 29491 = 2147475638 < INT32_MAX, and >>16 spans exactly the full
    // int16 range, so no post-clamp is needed.
    if (l > 72818) l = 72818; else if (l < -72818) l = -72818;
    if (r > 72818) r = 72818; else if (r < -72818) r = -72818;
    *out_l = (int16_t) ((l * 29491) >> 16);
    *out_r = (int16_t) ((r * 29491) >> 16);
}

INLINE int wt_has_active_voices(void) {
    for (int i = 0; i < WT_MAX_VOICES; ++i)
        if (g_voices[i].active) return 1;
    return 0;
}
