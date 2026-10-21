/*
 * tools.c - diagnostic commands. See tools.h.
 */
#include "tools.h"

#include "devices.h"
#include "usbmidi.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <dispatch/dispatch.h>
#include <libusb.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ------------------------------------------------------------------------ */
/* CoreMIDI helpers */

static void endpoint_string(MIDIObjectRef obj, CFStringRef prop, char *out, size_t cap)
{
    CFStringRef s = NULL;
    out[0] = 0;
    if (MIDIObjectGetStringProperty(obj, prop, &s) == noErr && s) {
        CFStringGetCString(s, out, (CFIndex)cap, kCFStringEncodingUTF8);
        CFRelease(s);
    }
}

static void endpoint_name(MIDIEndpointRef ep, char *out, size_t cap)
{
    endpoint_string(ep, kMIDIPropertyDisplayName, out, cap);
    if (!out[0])
        endpoint_string(ep, kMIDIPropertyName, out, cap);
}

/* No pattern: the ports this driver creates (names from the device table). */
static bool name_matches(const char *name, const char *pattern)
{
    if (pattern && *pattern)
        return strcasestr(name, pattern) != NULL;
    for (int i = 0; i < um_model_count; i++)
        if (strncmp(name, um_models[i].name, strlen(um_models[i].name)) == 0)
            return true;
    return false;
}

static MIDIEndpointRef find_destination(const char *pattern, char *name, size_t cap)
{
    ItemCount n = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; i++) {
        MIDIEndpointRef d = MIDIGetDestination(i);
        endpoint_name(d, name, cap);
        if (name_matches(name, pattern))
            return d;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* list, devices */

static void list_endpoints(const char *title, ItemCount n, MIDIEndpointRef (*get)(ItemCount))
{
    printf("\nCoreMIDI %s:\n", title);
    if (n == 0)
        printf("  (none)\n");
    for (ItemCount i = 0; i < n; i++) {
        MIDIEndpointRef ep = get(i);
        char name[128], manu[64], model[64];
        SInt32 uid = 0, offline = 0;
        endpoint_name(ep, name, sizeof name);
        endpoint_string(ep, kMIDIPropertyManufacturer, manu, sizeof manu);
        endpoint_string(ep, kMIDIPropertyModel, model, sizeof model);
        MIDIObjectGetIntegerProperty(ep, kMIDIPropertyUniqueID, &uid);
        MIDIObjectGetIntegerProperty(ep, kMIDIPropertyOffline, &offline);
        printf("  %-28s %-10s %-14s id %-11d %s\n", name, manu, model, (int)uid, offline ? "OFFLINE" : "online");
    }
}

int tool_list(void)
{
    libusb_context *ctx;
    if (libusb_init_context(&ctx, NULL, 0) != 0) {
        fprintf(stderr, "libusb init failed\n");
        return 1;
    }
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    int found = 0;
    printf("USB devices:\n");
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0)
            continue;
        const um_model *m = um_find_model(d.idVendor, d.idProduct);
        if (!m && d.idVendor != 0x0582)
            continue;
        uint8_t ports[8];
        int np = libusb_get_port_numbers(list[i], ports, sizeof ports);
        char where[32];
        int w = snprintf(where, sizeof where, "%d", libusb_get_bus_number(list[i]));
        for (int k = 0; k < np; k++)
            w += snprintf(where + w, sizeof where - (size_t)w, "%c%d", k ? '.' : '-', ports[k]);
        printf("  %04x:%04x at USB %-8s %s%s\n", d.idVendor, d.idProduct, where,
               m ? m->name : "Roland device not in the table (class compliant ones need no driver)",
               m && !m->tested ? "  [untested]" : "");
        found++;
    }
    if (!found)
        printf("  (no Roland/Edirol device found)\n");
    if (n >= 0)
        libusb_free_device_list(list, 1);
    libusb_exit(ctx);

    list_endpoints("sources (MIDI in to apps)", MIDIGetNumberOfSources(), MIDIGetSource);
    list_endpoints("destinations (MIDI out from apps)", MIDIGetNumberOfDestinations(), MIDIGetDestination);
    return 0;
}

int tool_devices(void)
{
    printf("%-9s %-16s %-9s %5s %5s  %s\n", "USB ID", "port name", "interface", "out", "in", "status");
    for (int i = 0; i < um_model_count; i++) {
        const um_model *m = &um_models[i];
        printf("%04x:%04x %-16s %-9d %5d %5d  %s\n", m->vid, m->pid, m->name, m->iface,
               __builtin_popcount(m->out_cables), __builtin_popcount(m->in_cables),
               m->tested ? "tested" : "untested");
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* monitor */

#define MON_MAX_SOURCES 64
#define MON_SYSEX_MAX   65536

typedef struct mon_source {
    MIDIEndpointRef ep;
    char name[64];
    midi_parser parser;
    uint8_t *sysex;
    size_t sysex_len;
} mon_source;

static struct {
    MIDIClientRef client;
    MIDIPortRef port;
    const char *pattern;
    bool show_all, raw;
    uint64_t start;
    mach_timebase_info_data_t tb;
    mon_source src[MON_MAX_SOURCES];
    int nsrc;
    unsigned long hidden_clock, hidden_sensing, messages;
    dispatch_queue_t q;                 /* serialises printing */
} mon;

static double mon_time(MIDITimeStamp ts)
{
    if (!ts || ts < mon.start)
        ts = mach_absolute_time();
    return (double)(ts - mon.start) * mon.tb.numer / mon.tb.denom / 1e9;
}

typedef struct mon_ctx {
    mon_source *s;
    double t;
} mon_ctx;

static void mon_message(void *ctx, const uint8_t *m, int len)
{
    mon_ctx *c = ctx;
    if (!mon.show_all && m[0] == 0xF8) { mon.hidden_clock++; return; }
    if (!mon.show_all && m[0] == 0xFE) { mon.hidden_sensing++; return; }
    char desc[128];
    midi_describe(m, len, desc, sizeof desc);
    printf("%9.3f  %-12s %-52s", c->t, c->s->name, desc);
    for (int i = 0; i < len; i++)
        printf(" %02X", m[i]);
    printf("\n");
    mon.messages++;
}

static void mon_sysex(void *ctx, uint8_t b)
{
    mon_ctx *c = ctx;
    mon_source *s = c->s;
    if (b == 0xF0)
        s->sysex_len = 0;
    if (s->sysex_len < MON_SYSEX_MAX)
        s->sysex[s->sysex_len++] = b;
    if (b != 0xF7)
        return;
    printf("%9.3f  %-12s SysEx, %zu bytes%-37s", c->t, s->name, s->sysex_len, "");
    size_t show = s->sysex_len <= 48 ? s->sysex_len : 40;
    for (size_t i = 0; i < show; i++)
        printf(" %02X", s->sysex[i]);
    if (show < s->sysex_len)
        printf(" ... %02X", s->sysex[s->sysex_len - 1]);
    printf("\n");
    mon.messages++;
}

static void mon_receive(const MIDIEventList *el, void *ref)
{
    mon_source *s = ref;
    dispatch_sync(mon.q, ^{
        const MIDIEventPacket *p = &el->packet[0];
        for (UInt32 i = 0; i < el->numPackets; i++) {
            mon_ctx c = { s, mon_time(p->timeStamp) };
            if (mon.raw) {
                printf("%9.3f  %-12s UMP", c.t, s->name);
                for (UInt32 w = 0; w < p->wordCount; w++)
                    printf(" %08X", p->words[w]);
                printf("\n");
            }
            midi_sink sink = { mon_message, mon_sysex, &c };
            for (UInt32 w = 0; w < p->wordCount;) {
                int n = ump_to_midi(&p->words[w], (int)(p->wordCount - w), &s->parser, &sink);
                if (n == 0)
                    break;
                w += (UInt32)n;
            }
            p = MIDIEventPacketNext(p);
        }
        fflush(stdout);
    });
}

static void mon_connect(void)
{
    ItemCount n = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < n; i++) {
        MIDIEndpointRef ep = MIDIGetSource(i);
        char name[64];
        endpoint_name(ep, name, sizeof name);
        if (!name_matches(name, mon.pattern))
            continue;
        bool known = false;
        for (int k = 0; k < mon.nsrc; k++)
            if (mon.src[k].ep == ep)
                known = true;
        if (known || mon.nsrc == MON_MAX_SOURCES)
            continue;
        mon_source *s = &mon.src[mon.nsrc++];
        s->ep = ep;
        snprintf(s->name, sizeof s->name, "%s", name);
        s->sysex = malloc(MON_SYSEX_MAX);
        if (MIDIPortConnectSource(mon.port, ep, s) == noErr)
            fprintf(stderr, "monitoring \"%s\"\n", name);
    }
}

static void mon_summary_and_exit(void)
{
    dispatch_sync(mon.q, ^{
        fprintf(stderr, "\n%lu messages", mon.messages);
        if (mon.hidden_clock || mon.hidden_sensing)
            fprintf(stderr, "; hidden: %lu clock, %lu active sensing (show with -a)",
                    mon.hidden_clock, mon.hidden_sensing);
        fprintf(stderr, "\n");
    });
    exit(0);
}

int tool_monitor(const char *pattern, bool show_all, bool raw, double seconds)
{
    mon.pattern = pattern;
    mon.show_all = show_all;
    mon.raw = raw;
    mach_timebase_info(&mon.tb);
    mon.start = mach_absolute_time();
    mon.q = dispatch_queue_create("monitor", NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);

    OSStatus st = MIDIClientCreateWithBlock(CFSTR("um1x monitor"), &mon.client, ^(const MIDINotification *n) {
        if (n->messageID == kMIDIMsgObjectAdded || n->messageID == kMIDIMsgSetupChanged)
            mon_connect();
    });
    if (st == noErr)
        st = MIDIInputPortCreateWithProtocol(mon.client, CFSTR("monitor"), kMIDIProtocol_1_0, &mon.port,
                                             ^(const MIDIEventList *el, void *ref) { mon_receive(el, ref); });
    if (st != noErr) {
        fprintf(stderr, "CoreMIDI error %d\n", (int)st);
        return 1;
    }
    mon_connect();
    if (mon.nsrc == 0)
        fprintf(stderr, "no matching source yet%s%s%s; waiting for one to appear\n",
                pattern ? " (\"" : "", pattern ? pattern : "", pattern ? "\")" : "");

    signal(SIGINT, SIG_IGN);
    dispatch_source_t sig = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(sig, ^{ mon_summary_and_exit(); });
    dispatch_resume(sig);
    signal(SIGTERM, SIG_IGN);
    dispatch_source_t term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(term, ^{ mon_summary_and_exit(); });
    dispatch_resume(term);
    if (seconds > 0)
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{ mon_summary_and_exit(); });
    CFRunLoopRun();                     /* CoreMIDI notifications need a run loop */
    return 0;
}

/* ------------------------------------------------------------------------ */
/* send, test-out */

typedef struct out_conn {
    MIDIClientRef client;
    MIDIPortRef port;
    MIDIEndpointRef dest;
    char name[128];
} out_conn;

static bool out_open(out_conn *c, const char *pattern)
{
    if (MIDIClientCreateWithBlock(CFSTR("um1x send"), &c->client, NULL) != noErr ||
        MIDIOutputPortCreate(c->client, CFSTR("send"), &c->port) != noErr) {
        fprintf(stderr, "CoreMIDI error\n");
        return false;
    }
    c->dest = find_destination(pattern, c->name, sizeof c->name);
    if (!c->dest) {
        fprintf(stderr, "no MIDI destination matching \"%s\" (is the driver running and the device plugged in?)\n",
                pattern ? pattern : "UM-1X");
        return false;
    }
    return true;
}

/* MIDI bytes -> UMP -> MIDISendEventList, in lists of up to 64 words. */
static void out_send_bytes(out_conn *c, const uint8_t *bytes, int n, MIDITimeStamp ts)
{
    uint32_t words[128];
    ump_builder b;
    ump_builder_init(&b, 0, words, 128);
    midi_parser p;
    midi_parser_reset(&p);
    midi_sink sink = ump_builder_sink(&b);
    union { MIDIEventList list; uint8_t raw[sizeof(MIDIEventList) + 128 * 4 + 64]; } u;

    for (int i = 0; i <= n; i++) {
        if (i < n)
            midi_parser_feed(&p, bytes[i], &sink);
        if (b.count < 64 && i < n)
            continue;
        MIDIEventPacket *pk = MIDIEventListInit(&u.list, kMIDIProtocol_1_0);
        for (int w = 0; w < b.count;) {
            int k = ump_word_count(words[w]);
            MIDIEventPacket *next = MIDIEventListAdd(&u.list, sizeof u, pk, ts, (ByteCount)k, &words[w]);
            if (!next) {
                MIDISendEventList(c->port, c->dest, &u.list);
                pk = MIDIEventListInit(&u.list, kMIDIProtocol_1_0);
                next = MIDIEventListAdd(&u.list, sizeof u, pk, ts, (ByteCount)k, &words[w]);
            }
            pk = next;
            w += k;
        }
        if (u.list.numPackets)
            MIDISendEventList(c->port, c->dest, &u.list);
        b.count = 0;
    }
}

int tool_parse_hex(const char *s, uint8_t *out)
{
    char *end;
    long v = strtol(s, &end, 16);
    if (*s == 0 || *end != 0 || v < 0 || v > 255)
        return -1;
    *out = (uint8_t)v;
    return 0;
}

int tool_send(const char *pattern, const uint8_t *bytes, int n, int delay_ms)
{
    out_conn c;
    if (!out_open(&c, pattern))
        return 1;
    MIDITimeStamp ts = 0;
    if (delay_ms > 0) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        ts = mach_absolute_time() + (MIDITimeStamp)delay_ms * 1000000ull * tb.denom / tb.numer;
    }
    out_send_bytes(&c, bytes, n, ts);
    printf("sent %d byte%s to \"%s\"", n, n == 1 ? "" : "s", c.name);
    if (delay_ms > 0)
        printf(", stamped %d ms ahead", delay_ms);
    printf("\n");
    /* Stay until it is out: CoreMIDI may drop what a client scheduled ahead
     * when the client goes away first. */
    usleep((useconds_t)(200 + (delay_ms > 0 ? delay_ms : 0)) * 1000);
    return 0;
}

/* Plays a short phrase. All events are sent at once with future time
 * stamps, so this also exercises the driver's output scheduling. */
int tool_test_out(const char *pattern)
{
    out_conn c;
    if (!out_open(&c, pattern))
        return 1;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    MIDITimeStamp t0 = mach_absolute_time() + 100ull * 1000000 * tb.denom / tb.numer;
#define AT(ms) (t0 + (MIDITimeStamp)(ms) * 1000000ull * tb.denom / tb.numer)
    static const uint8_t notes[] = { 60, 64, 67, 72, 76, 79, 84 };
    int ms = 0;
    for (size_t i = 0; i < sizeof notes; i++, ms += 180) {
        uint8_t on[3] = { 0x90, notes[i], 80 }, off[3] = { 0x80, notes[i], 0 };
        out_send_bytes(&c, on, 3, AT(ms));
        out_send_bytes(&c, off, 3, AT(ms + 160));
    }
    ms += 200;
    uint8_t ped_down[3] = { 0xB0, 64, 127 }, ped_up[3] = { 0xB0, 64, 0 };
    uint8_t chord_on[] = { 0x90, 48, 70, 55, 70, 60, 70, 64, 70 };   /* running status */
    uint8_t chord_off[] = { 0x80, 48, 0, 55, 0, 60, 0, 64, 0 };
    out_send_bytes(&c, ped_down, 3, AT(ms));
    out_send_bytes(&c, chord_on, sizeof chord_on, AT(ms + 20));
    out_send_bytes(&c, chord_off, sizeof chord_off, AT(ms + 300));  /* held by the pedal */
    out_send_bytes(&c, ped_up, 3, AT(ms + 2000));
#undef AT
    printf("playing a test phrase on \"%s\": arpeggio C4-C6, then a sustained C chord (~3.5 s)\n", c.name);
    usleep((useconds_t)(ms + 2400) * 1000);
    return 0;
}
