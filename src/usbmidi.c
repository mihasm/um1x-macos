/*
 * usbmidi.c - MIDI 1.0 byte stream <-> USB-MIDI 1.0 event packets <-> UMP.
 * See usbmidi.h.
 */
#include "usbmidi.h"

#include <stdio.h>
#include <string.h>

/* USB Device Class Definition for MIDI Devices 1.0, table 4-1. */
const uint8_t usbmidi_cin_length[16] = {
    0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1,
};

int ump_word_count(uint32_t word0)
{
    static const uint8_t words[16] = { 1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4 };
    return words[word0 >> 28];
}

static int channel_len(uint8_t status)
{
    uint8_t t = status & 0xF0;
    return (t == 0xC0 || t == 0xD0) ? 2 : 3;
}

/* Length of a system common message by status, 0 for undefined ones. */
static int common_len(uint8_t status)
{
    switch (status) {
    case 0xF1: case 0xF3: return 2;
    case 0xF2: return 3;
    case 0xF6: return 1;
    default: return 0;
    }
}

/* ------------------------------------------------------------------------ */

void midi_parser_reset(midi_parser *p)
{
    memset(p, 0, sizeof *p);
}

void midi_parser_feed(midi_parser *p, uint8_t b, const midi_sink *sink)
{
    if (b >= 0xF8) {                    /* real-time: anywhere, no state change */
        sink->message(sink->ctx, &b, 1);
        return;
    }
    if (b & 0x80) {                     /* status byte */
        if (p->in_sysex) {
            p->in_sysex = false;
            sink->sysex(sink->ctx, 0xF7);   /* real or synthesised end */
            if (b == 0xF7)
                return;
        }
        p->len = 0;
        if (b == 0xF0) {
            p->in_sysex = true;
            p->running = 0;
            sink->sysex(sink->ctx, 0xF0);
            return;
        }
        if (b < 0xF0) {
            p->running = b;
            p->buf[0] = b;
            p->len = 1;
            p->need = (uint8_t)channel_len(b);
            return;
        }
        p->running = 0;                 /* system common cancels running status */
        int n = common_len(b);
        if (n == 1) {
            sink->message(sink->ctx, &b, 1);
        } else if (n > 1) {
            p->buf[0] = b;
            p->len = 1;
            p->need = (uint8_t)n;
        }
        return;                         /* stray F7, undefined F4/F5: dropped */
    }
    /* data byte */
    if (p->in_sysex) {
        sink->sysex(sink->ctx, b);
        return;
    }
    if (p->len == 0) {
        if (!p->running)
            return;                     /* data without status: dropped */
        p->buf[0] = p->running;
        p->len = 1;
        p->need = (uint8_t)channel_len(p->running);
    }
    p->buf[p->len++] = b;
    if (p->len == p->need) {
        sink->message(sink->ctx, p->buf, p->len);
        p->len = 0;
    }
}

/* ------------------------------------------------------------------------ */

int usbmidi_decode_packet(const uint8_t pkt[4], midi_parser parsers[16],
                          const midi_sink sinks[16], uint16_t cable_mask)
{
    if (pkt[0] == 0)
        return -1;                      /* padding / empty packet */
    int cable = pkt[0] >> 4;
    int cin = pkt[0] & 0x0F;
    int len = usbmidi_cin_length[cin];
    if (len == 0 || !(cable_mask & (1u << cable)))
        return -1;
    const uint8_t *b = pkt + 1;
    /* Some firmwares put running-status messages into channel packets with
     * the status byte left out; the data then starts at byte 1. */
    if (cin >= 0x8 && cin <= 0xE && b[0] < 0x80)
        len--;
    for (int i = 0; i < len; i++)
        midi_parser_feed(&parsers[cable], b[i], &sinks[cable]);
    return cable;
}

/* ------------------------------------------------------------------------ */

static void enc_put(usbmidi_encoder *e, uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3)
{
    if (e->out_len + 4 > e->out_cap) {
        e->overflow = true;
        return;
    }
    uint8_t *o = e->out + e->out_len;
    o[0] = (uint8_t)(e->cable << 4 | cin);
    o[1] = b1;
    o[2] = b2;
    o[3] = b3;
    e->out_len += 4;
}

static void enc_message(void *ctx, const uint8_t *m, int len)
{
    usbmidi_encoder *e = ctx;
    uint8_t s = m[0], cin;
    if (s >= 0xF8)
        cin = 0xF;
    else if (s < 0xF0)
        cin = s >> 4;
    else if (len == 1)
        cin = 0x5;                      /* F6 tune request */
    else
        cin = len == 2 ? 0x2 : 0x3;
    enc_put(e, cin, s, len > 1 ? m[1] : 0, len > 2 ? m[2] : 0);
}

static void enc_sysex(void *ctx, uint8_t b)
{
    usbmidi_encoder *e = ctx;
    if (b == 0xF0)
        e->sx_len = 0;
    e->sx[e->sx_len++] = b;
    if (b == 0xF7) {                    /* CIN 5/6/7: SysEx ends with 1/2/3 bytes */
        uint8_t n = e->sx_len;
        enc_put(e, (uint8_t)(0x4 + n), e->sx[0], n > 1 ? e->sx[1] : 0, n > 2 ? e->sx[2] : 0);
        e->sx_len = 0;
    } else if (e->sx_len == 3) {        /* CIN 4: SysEx starts or continues */
        enc_put(e, 0x4, e->sx[0], e->sx[1], e->sx[2]);
        e->sx_len = 0;
    }
}

void usbmidi_encoder_init(usbmidi_encoder *e, uint8_t cable, uint8_t *out, size_t cap)
{
    memset(e, 0, sizeof *e);
    e->cable = cable & 0x0F;
    e->out = out;
    e->out_cap = cap;
}

midi_sink usbmidi_encoder_sink(usbmidi_encoder *e)
{
    return (midi_sink){ enc_message, enc_sysex, e };
}

/* ------------------------------------------------------------------------ */

int ump_to_midi(const uint32_t *w, int avail, midi_parser *p, const midi_sink *sink)
{
    if (avail < 1)
        return 0;
    int n = ump_word_count(w[0]);
    if (avail < n)
        return 0;
    uint8_t status = (uint8_t)(w[0] >> 16);
    uint8_t d1 = (w[0] >> 8) & 0x7F, d2 = w[0] & 0x7F;

    switch (w[0] >> 28) {
    case 0x1: {                         /* system common / real-time */
        int len = status >= 0xF8 ? 1 : common_len(status);
        if (len == 0 || status == 0xF0 || status == 0xF7)
            break;
        midi_parser_feed(p, status, sink);
        if (len > 1) midi_parser_feed(p, d1, sink);
        if (len > 2) midi_parser_feed(p, d2, sink);
        break;
    }
    case 0x2:                           /* MIDI 1.0 channel voice */
        if (status < 0x80 || status >= 0xF0)
            break;
        midi_parser_feed(p, status, sink);
        midi_parser_feed(p, d1, sink);
        if (channel_len(status) == 3)
            midi_parser_feed(p, d2, sink);
        break;
    case 0x3: {                         /* SysEx7: 0 complete, 1 start, 2 continue, 3 end */
        int st = (w[0] >> 20) & 0xF;
        int cnt = (w[0] >> 16) & 0xF;
        if (cnt > 6) cnt = 6;
        uint8_t bytes[6] = {
            (uint8_t)(w[0] >> 8), (uint8_t)w[0],
            (uint8_t)(w[1] >> 24), (uint8_t)(w[1] >> 16), (uint8_t)(w[1] >> 8), (uint8_t)w[1],
        };
        if (st == 0 || st == 1)
            midi_parser_feed(p, 0xF0, sink);
        else if (!p->in_sysex)
            break;                      /* continuation of a SysEx we never saw start */
        for (int i = 0; i < cnt; i++)
            midi_parser_feed(p, bytes[i] & 0x7F, sink);
        if (st == 0 || st == 3)
            midi_parser_feed(p, 0xF7, sink);
        break;
    }
    default:                            /* utility, MIDI 2.0, data, stream: not MIDI 1.0 */
        break;
    }
    return n;
}

/* ------------------------------------------------------------------------ */

static void ump_put(ump_builder *b, const uint32_t *w, int n)
{
    if (b->count + n > b->cap) {
        b->overflow = true;
        return;
    }
    for (int i = 0; i < n; i++)
        b->words[b->count++] = w[i];
}

static void ump_message(void *ctx, const uint8_t *m, int len)
{
    ump_builder *b = ctx;
    uint32_t type = m[0] >= 0xF0 ? 0x1 : 0x2;
    uint32_t w = type << 28 | (uint32_t)b->group << 24 | (uint32_t)m[0] << 16 |
                 (uint32_t)(len > 1 ? m[1] : 0) << 8 | (uint32_t)(len > 2 ? m[2] : 0);
    ump_put(b, &w, 1);
}

static void ump_sysex_flush(ump_builder *b, uint32_t st)
{
    uint8_t x[6] = { 0 };
    memcpy(x, b->sx, b->sx_len);
    uint32_t w[2] = {
        0x3u << 28 | (uint32_t)b->group << 24 | st << 20 | (uint32_t)b->sx_len << 16 |
            (uint32_t)x[0] << 8 | x[1],
        (uint32_t)x[2] << 24 | (uint32_t)x[3] << 16 | (uint32_t)x[4] << 8 | x[5],
    };
    ump_put(b, w, 2);
    b->sx_len = 0;
}

static void ump_sysex(void *ctx, uint8_t byte)
{
    ump_builder *b = ctx;
    if (byte == 0xF0) {
        b->sx_len = 0;
        b->sx_started = false;
    } else if (byte == 0xF7) {
        ump_sysex_flush(b, b->sx_started ? 3 : 0);
        b->sx_started = false;
    } else {
        if (b->sx_len == 6) {
            ump_sysex_flush(b, b->sx_started ? 2 : 1);
            b->sx_started = true;
        }
        b->sx[b->sx_len++] = byte;
    }
}

void ump_builder_init(ump_builder *b, uint8_t group, uint32_t *words, int cap)
{
    memset(b, 0, sizeof *b);
    b->group = group & 0x0F;
    b->words = words;
    b->cap = cap;
}

midi_sink ump_builder_sink(ump_builder *b)
{
    return (midi_sink){ ump_message, ump_sysex, b };
}

/* ------------------------------------------------------------------------ */

static const char *cc_name(int cc)
{
    switch (cc) {
    case 0: return "Bank Select";
    case 1: return "Modulation";
    case 7: return "Volume";
    case 10: return "Pan";
    case 11: return "Expression";
    case 32: return "Bank Select LSB";
    case 64: return "Sustain";
    case 66: return "Sostenuto";
    case 67: return "Soft Pedal";
    case 91: return "Reverb";
    case 93: return "Chorus";
    case 120: return "All Sound Off";
    case 121: return "Reset All Controllers";
    case 123: return "All Notes Off";
    default: return NULL;
    }
}

void midi_describe(const uint8_t *m, int len, char *out, size_t cap)
{
    static const char *names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    uint8_t s = m[0];
    int ch = (s & 0x0F) + 1;
    int d1 = len > 1 ? m[1] : 0, d2 = len > 2 ? m[2] : 0;
    const char *n;

    switch (s & 0xF0) {
    case 0x80:
        snprintf(out, cap, "Note Off       ch %-2d %s%d (%d) vel %d", ch, names[d1 % 12], d1 / 12 - 1, d1, d2);
        return;
    case 0x90:
        snprintf(out, cap, "%-14s ch %-2d %s%d (%d) vel %d", d2 ? "Note On" : "Note On/Off", ch,
                 names[d1 % 12], d1 / 12 - 1, d1, d2);
        return;
    case 0xA0:
        snprintf(out, cap, "Poly Pressure  ch %-2d note %d value %d", ch, d1, d2);
        return;
    case 0xB0:
        n = cc_name(d1);
        snprintf(out, cap, "Control Change ch %-2d cc %d%s%s%s value %d", ch, d1,
                 n ? " (" : "", n ? n : "", n ? ")" : "", d2);
        return;
    case 0xC0:
        snprintf(out, cap, "Program Change ch %-2d program %d", ch, d1 + 1);
        return;
    case 0xD0:
        snprintf(out, cap, "Channel Press. ch %-2d value %d", ch, d1);
        return;
    case 0xE0:
        snprintf(out, cap, "Pitch Bend     ch %-2d value %d", ch, (d2 << 7 | d1) - 8192);
        return;
    }
    switch (s) {
    case 0xF1: snprintf(out, cap, "MTC Quarter Frame %d", d1); return;
    case 0xF2: snprintf(out, cap, "Song Position %d", d2 << 7 | d1); return;
    case 0xF3: snprintf(out, cap, "Song Select %d", d1); return;
    case 0xF6: snprintf(out, cap, "Tune Request"); return;
    case 0xF8: snprintf(out, cap, "Clock"); return;
    case 0xFA: snprintf(out, cap, "Start"); return;
    case 0xFB: snprintf(out, cap, "Continue"); return;
    case 0xFC: snprintf(out, cap, "Stop"); return;
    case 0xFE: snprintf(out, cap, "Active Sensing"); return;
    case 0xFF: snprintf(out, cap, "System Reset"); return;
    default: snprintf(out, cap, "Status %02X", s); return;
    }
}
