// Shared Standard MIDI File (SMF) loader: parses format 0/1 files into a single
// tick-sorted event stream. Used by the host render harness
// (examples/wt_render.c) to drive the packed wavetable engine.
//
// Requires dls_parse.inl to be included first (uses file_blob_t, read_entire_file,
// free_file_blob, fourcc_is, rd_u16be, rd_u32be).
#pragma once

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
