/*
 * test_usbmidi.c - unit tests for the packet conversion (make test).
 */
#include "../src/usbmidi.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ------------------------------------------------------------------------ */
/* device -> host: USB packets -> UMP */

typedef struct in_rig {
    midi_parser parsers[16];
    midi_sink sinks[16];
    ump_builder b[16];
    uint32_t words[16][256];
} in_rig;

static void in_rig_init(in_rig *r)
{
    memset(r, 0, sizeof *r);
    for (int c = 0; c < 16; c++) {
        ump_builder_init(&r->b[c], 0, r->words[c], 256);
        r->sinks[c] = ump_builder_sink(&r->b[c]);
    }
}

static void in_feed(in_rig *r, const uint8_t *pk, int len, uint16_t mask)
{
    for (int i = 0; i + 3 < len; i += 4)
        usbmidi_decode_packet(pk + i, r->parsers, r->sinks, mask);
}

static void test_in_channel(void)
{
    in_rig r;
    in_rig_init(&r);
    const uint8_t pk[] = {
        0x09, 0x90, 0x3C, 0x50,     /* note on */
        0x00, 0x00, 0x00, 0x00,     /* padding */
        0x0B, 0xB0, 0x40, 0x7F,     /* sustain down */
        0x0C, 0xC0, 0x05, 0x00,     /* program change */
        0x0E, 0xE0, 0x00, 0x40,     /* pitch bend centre */
        0x00, 0x00, 0x00, 0x00,
    };
    in_feed(&r, pk, sizeof pk, 0x0001);
    CHECK(r.b[0].count == 4);
    CHECK(r.words[0][0] == 0x20903C50);
    CHECK(r.words[0][1] == 0x20B0407F);
    CHECK(r.words[0][2] == 0x20C00500);
    CHECK(r.words[0][3] == 0x20E00040);
}

static void test_in_running_status_packet(void)
{
    in_rig r;
    in_rig_init(&r);
    const uint8_t pk[] = {
        0x09, 0x90, 0x3C, 0x50,
        0x09, 0x3E, 0x51, 0x00,     /* status left out by the firmware */
    };
    in_feed(&r, pk, sizeof pk, 0x0001);
    CHECK(r.b[0].count == 2);
    CHECK(r.words[0][1] == 0x20903E51);
}

static void test_in_realtime_and_cable_mask(void)
{
    in_rig r;
    in_rig_init(&r);
    const uint8_t pk[] = {
        0x0F, 0xF8, 0x00, 0x00,     /* clock */
        0x0F, 0xFE, 0x00, 0x00,     /* active sensing */
        0x19, 0x90, 0x3C, 0x50,     /* cable 1: not in mask */
        0x05, 0xF6, 0x00, 0x00,     /* tune request */
        0x03, 0xF2, 0x10, 0x20,     /* song position */
        0x01, 0x12, 0x34, 0x56,     /* reserved CIN 1 */
    };
    in_feed(&r, pk, sizeof pk, 0x0001);
    CHECK(r.b[0].count == 4);
    CHECK(r.words[0][0] == 0x10F80000);
    CHECK(r.words[0][1] == 0x10FE0000);
    CHECK(r.words[0][2] == 0x10F60000);
    CHECK(r.words[0][3] == 0x10F21020);
    CHECK(r.b[1].count == 0);
}

static void test_in_sysex(void)
{
    in_rig r;
    in_rig_init(&r);
    /* Roland DT1: F0 41 10 42 12 40 00 7F F7 = 7 data bytes */
    const uint8_t pk[] = {
        0x04, 0xF0, 0x41, 0x10,
        0x04, 0x42, 0x12, 0x40,
        0x07, 0x00, 0x7F, 0xF7,
    };
    in_feed(&r, pk, sizeof pk, 0x0001);
    CHECK(r.b[0].count == 4);
    CHECK(r.words[0][0] == 0x30164110);     /* start, 6 bytes */
    CHECK(r.words[0][1] == 0x42124000);
    CHECK(r.words[0][2] == 0x30317F00);     /* end, 1 byte */
    CHECK(r.words[0][3] == 0x00000000);
}

static void test_in_sysex_split_across_transfers_with_realtime(void)
{
    in_rig r;
    in_rig_init(&r);
    const uint8_t t1[] = { 0x04, 0xF0, 0x01, 0x02 };
    const uint8_t t2[] = { 0x0F, 0xF8, 0x00, 0x00, 0x06, 0x03, 0xF7, 0x00 };
    in_feed(&r, t1, sizeof t1, 1);
    CHECK(r.b[0].count == 0);               /* nothing complete yet */
    in_feed(&r, t2, sizeof t2, 1);
    CHECK(r.b[0].count == 3);
    CHECK(r.words[0][0] == 0x10F80000);     /* clock goes out at once */
    CHECK(r.words[0][1] == 0x30030102);     /* complete, 3 bytes */
    CHECK(r.words[0][2] == 0x03000000);
}

static void test_in_short_sysex(void)
{
    in_rig r;
    in_rig_init(&r);
    const uint8_t pk[] = { 0x06, 0xF0, 0xF7, 0x00, 0x07, 0xF0, 0x7D, 0xF7 };
    in_feed(&r, pk, sizeof pk, 1);
    CHECK(r.b[0].count == 4);
    CHECK(r.words[0][0] == 0x30000000);     /* empty SysEx */
    CHECK(r.words[0][2] == 0x30017D00);
}

/* ------------------------------------------------------------------------ */
/* byte stream parser */

typedef struct collect {
    uint8_t bytes[1024];
    int n;
    int messages;
} collect;

static void col_msg(void *ctx, const uint8_t *m, int len)
{
    collect *c = ctx;
    memcpy(c->bytes + c->n, m, (size_t)len);
    c->n += len;
    c->messages++;
}

static void col_sx(void *ctx, uint8_t b)
{
    collect *c = ctx;
    c->bytes[c->n++] = b;
    if (b == 0xF7)
        c->messages++;
}

static void test_parser_running_status(void)
{
    collect c = { 0 };
    midi_sink s = { col_msg, col_sx, &c };
    midi_parser p;
    midi_parser_reset(&p);
    const uint8_t in[] = { 0x90, 0x3C, 0x40, 0x3E, 0x40, 0xF8, 0x40, 0x00, 0xC0, 0x01, 0x02 };
    for (size_t i = 0; i < sizeof in; i++)
        midi_parser_feed(&p, in[i], &s);
    const uint8_t want[] = { 0x90, 0x3C, 0x40, 0x90, 0x3E, 0x40, 0xF8, 0x90, 0x40, 0x00,
                             0xC0, 0x01, 0xC0, 0x02 };
    CHECK(c.messages == 6);
    CHECK(c.n == (int)sizeof want && memcmp(c.bytes, want, sizeof want) == 0);
}

static void test_parser_sysex_cut_short(void)
{
    collect c = { 0 };
    midi_sink s = { col_msg, col_sx, &c };
    midi_parser p;
    midi_parser_reset(&p);
    const uint8_t in[] = { 0xF0, 0x01, 0x90, 0x3C, 0x40, 0xF7, 0x05, 0xF1, 0x23 };
    for (size_t i = 0; i < sizeof in; i++)
        midi_parser_feed(&p, in[i], &s);
    /* SysEx ended by the note on, stray F7 and orphan data dropped */
    const uint8_t want[] = { 0xF0, 0x01, 0xF7, 0x90, 0x3C, 0x40, 0xF1, 0x23 };
    CHECK(c.n == (int)sizeof want && memcmp(c.bytes, want, sizeof want) == 0);
}

/* ------------------------------------------------------------------------ */
/* host -> device: UMP -> USB packets */

static size_t encode(const uint32_t *w, int n, uint8_t *out, size_t cap, midi_parser *p, usbmidi_encoder *e)
{
    e->out = out;
    e->out_len = 0;
    e->out_cap = cap;
    midi_sink s = usbmidi_encoder_sink(e);
    for (int i = 0; i < n;) {
        int k = ump_to_midi(w + i, n - i, p, &s);
        if (k == 0)
            break;
        i += k;
    }
    return e->out_len;
}

static void test_out_messages(void)
{
    uint8_t out[256];
    midi_parser p;
    midi_parser_reset(&p);
    usbmidi_encoder e;
    usbmidi_encoder_init(&e, 0, NULL, 0);
    const uint32_t w[] = {
        0x20903C64, 0x20C00500, 0x20D04000, 0x10F80000, 0x10F60000, 0x10F21020, 0x10F30700,
        0x00200000,                 /* utility (JR timestamp): skipped */
        0x40903C00, 0x80000000,     /* MIDI 2.0 note on: skipped */
    };
    size_t n = encode(w, sizeof w / 4, out, sizeof out, &p, &e);
    const uint8_t want[] = {
        0x09, 0x90, 0x3C, 0x64, 0x0C, 0xC0, 0x05, 0x00, 0x0D, 0xD0, 0x40, 0x00,
        0x0F, 0xF8, 0x00, 0x00, 0x05, 0xF6, 0x00, 0x00, 0x03, 0xF2, 0x10, 0x20,
        0x02, 0xF3, 0x07, 0x00,
    };
    CHECK(n == sizeof want && memcmp(out, want, sizeof want) == 0);
}

static void test_out_sysex(void)
{
    uint8_t out[256];
    midi_parser p;
    midi_parser_reset(&p);
    usbmidi_encoder e;
    usbmidi_encoder_init(&e, 3, NULL, 0);
    /* identity request F0 7E 7F 06 01 F7 as one complete SysEx7 packet */
    const uint32_t w[] = { 0x30047E7F, 0x06010000 };
    size_t n = encode(w, 2, out, sizeof out, &p, &e);
    const uint8_t want[] = { 0x34, 0xF0, 0x7E, 0x7F, 0x37, 0x06, 0x01, 0xF7 };
    CHECK(n == sizeof want && memcmp(out, want, sizeof want) == 0);

    /* start / continue lost: a continuation without a start is dropped */
    midi_parser_reset(&p);
    const uint32_t orphan[] = { 0x30260102, 0x03040506 };
    CHECK(encode(orphan, 2, out, sizeof out, &p, &e) == 0);

    /* every end length: F0 F7, F0 x F7, F0 x y F7 */
    const uint32_t e0[] = { 0x30000000, 0 }, e1[] = { 0x30010100, 0 }, e2[] = { 0x30020102, 0 };
    CHECK(encode(e0, 2, out, sizeof out, &p, &e) == 4 && out[0] == 0x36 && out[1] == 0xF0 && out[2] == 0xF7);
    CHECK(encode(e1, 2, out, sizeof out, &p, &e) == 4 && out[0] == 0x37 && out[3] == 0xF7);
    CHECK(encode(e2, 2, out, sizeof out, &p, &e) == 8 && out[0] == 0x34 && out[4] == 0x35 && out[5] == 0xF7);
}

static void test_out_overflow_flag(void)
{
    uint8_t out[4];
    midi_parser p;
    midi_parser_reset(&p);
    usbmidi_encoder e;
    usbmidi_encoder_init(&e, 0, NULL, 0);
    const uint32_t w[] = { 0x20903C64, 0x20803C00 };
    CHECK(encode(w, 2, out, sizeof out, &p, &e) == 4);
    CHECK(e.overflow);
}

/* ------------------------------------------------------------------------ */
/* round trip: random MIDI -> UMP -> USB -> UMP must give the same UMP */

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static int split(const uint32_t *w, int n, bool rt, uint32_t *out)
{
    int k = 0;
    for (int i = 0; i < n;) {
        int c = ump_word_count(w[i]);
        bool is_rt = (w[i] >> 28) == 1 && ((w[i] >> 16) & 0xFF) >= 0xF8;
        if (is_rt == rt)
            for (int j = 0; j < c; j++)
                out[k++] = w[i + j];
        i += c;
    }
    return k;
}

static bool split_equal(const uint32_t *a, int na, const uint32_t *b, int nb)
{
    static uint32_t x[8192], y[8192];
    for (int rt = 0; rt < 2; rt++) {
        int nx = split(a, na, rt, x), ny = split(b, nb, rt, y);
        if (nx != ny || memcmp(x, y, (size_t)nx * 4) != 0)
            return false;
    }
    return true;
}

static void test_round_trip(void)
{
    for (int round = 0; round < 200; round++) {
        /* random byte stream of valid messages with interleaved real-time */
        uint8_t stream[4096];
        int n = 0;
        while (n < 3000) {
            int kind = (int)(rnd() % 10);
            if (kind < 5) {
                uint8_t st = (uint8_t)(0x80 | (rnd() % 7) << 4 | (rnd() % 16));
                stream[n++] = st;
                stream[n++] = rnd() & 0x7F;
                if ((st & 0xF0) != 0xC0 && (st & 0xF0) != 0xD0)
                    stream[n++] = rnd() & 0x7F;
            } else if (kind < 7) {
                stream[n++] = (uint8_t[]){ 0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0xFF }[rnd() % 6];
            } else {
                int len = (int)(rnd() % 40);
                stream[n++] = 0xF0;
                for (int i = 0; i < len; i++) {
                    stream[n++] = rnd() & 0x7F;
                    if (rnd() % 17 == 0)
                        stream[n++] = 0xF8;     /* clock inside SysEx */
                }
                stream[n++] = 0xF7;
            }
        }

        /* bytes -> UMP (reference) */
        static uint32_t ref[8192], got[8192];
        ump_builder b;
        ump_builder_init(&b, 0, ref, 8192);
        midi_parser p;
        midi_parser_reset(&p);
        midi_sink s = ump_builder_sink(&b);
        for (int i = 0; i < n; i++)
            midi_parser_feed(&p, stream[i], &s);
        int nref = b.count;

        /* UMP -> USB packets */
        static uint8_t usb[16384];
        midi_parser p2;
        midi_parser_reset(&p2);
        usbmidi_encoder e;
        usbmidi_encoder_init(&e, 0, NULL, 0);
        size_t nusb = encode(ref, nref, usb, sizeof usb, &p2, &e);
        CHECK(!e.overflow);

        /* USB packets -> UMP, in random-sized transfers with padding */
        in_rig *r = malloc(sizeof *r);
        in_rig_init(r);
        ump_builder_init(&r->b[0], 0, got, 8192);
        r->sinks[0] = ump_builder_sink(&r->b[0]);
        for (size_t off = 0; off < nusb;) {
            size_t take = 4 * (1 + rnd() % 16);
            if (off + take > nusb)
                take = nusb - off;
            uint8_t xfer[128] = { 0 };
            memcpy(xfer, usb + off, take);
            in_feed(r, xfer, (int)take + 8, 1);     /* + two padding packets */
            off += take;
        }
        /* Real-time messages may move by a packet or two relative to the
         * SysEx bytes around them (legal: they can go anywhere), so the
         * real-time and the other messages are compared as two streams. */
        CHECK(r->b[0].count == nref);
        CHECK(split_equal(got, r->b[0].count, ref, nref));
        free(r);
        if (failures)
            break;
    }
}

int main(void)
{
    test_in_channel();
    test_in_running_status_packet();
    test_in_realtime_and_cable_mask();
    test_in_sysex();
    test_in_sysex_split_across_transfers_with_realtime();
    test_in_short_sysex();
    test_parser_running_status();
    test_parser_sysex_cut_short();
    test_out_messages();
    test_out_sysex();
    test_out_overflow_flag();
    test_round_trip();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
