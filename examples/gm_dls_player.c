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

#define MAX_VOICES 96
#define MIDI_CHANNELS 16
#define DEFAULT_SAMPLE_RATE 44100u
#define DEFAULT_PITCH_BEND_RANGE_SEMITONES 2.0

#include "../dls_parse.inl"

#include "../gm_env_table.h"

#include "../smf_parse.inl"

typedef struct {
    uint8_t program;
    uint8_t volume;
    uint8_t expression;
    uint8_t pan;
    uint8_t modulation;
    uint8_t reverb;
    uint8_t chorus;
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
    bool use_dls_env;
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
    dls_articulation_t articulation;
    double position;
    double step;
    double attenuation_gain;
    double amp_env;
    double amp_sustain;
    double amp_attack_step;
    double amp_decay_coef;
    double amp_release_coef;
    double lfo_phase;
    double lfo_delay_samples;
    double lfo_frequency_hz;
    double lfo_pitch_cents;
    double mod_lfo_pitch_cents;
    bool filter_enabled;
    double filter_f;
    double filter_damp;
    double filter_low;
    double filter_band;
    double percussion_gain;
    double percussion_decay;
    uint8_t amp_stage;
} synth_voice_t;

typedef struct {
    const dls_bank_t *bank;
    uint32_t sample_rate;
    midi_channel_t channels[MIDI_CHANNELS];
    synth_voice_t voices[MAX_VOICES];
    uint64_t next_age;
    double *reverb_l;
    double *reverb_r;
    size_t reverb_len;
    size_t reverb_pos;
    double *chorus_l;
    double *chorus_r;
    size_t chorus_len;
    size_t chorus_pos;
    double chorus_phase;
} synth_t;

typedef struct {
    FILE *file;
    uint32_t sample_rate;
    uint32_t frames_written;
} wav_writer_t;

enum {
    AMP_ATTACK,
    AMP_DECAY,
    AMP_SUSTAIN,
    AMP_RELEASE,
};

static double clamp_double(double value, double min_value, double max_value) {
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
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
        synth->channels[i].modulation = 0;
        synth->channels[i].reverb = 0;
        synth->channels[i].chorus = 0;
        synth->channels[i].pitch_bend = 8192;
        synth->channels[i].pitch_bend_range = DEFAULT_PITCH_BEND_RANGE_SEMITONES;
        synth->channels[i].rpn_msb = 127;
        synth->channels[i].rpn_lsb = 127;
    }

    synth->reverb_len = sample_rate / 2u;
    if (synth->reverb_len < 1) synth->reverb_len = 1;
    synth->chorus_len = sample_rate / 20u;
    if (synth->chorus_len < 8) synth->chorus_len = 8;
    synth->reverb_l = calloc(synth->reverb_len, sizeof(*synth->reverb_l));
    synth->reverb_r = calloc(synth->reverb_len, sizeof(*synth->reverb_r));
    synth->chorus_l = calloc(synth->chorus_len, sizeof(*synth->chorus_l));
    synth->chorus_r = calloc(synth->chorus_len, sizeof(*synth->chorus_r));
}

static void synth_free(synth_t *synth) {
    free(synth->reverb_l);
    free(synth->reverb_r);
    free(synth->chorus_l);
    free(synth->chorus_r);
    synth->reverb_l = NULL;
    synth->reverb_r = NULL;
    synth->chorus_l = NULL;
    synth->chorus_r = NULL;
    synth->reverb_len = 0;
    synth->chorus_len = 0;
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
    if (voice->use_dls_env) {
        voice->amp_stage = AMP_RELEASE;
        return;
    }
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

static void synth_init_dls_envelope(synth_voice_t *voice, const dls_articulation_t *articulation,
                                    uint32_t sample_rate) {
    voice->use_dls_env = articulation->has_eg1;
    if (!voice->use_dls_env) return;

    voice->articulation = *articulation;
    double attack_seconds = articulation->has_attack ? dls_timecents_to_seconds(articulation->attack_time) : 0.0;
    double decay_seconds = articulation->has_decay ? dls_timecents_to_seconds(articulation->decay_time) : 0.0;
    double release_seconds = articulation->has_release ? dls_timecents_to_seconds(articulation->release_time) : 0.05;
    voice->amp_sustain = articulation->has_sustain ? dls_sustain_to_gain(articulation->sustain_level) : 1.0;

    voice->amp_env = attack_seconds <= 0.0 ? 1.0 : 0.0;
    voice->amp_stage = attack_seconds <= 0.0 ? AMP_DECAY : AMP_ATTACK;
    voice->amp_attack_step = attack_seconds <= 0.0 ? 1.0 : 1.0 / (attack_seconds * (double) sample_rate);
    voice->amp_decay_coef = decay_coef_for_seconds(decay_seconds, sample_rate);
    voice->amp_release_coef = decay_coef_for_seconds(release_seconds, sample_rate);
}

static void synth_init_dls_modulators(synth_voice_t *voice, const dls_articulation_t *articulation,
                                      uint32_t sample_rate) {
    voice->lfo_pitch_cents = articulation->lfo_pitch_cents;
    voice->mod_lfo_pitch_cents = articulation->mod_lfo_pitch_cents;
    if (voice->lfo_pitch_cents != 0.0 || voice->mod_lfo_pitch_cents != 0.0) {
        voice->lfo_frequency_hz = articulation->has_lfo_frequency
                                      ? dls_absolute_cents_to_hz(articulation->lfo_frequency)
                                      : 5.0;
        voice->lfo_frequency_hz = clamp_double(voice->lfo_frequency_hz, 0.01, 40.0);
        double delay_seconds = articulation->has_lfo_delay ? dls_timecents_to_seconds(articulation->lfo_delay) : 0.0;
        voice->lfo_delay_samples = delay_seconds * (double) sample_rate;
    }

    if (articulation->has_filter_cutoff) {
        double cutoff = dls_absolute_cents_to_hz(articulation->filter_cutoff);
        cutoff = clamp_double(cutoff, 20.0, (double) sample_rate * 0.45);
        voice->filter_f = 2.0 * sin(M_PI * cutoff / (double) sample_rate);
        voice->filter_f = clamp_double(voice->filter_f, 0.001, 0.99);

        double q_db = articulation->has_filter_q ? (double) articulation->filter_q / 65536.0 : 0.0;
        double q = pow(10.0, q_db / 20.0);
        q = clamp_double(q, 0.5, 12.0);
        voice->filter_damp = clamp_double(1.0 / q, 0.05, 2.0);
        voice->filter_enabled = true;
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
        synth_init_dls_envelope(voice, &instrument->articulation, synth->sample_rate);
        synth_init_dls_modulators(voice, &instrument->articulation, synth->sample_rate);
        if (voice->use_dls_env) {
            voice->env_level = velocity;
            voice->attack_target = 0;
            voice->sustain_level = 0;
            voice->decay_shift = 0;
            return;
        }

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
                case 0x01:
                    channel->modulation = event->data2 & 0x7f;
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
                    channel->modulation = 0;
                    channel->reverb = 0;
                    channel->chorus = 0;
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
                case 0x5b:
                    channel->reverb = event->data2 & 0x7f;
                    break;
                case 0x5d:
                    channel->chorus = event->data2 & 0x7f;
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

static double delay_read_linear(const double *buffer, size_t length, size_t write_pos, double delay_samples) {
    if (!buffer || length == 0) return 0.0;
    double read_pos = (double) write_pos - delay_samples;
    while (read_pos < 0.0) read_pos += (double) length;
    while (read_pos >= (double) length) read_pos -= (double) length;
    size_t i0 = (size_t) read_pos;
    size_t i1 = i0 + 1u;
    if (i1 >= length) i1 = 0;
    double frac = read_pos - (double) i0;
    return buffer[i0] + (buffer[i1] - buffer[i0]) * frac;
}

static void synth_render_one(synth_t *synth, double *left, double *right) {
    double l = 0.0;
    double r = 0.0;
    double reverb_send_l = 0.0;
    double reverb_send_r = 0.0;
    double chorus_send_l = 0.0;
    double chorus_send_r = 0.0;

    for (int i = 0; i < MAX_VOICES; ++i) {
        synth_voice_t *voice = &synth->voices[i];
        if (!voice->active) continue;

        if (voice->use_dls_env) {
            switch (voice->amp_stage) {
                case AMP_ATTACK:
                    voice->amp_env += voice->amp_attack_step;
                    if (voice->amp_env >= 1.0) {
                        voice->amp_env = 1.0;
                        voice->amp_stage = AMP_DECAY;
                    }
                    break;
                case AMP_DECAY:
                    if (voice->amp_env > voice->amp_sustain) {
                        voice->amp_env = voice->amp_sustain + (voice->amp_env - voice->amp_sustain) * voice->amp_decay_coef;
                        if (voice->amp_env <= voice->amp_sustain + 0.0001) {
                            voice->amp_env = voice->amp_sustain;
                            voice->amp_stage = AMP_SUSTAIN;
                        }
                    } else {
                        voice->amp_env = voice->amp_sustain;
                        voice->amp_stage = AMP_SUSTAIN;
                    }
                    break;
                case AMP_RELEASE:
                    voice->amp_env *= voice->amp_release_coef;
                    if (voice->amp_env < 0.0001) {
                        voice->active = false;
                        continue;
                    }
                    break;
                default:
                    break;
            }
            voice->env_level = (uint8_t) lrint(fmin(127.0, fmax(0.0, voice->amp_env * (double) voice->velocity)));
        } else if ((voice->age & 255u) == 0 && !voice->percussion) {
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

        midi_channel_t *channel = &synth->channels[voice->channel];
        double local_step = voice->step;
        if ((voice->lfo_pitch_cents != 0.0 || voice->mod_lfo_pitch_cents != 0.0) &&
            (double) voice->age >= voice->lfo_delay_samples) {
            double lfo = sin(voice->lfo_phase);
            double pitch_cents = lfo * (voice->lfo_pitch_cents +
                                        voice->mod_lfo_pitch_cents * ((double) channel->modulation / 127.0));
            local_step *= pow(2.0, pitch_cents / 1200.0);
            voice->lfo_phase += 2.0 * M_PI * voice->lfo_frequency_hz / (double) synth->sample_rate;
            if (voice->lfo_phase >= 2.0 * M_PI) voice->lfo_phase = fmod(voice->lfo_phase, 2.0 * M_PI);
        }

        double sample = wave_sample_mono(voice->wave, voice->position);
        if (voice->filter_enabled) {
            voice->filter_low += voice->filter_f * voice->filter_band;
            double high = sample - voice->filter_low - voice->filter_damp * voice->filter_band;
            voice->filter_band += voice->filter_f * high;
            sample = voice->filter_low;
        }

        double channel_gain = ((double) channel->volume / 127.0) * ((double) channel->expression / 127.0);
        double env_gain = voice->use_dls_env
                              ? ((double) voice->velocity / 127.0) * voice->amp_env
                              : (double) voice->env_level / 127.0;
        double gain = env_gain * channel_gain * voice->attenuation_gain;
        if (voice->percussion) gain *= voice->percussion_gain;
        double pan = (double) channel->pan / 127.0;
        double pan_l = cos(pan * M_PI * 0.5);
        double pan_r = sin(pan * M_PI * 0.5);
        double dry_l = sample * gain * pan_l;
        double dry_r = sample * gain * pan_r;

        l += dry_l;
        r += dry_r;

        if (channel->reverb) {
            double send = (double) channel->reverb / 127.0;
            reverb_send_l += dry_l * send;
            reverb_send_r += dry_r * send;
        }
        if (channel->chorus) {
            double send = (double) channel->chorus / 127.0;
            chorus_send_l += dry_l * send;
            chorus_send_r += dry_r * send;
        }

        const bool region_looped = voice->region->looped || (!voice->region->has_wsmp && voice->wave->looped);
        const uint32_t loop_start = voice->region->looped ? voice->region->loop_start : voice->wave->loop_start;
        const uint32_t loop_length = voice->region->looped ? voice->region->loop_length : voice->wave->loop_length;
        double loop_end = (double) loop_start + (double) loop_length;

        voice->position += local_step;
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

    if (synth->reverb_l && synth->reverb_r && synth->reverb_len > 0) {
        double wet_l = synth->reverb_l[synth->reverb_pos];
        double wet_r = synth->reverb_r[synth->reverb_pos];
        synth->reverb_l[synth->reverb_pos] = reverb_send_l * 0.35 + wet_l * 0.52 + wet_r * 0.18;
        synth->reverb_r[synth->reverb_pos] = reverb_send_r * 0.35 + wet_r * 0.52 + wet_l * 0.18;
        synth->reverb_pos++;
        if (synth->reverb_pos >= synth->reverb_len) synth->reverb_pos = 0;
        l += wet_l * 0.28;
        r += wet_r * 0.28;
    }

    if (synth->chorus_l && synth->chorus_r && synth->chorus_len > 0) {
        double base_delay = (double) synth->sample_rate * 0.018;
        double depth = (double) synth->sample_rate * 0.006;
        double delay_l = base_delay + depth * (0.5 + 0.5 * sin(synth->chorus_phase));
        double delay_r = base_delay + depth * (0.5 + 0.5 * sin(synth->chorus_phase + M_PI * 0.5));
        double wet_l = delay_read_linear(synth->chorus_l, synth->chorus_len, synth->chorus_pos, delay_l);
        double wet_r = delay_read_linear(synth->chorus_r, synth->chorus_len, synth->chorus_pos, delay_r);
        synth->chorus_l[synth->chorus_pos] = chorus_send_l;
        synth->chorus_r[synth->chorus_pos] = chorus_send_r;
        synth->chorus_pos++;
        if (synth->chorus_pos >= synth->chorus_len) synth->chorus_pos = 0;
        synth->chorus_phase += 2.0 * M_PI * 0.35 / (double) synth->sample_rate;
        if (synth->chorus_phase >= 2.0 * M_PI) synth->chorus_phase -= 2.0 * M_PI;
        l += wet_l * 0.32;
        r += wet_r * 0.32;
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
    synth_free(&synth);
    fprintf(stderr, "WAV: %u Hz stereo, %u frames written to %s\n",
            sample_rate, writer.frames_written, wav_path);
    return ok;
}

static const char *find_default_dls(void) {
    static const char *candidates[] = {
        "gm.dls",
        "GM.DLS",
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
            "renders a Standard MIDI File through a user-supplied RIFF DLS GM bank to 16-bit stereo WAV\n",
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
        fprintf(stderr, "gm.dls not found in the current directory; pass the path explicitly\n");
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
