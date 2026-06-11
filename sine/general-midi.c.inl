#pragma GCC optimize("Ofast")
#pragma once
#include "general-midi.h"

// #define DEBUG_MIDI

#if defined(DEBUG_MIDI)
#define debug_log(...) printf(__VA_ARGS__)
#else
#define debug_log(...)
#endif

#define MAX_MIDI_VOICES 32
#define MIDI_CHANNELS 16

typedef struct midi_voice_s {
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    uint8_t velocity_base;
    uint8_t decay_shift;
    uint8_t sustain_level;
    uint8_t attack_target;   // >0 = attack phase, target velocity
    uint8_t drum_type;       // cached GM percussion synthesis type (channel 9 only)
    uint16_t pan_l_q8;
    uint16_t pan_r_q8;

    int32_t frequency_m100;
    uint32_t phase;          // melodic oscillator phase accumulator (wraps over one period)
    uint32_t phase_inc;      // phase advance per sample, derived from frequency_m100
    uint32_t drum_env;       // percussion body amplitude envelope (Q16), channel 9 only
    uint32_t drum_punch;     // fast attack-transient envelope (Q16) layered on top for punch
    int32_t  gain_q8;        // melodic audio-rate gain (Q8), tracks `velocity` smoothly
    int32_t  gain_inc;       // per-sample gain ramp toward the control-rate target
    uint16_t sample_position;
} midi_voice_t;

typedef struct midi_channel_s {
    uint8_t program;
    uint8_t volume;
    uint8_t pan;
    int32_t pitch;
} midi_channel_t;

typedef struct __attribute__((packed)) {
    uint8_t command;
    uint8_t note;
    uint8_t velocity;
    uint8_t other;
} midi_command_t;

static midi_voice_t midi_voices[MAX_MIDI_VOICES] = {0};
static midi_channel_t midi_channels[MIDI_CHANNELS] = {
    {0, 100, 64, 0}, {0, 100, 64, 0}, {0, 100, 64, 0}, {0, 100, 64, 0},
    {0, 100, 64, 0}, {0, 100, 64, 0}, {0, 100, 64, 0}, {0, 100, 64, 0},
    {0, 100, 64, 0}, {0, 100, 64, 0}, {0, 100, 64, 0}, {0, 100, 64, 0},
    {0, 100, 64, 0}, {0, 100, 64, 0}, {0, 100, 64, 0}, {0, 100, 64, 0},
};

// Bitmask for active voices
static uint32_t active_voice_bitmask = 0;
// Bitmask for sustained channels
static uint32_t channels_sustain_bitmask = 0;

// Best seeds for different percussion characteristics
static uint32_t noise_seed = 0x7EC80000; // Best overall for drums

// Alternative excellent seeds:
// 0x7EC80000 - Optimal: Maximum length sequence, good spectral distribution
// 0xDEADBEEF - Classic: Good randomness, widely used
// 0x8B8B8B8B - Balanced: Equal bit distribution
// 0xAAAA5555 - Alternating: Good for metallic sounds
// 0x13579BDF - Fibonacci-like: Natural sounding randomness

#define SET_ACTIVE_VOICE(idx) (active_voice_bitmask |= (1U << (idx)))
#define CLEAR_ACTIVE_VOICE(idx) (active_voice_bitmask &= ~(1U << (idx)))
#define IS_ACTIVE_VOICE(idx) ((active_voice_bitmask & (1U << (idx))) != 0)

#define SET_CHANNEL_SUSTAIN(idx) (channels_sustain_bitmask |= (1U << (idx)))
#define CLEAR_CHANNEL_SUSTAIN(idx) (channels_sustain_bitmask &= ~(1U << (idx)))
#define IS_CHANNEL_SUSTAIN(idx) ((channels_sustain_bitmask & (1U << (idx))) != 0)

#define DRUM_BODY_GAIN 96u
#define DRUM_PUNCH_GAIN 64u

#ifndef __fast_mul
#define __fast_mul(a, b) ((a) * (b))
#endif

static INLINE int32_t sine_mul_q8_i32(int32_t a, uint16_t b) {
    return __fast_mul(a, (int32_t) b) >> 8;
}

// bend: 0-16383, center 8192. Returns multiplier ×10000
static INLINE int32_t calc_pitch_bend(const int bend) {
    // Map bend (0..16383) to fixed-point position in table (0..24), scaled by 256 for lerp
    // pos = 24 * 256 * (bend - 0) / 16383 = 6144 * bend / 16383
    const int32_t pos = (int32_t) bend * 6144 / 16383; // 0..6144
    const int idx = pos >> 8; // 0..24
    if (idx >= 24) return pitch_bend_table[24];
    const int frac = pos & 255; // 0..255
    return pitch_bend_table[idx] + (((pitch_bend_table[idx + 1] - pitch_bend_table[idx]) * frac) >> 8);
}

#define SIN_STEP (SOUND_FREQUENCY * 100 / 4096)

// Map a 12-bit phase index (0..4095, = one full period) to the quarter-wave
// table with sign/mirror reconstruction. Returns the Q8 sample (±32512); the
// melodic path multiplies by velocity then shifts >>8.
static INLINE int32_t sine_from_index(const uint32_t index) {
    return index < 2048
               ? sin16_q8[index < 1024 ? index : 2047 - index]
               : -sin16_q8[index < 3072 ? index - 2048 : 4095 - index];
}

// Drum path: angle->index form (fixed frequencies, short bursts), scaled back to
// the ±127 unit the noise generator and drum mixes work in.
static INLINE int32_t sine_lookup(const uint32_t angle) {
    return sine_from_index((angle / SIN_STEP) & 4095) >> 8;
}

// Phase increment per sample for a free-running uint32 accumulator whose top
// 12 bits index the sine table: inc = freq_m100 * 2^32 / (100 * SOUND_FREQUENCY).
// Computed only on note-on / pitch-bend, never in the per-sample loop.
static INLINE uint32_t calc_phase_inc(const int32_t frequency_m100) {
    return (uint32_t) (((uint64_t) (uint32_t) frequency_m100 << 32) /
                       (100u * (uint32_t) SOUND_FREQUENCY));
}

static INLINE uint16_t pan_left_q8(uint8_t pan) {
    return pan <= 64 ? 256u : (uint16_t) (((uint32_t) (127u - pan) * 256u + 31u) / 63u);
}

static INLINE uint16_t pan_right_q8(uint8_t pan) {
    return pan >= 64 ? 256u : (uint16_t) (((uint32_t) pan * 256u + 32u) >> 6);
}

static INLINE int16_t generate_noise() {
    // Linear feedback shift register for white noise
    noise_seed = (noise_seed >> 1) ^ (-(noise_seed & 1) & 0xD0000001);
    return (int16_t) ((noise_seed & 0xFFFF) - 0x8000) >> 8;
}

// Bright "metallic" noise for cymbals/hats: a second-order high-pass (double
// first-difference, +12 dB/oct) of the white LFSR. Flat white noise sounds like
// static/"shh"; pushing the energy up top gives the "tss" shimmer of a real
// cymbal. State is global - all cymbals are bright noise, so sharing it is
// inaudible. Output is in the same ±127 unit as generate_noise().
static int16_t metal_d1 = 0, metal_d2 = 0;
static INLINE int16_t generate_metal(void) {
    const int16_t n = generate_noise();
    const int16_t d1 = (int16_t) (n - metal_d1);
    const int16_t hp = (int16_t) (d1 - metal_d2);
    metal_d1 = n;
    metal_d2 = d1;
    return (int16_t) (hp >> 1);   // normalise the +12 dB/oct gain back toward ±127
}

// Drum-specific synthesis
static INLINE int32_t generate_drum_sample(midi_voice_t *voice, const uint16_t sample_position) {
    int32_t sample = 0;

    // Amplitude = body + fast attack transient. The transient peaks ~2x the body
    // and collapses in a few ms, so the hit snaps/punches instead of being flat.
    const int32_t amp = (int32_t) ((voice->drum_env + voice->drum_punch) >> 16);

    // Synthesis type cached at note-on (see parse_midi)
    switch (voice->drum_type) {
        case DRUM_T_KICK: {
            // KICK: pitch sweep ~5x -> 1x over the first ~512 samples (~23 ms) via
            // the phase accumulator. Continuous phase = a real "click->thud" with
            // audible midrange, not the click-per-block the old freq*position
            // lookup produced and not pure sub-bass you can't hear on small speakers.
            uint32_t inc = voice->phase_inc;
            if (sample_position < 512)
                inc += (voice->phase_inc >> 7) * (uint32_t) (512 - sample_position);
            voice->phase += inc;
            const int32_t tone = sine_from_index(voice->phase >> 20) >> 8; // ±127

            if (sample_position < 64)                 // bright beater click
                sample = __fast_mul(amp, generate_noise());
            sample += __fast_mul(amp, tone);
            break;
        }

        case DRUM_T_SNARE: {
            // SNARE: bright noise over a short ~180 Hz body tone.
            const int32_t tone = sine_lookup(__fast_mul(18000, sample_position));
            const int16_t noise = generate_noise();
            sample = __fast_mul(amp, (noise * 7 + tone * 3) / 10);
            break;
        }

        case DRUM_T_TOM: {
            // TOM: tonal drum, pitch from the note (phase accumulator) + a touch
            // of noise on the attack.
            voice->phase += voice->phase_inc;
            const int32_t tone = sine_from_index(voice->phase >> 20) >> 8; // ±127
            const int16_t noise = generate_noise();
            sample = __fast_mul(amp, tone + (noise >> 2));
            break;
        }

        case DRUM_T_HIHAT_C:
        case DRUM_T_HIHAT_O: {
            // HI-HAT: bright high-passed "tss" noise. Open vs closed differ only
            // in the envelope decay rate (drum_decay table).
            sample = __fast_mul(amp, generate_metal());
            break;
        }

        case DRUM_T_CRASH: {
            // CRASH: dense bright shimmer (two metal layers) for a long wash.
            sample = __fast_mul(amp, generate_metal() + generate_metal());
            break;
        }

        case DRUM_T_RIDE: {
            // RIDE: tonal 500 Hz "ping" + bright metallic wash.
            const int32_t ping = sine_lookup(__fast_mul(50000, sample_position));
            sample = __fast_mul(amp, (ping + generate_metal()) >> 1);
            break;
        }

        case DRUM_T_COWBELL: {
            // COWBELL: pure ~800 Hz tone.
            const int32_t tone = sine_lookup(__fast_mul(80000, sample_position));
            sample = __fast_mul(amp, tone);
            break;
        }

        case DRUM_T_STICK:
        case DRUM_T_CLAP:
        default: {
            // STICK / CLAP / GENERIC: shaped noise burst (length set by decay).
            sample = __fast_mul(amp, generate_noise());
            break;
        }
    }

    // Natural exponential tail; the caller frees the slot once it goes inaudible.
    voice->drum_env   -= voice->drum_env   >> voice->decay_shift;
    voice->drum_punch -= voice->drum_punch >> 6;   // ~3 ms transient: the punch
    // Full level (drum body ~= one melodic voice, attack ~3x); the int32 mix in
    // midi_sample saturates, so the loud transient is what makes drums cut through.
    return sample;
}

static INLINE void midi_sample_stereo(int16_t *out_l, int16_t *out_r) {
    if (__builtin_expect(!active_voice_bitmask, 0)) {
        *out_l = 0;
        *out_r = 0;
        return;
    }

    int32_t l = 0, r = 0;
    uint32_t active_voices = active_voice_bitmask;

    do {
        const uint32_t voice_index = __builtin_ctz(active_voices);
        const uint32_t voice_bit = 1U << voice_index;
        active_voices ^= voice_bit;

        midi_voice_t *__restrict voice = &midi_voices[voice_index];
        const uint16_t sample_position = voice->sample_position++;
        int32_t v = 0;

        if (voice->channel == 9) {
            v = generate_drum_sample(voice, sample_position);
            if (voice->drum_env < (1u << 16)) {
                active_voice_bitmask &= ~voice_bit;
            }
        } else {
            if (__builtin_expect((sample_position & 255) == 0 && sample_position > 0, 0)) {
                if (voice->attack_target) {
                    voice->velocity += ((voice->attack_target - voice->velocity) >> voice->decay_shift) | 1;
                    if (voice->velocity >= voice->attack_target) {
                        voice->velocity = voice->attack_target;
                        voice->attack_target = 0;
                        const gm_envelope_t *env = &gm_envelopes[midi_channels[voice->channel].program];
                        voice->decay_shift = env->decay_shift;
                    }
                } else {
                    const uint8_t target = voice->sustain_level;
                    if (voice->velocity > target) {
                        voice->velocity -= ((voice->velocity - target) >> voice->decay_shift) | 1;
                    }
                }
                voice->gain_inc = (((int32_t) voice->velocity << 8) - voice->gain_q8) >> 8;
                if (voice->velocity == 0 && voice->gain_q8 < 256) {
                    active_voice_bitmask &= ~voice_bit;
                    continue;
                }
            }

            const int32_t sine_val = sine_from_index(voice->phase >> 20);
            voice->phase += voice->phase_inc;
            voice->gain_q8 += voice->gain_inc;
            v = __fast_mul(voice->gain_q8, sine_val) >> 16;
        }

        l += sine_mul_q8_i32(v, voice->pan_l_q8);
        r += sine_mul_q8_i32(v, voice->pan_r_q8);
    } while (active_voices);

    l >>= 2;
    r >>= 2;
    if (__builtin_expect(l > 32767, 0)) l = 32767;
    else if (__builtin_expect(l < -32768, 0)) l = -32768;
    if (__builtin_expect(r > 32767, 0)) r = 32767;
    else if (__builtin_expect(r < -32768, 0)) r = -32768;
    *out_l = (int16_t) l;
    *out_r = (int16_t) r;
}

// Optimized pitch bend calculation with lookup table or approximation
static INLINE int32_t apply_pitch(const int32_t base_frequency, const int32_t cents) {
    // Optimized: avoid division if cents is zero
    return __builtin_expect(cents == 0, 1) ? base_frequency : (base_frequency * cents + 5000) / 10000;
}

static INLINE uint8_t melodic_velocity(uint8_t channel, uint8_t velocity) {
    const gm_envelope_t *env = &gm_envelopes[midi_channels[channel].program];
    uint16_t v = ((uint16_t) midi_channels[channel].volume * velocity) >> 7;
    v = (v * env->gain) >> 7;
    return v > 255 ? 255 : (uint8_t) v;
}

static INLINE void parse_midi(const midi_command_t *message) {
    const uint8_t channel = message->command & 0xf;

    switch (message->command >> 4) {
        case 0x9: // Note ON
            if (__builtin_expect(message->velocity != 0, 1)) {
                // Kill previous voice with same note+channel (MIDI spec: re-trigger)
                {
                    uint32_t voices_to_check = active_voice_bitmask;
                    while (voices_to_check) {
                        const uint32_t vs = __builtin_ctz(voices_to_check);
                        voices_to_check &= ~(1U << vs);
                        if (midi_voices[vs].channel == channel && midi_voices[vs].note == message->note) {
                            CLEAR_ACTIVE_VOICE(vs);
                            break;
                        }
                    }
                }

                // Find free voice slot using bit operations
                const uint32_t free_voices = ~active_voice_bitmask;
                if (__builtin_expect(free_voices != 0, 1)) {
                    const uint32_t voice_slot = __builtin_ctz(free_voices);
                    if (voice_slot < MAX_MIDI_VOICES) {
                        midi_voice_t *__restrict voice = &midi_voices[voice_slot];

                        // Initialize voice data
                        voice->sample_position = 0;
                        voice->phase = 0;
                        voice->channel = channel;
                        voice->note = message->note;
                        voice->velocity_base = message->velocity;
                        voice->pan_l_q8 = pan_left_q8(midi_channels[channel].pan);
                        voice->pan_r_q8 = pan_right_q8(midi_channels[channel].pan);

                        // Cache the percussion synthesis type once (channel 9)
                        voice->drum_type = (message->note >= 35 && message->note <= 81)
                                               ? drum_type_map[message->note - 35]
                                               : DRUM_T_GENERIC;

                        // Apply pitch bend and volume in one go
                        voice->frequency_m100 = apply_pitch(
                            note_frequencies_m_100[message->note],
                            midi_channels[channel].pitch
                        );
                        voice->phase_inc = calc_phase_inc(voice->frequency_m100);

                        voice->velocity = (channel == 9)
                                          ? (uint8_t) (((uint16_t) midi_channels[channel].volume * message->velocity) >> 7)
                                          : melodic_velocity(channel, message->velocity);

                        // Percussion (channel 9): seed the exponential drum
                        // envelope and pick the per-type decay rate. The melodic
                        // envelope fields below are then unused for this voice.
                        if (channel == 9) {
                            voice->drum_env = (((uint32_t) voice->velocity * DRUM_BODY_GAIN) >> 7) << 16;
                            voice->drum_punch = (((uint32_t) voice->velocity * DRUM_PUNCH_GAIN) >> 7) << 16;
                            voice->decay_shift = drum_decay[voice->drum_type];
                            SET_ACTIVE_VOICE(voice_slot);
                            break;
                        }

                        // Set envelope from GM program table
                        const gm_envelope_t *env = &gm_envelopes[midi_channels[channel].program];
                        voice->sustain_level = (uint8_t) (((uint16_t) voice->velocity * env->sustain_level) / 200u);
                        if (env->attack_shift) {
                            // Slow attack: start from zero, rise to target
                            voice->attack_target = voice->velocity;
                            voice->velocity = 0;
                            voice->decay_shift = env->attack_shift;
                        } else {
                            // Instant attack
                            voice->attack_target = 0;
                            voice->decay_shift = env->decay_shift;
                        }

                        // Start the audio-rate gain at zero and ramp to the
                        // current level over the first 256 samples: an instant
                        // attack gets a click-free ~11 ms fade-in; a slow attack
                        // (velocity == 0 now) stays silent until the ADSR lifts it.
                        voice->gain_q8 = 0;
                        voice->gain_inc = voice->velocity;

                        SET_ACTIVE_VOICE(voice_slot);
                        break;
                    }
                }
            }
            [[fallthrough]]; // Note On with velocity 0 is Note Off.
        case 0x8: // Note OFF
            /* Probably we should
             * Find the first and last entry in the voices list with matching channel, key and look up the smallest play position
             */
            if (!IS_CHANNEL_SUSTAIN(channel)) {
                // Optimized voice search with early termination
                uint32_t voices_to_check = active_voice_bitmask;
                while (voices_to_check) {
                    const uint32_t voice_slot = __builtin_ctz(voices_to_check);
                    voices_to_check &= ~(1U << voice_slot);

                    midi_voice_t *__restrict voice = &midi_voices[voice_slot];
                    if (voice->channel == channel && voice->note == message->note) {
                        if (__builtin_expect(channel == 9, 0)) {
                            CLEAR_ACTIVE_VOICE(voice_slot);
                        } else {
                            // Switch to release: fast exponential decay to zero
                            voice->attack_target = 0;
                            voice->sustain_level = 0;
                            voice->decay_shift = 2;
                        }
                        break;
                    }
                }
            }
            break;


        case 0xB: // Controller Change
            switch (message->note) {
                case 0x7: // Volume change
                    debug_log("[MIDI] Channel %i volume %i\n", channel, midi_channels[channel].volume);
                    midi_channels[channel].volume = message->velocity;
                    for (int voice_slot = 0; voice_slot < MAX_MIDI_VOICES; ++voice_slot) {
                        if (midi_voices[voice_slot].channel == channel) {
                            const uint8_t new_vel = (channel == 9)
                                                    ? (uint8_t) (((uint16_t) message->velocity * midi_voices[voice_slot].velocity_base) >> 7)
                                                    : melodic_velocity(channel, midi_voices[voice_slot].velocity_base);
                            midi_voices[voice_slot].velocity = new_vel;
                            if (midi_voices[voice_slot].attack_target)
                                midi_voices[voice_slot].attack_target = new_vel;
                        }
                    }
                    break;
                case 0x0A: //  Left-right pan
                    midi_channels[channel].pan = message->velocity & 0x7f;
                    break;
                case 0x40: // Sustain
                    if (message->velocity & 64) {
                        SET_CHANNEL_SUSTAIN(channel);
                    } else {
                        CLEAR_CHANNEL_SUSTAIN(channel);

                        for (int voice_slot = 0; voice_slot < MAX_MIDI_VOICES; ++voice_slot)
                            if (midi_voices[voice_slot].channel == channel) {
                                if (channel == 9) {
                                    CLEAR_ACTIVE_VOICE(voice_slot);
                                } else {
                                    // Switch to release: fast decay to zero
                                    midi_voices[voice_slot].attack_target = 0;
                                    midi_voices[voice_slot].sustain_level = 0;
                                    midi_voices[voice_slot].decay_shift = 2;
                                }
                            }
                    }
                    debug_log("[MIDI] Channel %i sustain %i\n", channel, message->velocity);

                    break;
                case 0x78: // All Sound Off
                case 0x7b: // All Notes Off
                    active_voice_bitmask = 0;
                    channels_sustain_bitmask = 0;
                    memset(midi_voices, 0, sizeof(midi_voices));
                    /*
                                    for (int voice_number = 0; voice_number < MAX_MIDI_VOICES; ++voice_number)
                                        midi_voices[voice_number].playing = 0;
                    */
                    break;
                case 0x79: // all controllers off
                    memset(midi_channels, 0, sizeof(midi_channel_t) * MIDI_CHANNELS);
                    for (int i = 0; i < MIDI_CHANNELS; i++) {
                        midi_channels[i].volume = 100;
                        midi_channels[i].pan = 64;
                    }
                    break;
                default:
                    debug_log("[MIDI] Unknown channel %i controller %02x %02x\n", channel, message->note,
                              message->velocity);
            }
            break;

        case 0xC: // Channel Program
            midi_channels[channel].program = message->note;

            for (int voice_slot = 0; voice_slot < MAX_MIDI_VOICES; ++voice_slot)
                if (midi_voices[voice_slot].channel == channel) {
                    CLEAR_ACTIVE_VOICE(voice_slot);
                }

            debug_log("[MIDI] Channel %i program %i\n", message->command & 0xf, message->note);
            break;
        case 0xE: {
            const int pitch_bend = (message->velocity * 128 + message->note);
            const int cents = calc_pitch_bend(pitch_bend);
            midi_channels[channel].pitch = cents;
            debug_log("[MIDI] Channel %i pitch_bend %i cents %i 44000->%i\n", channel, pitch_bend - 8192, cents,
                      apply_pitch(44000, cents));
            for (int voice_slot = 0; voice_slot < MAX_MIDI_VOICES; ++voice_slot)
                if (midi_voices[voice_slot].channel == channel) {
                    midi_voices[voice_slot].frequency_m100 = apply_pitch(
                        note_frequencies_m_100[midi_voices[voice_slot].note], cents);
                    midi_voices[voice_slot].phase_inc = calc_phase_inc(
                        midi_voices[voice_slot].frequency_m100);
                }
            break;
        }
        default:
            debug_log("[MIDI] Unknown channel %i command %x message %04x \n", channel, message->command >> 4,
                      midi_command);
            break;
    }
}
