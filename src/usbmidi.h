/*
 * usbmidi.h - MIDI 1.0 byte stream <-> USB-MIDI 1.0 event packets <-> UMP.
 *
 * Pure conversion code with no I/O, so it can be unit tested on its own
 * (tests/test_usbmidi.c).
 *
 *   device -> host:  USB-MIDI packet -> bytes -> midi_parser -> ump_builder
 *   host -> device:  UMP -> bytes -> midi_parser -> usbmidi_encoder
 *
 * Every direction goes through the same byte-stream parser, so running
 * status, real-time bytes inside SysEx and SysEx cut short by another
 * status byte are handled in one place.
 */
#ifndef UM1X_USBMIDI_H
#define UM1X_USBMIDI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Number of MIDI bytes carried by a USB-MIDI packet, by Code Index Number. */
extern const uint8_t usbmidi_cin_length[16];

/* Number of 32-bit words in a UMP message, by message type (word0 >> 28). */
int ump_word_count(uint32_t word0);

/* ------------------------------------------------------------------------ */
/* MIDI 1.0 byte stream parser. */

typedef struct midi_sink {
    /* A complete non-SysEx message of 1-3 bytes, status byte first. */
    void (*message)(void *ctx, const uint8_t *msg, int len);
    /* SysEx, one byte at a time: F0 starts, data bytes follow, F7 ends.
     * An F7 is synthesised when a SysEx is cut off by another status byte. */
    void (*sysex)(void *ctx, uint8_t byte);
    void *ctx;
} midi_sink;

typedef struct midi_parser {
    uint8_t buf[3];
    uint8_t len;       /* bytes in buf, 0 = idle */
    uint8_t need;      /* length of the message being collected */
    uint8_t running;   /* running status (channel messages only), 0 = none */
    bool in_sysex;
} midi_parser;

void midi_parser_reset(midi_parser *p);
void midi_parser_feed(midi_parser *p, uint8_t byte, const midi_sink *sink);

/* ------------------------------------------------------------------------ */
/* Device -> host: one 4-byte USB-MIDI event packet. */

/* Feeds the MIDI bytes of the packet into parsers[cable] / sinks[cable].
 * Returns the cable number, or -1 when the packet was skipped: empty
 * (padding), reserved CIN 0/1, or a cable outside cable_mask. */
int usbmidi_decode_packet(const uint8_t pkt[4], midi_parser parsers[16],
                          const midi_sink sinks[16], uint16_t cable_mask);

/* ------------------------------------------------------------------------ */
/* Host -> device: MIDI messages to USB-MIDI packets. */

typedef struct usbmidi_encoder {
    uint8_t cable;
    uint8_t sx[3];      /* SysEx bytes not yet sent, carried across calls */
    uint8_t sx_len;
    uint8_t *out;       /* packets are appended here; the caller drains it */
    size_t out_len, out_cap;
    bool overflow;      /* set when a packet did not fit into out */
} usbmidi_encoder;

void usbmidi_encoder_init(usbmidi_encoder *e, uint8_t cable, uint8_t *out, size_t cap);
midi_sink usbmidi_encoder_sink(usbmidi_encoder *e);

/* ------------------------------------------------------------------------ */
/* UMP (MIDI 1.0 protocol). */

/* Feeds the MIDI 1.0 bytes of one UMP message into a parser: system
 * (type 1), MIDI 1.0 channel voice (type 2) and SysEx7 (type 3). Other
 * types are skipped. Returns the number of words consumed, 0 when the
 * message is longer than avail. */
int ump_to_midi(const uint32_t *words, int avail, midi_parser *p, const midi_sink *sink);

typedef struct ump_builder {
    uint8_t group;
    uint8_t sx[6];      /* SysEx bytes not yet sent, carried across calls */
    uint8_t sx_len;
    bool sx_started;
    uint32_t *words;    /* messages are appended here; the caller drains it */
    int count, cap;
    bool overflow;
} ump_builder;

void ump_builder_init(ump_builder *b, uint8_t group, uint32_t *words, int cap);
midi_sink ump_builder_sink(ump_builder *b);

/* ------------------------------------------------------------------------ */
/* Human-readable description of a MIDI message, for the monitor. */
void midi_describe(const uint8_t *msg, int len, char *out, size_t cap);

#endif
