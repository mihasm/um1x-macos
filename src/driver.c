/*
 * driver.c - the resident driver.
 *
 * Threads:
 *   main queue   signals (quit, reconnect) and system sleep/wake notifications
 *   usb thread   libusb event loop: hot-plug, opening/closing devices, IN
 *                transfer completions (converted to UMP and handed to CoreMIDI
 *                right in the completion callback)
 *   out thread   one per device: takes UMP from the CoreMIDI destination's
 *                receive block, holds events stamped in the future until they
 *                are due, encodes USB-MIDI packets and writes them
 *
 * A "slot" is one physical device position. Its CoreMIDI endpoints are
 * created the first time a device fills it and live until the driver exits,
 * so apps keep their connections across unplug/replug and sleep; while the
 * device is away the endpoints are marked offline.
 */
#include "driver.h"

#include "devices.h"
#include "log.h"
#include "usbmidi.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <libusb.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define MAX_SLOTS        8
#define IN_TRANSFERS     4        /* IN transfers kept in flight per device */
#define OUT_TIMEOUT_MS   2000
#define RETRY_MS         2000     /* after a failed open */
#define SAFETY_SCAN_MS   30000    /* rescan now and then even with hot-plug */
#define QUEUE_MAX        (1u << 20)
#define OUT_BATCH        256
#define OUT_LEAD_NS      500000   /* send events due within 0.5 ms now (USB frame latency) */
#define ADVANCE_US       2000     /* CoreMIDI hands us future events this early */
#define IN_WORDS         64

enum { ST_CLOSED, ST_OPEN, ST_CLOSING };

typedef struct out_item {
    uint64_t ts, seq;
    uint32_t w[4];
    uint8_t n, cable;
} out_item;

typedef struct slot slot;

typedef struct in_cable {
    slot *s;
    ump_builder ump;
    uint32_t words[IN_WORDS];
} in_cable;

struct slot {
    const um_model *model;
    int instance;
    char name[64];
    char location[32];               /* last USB port path, for reconnects */
    MIDIEndpointRef src[16], dst[16];

    /* USB state, guarded by m */
    pthread_mutex_t m;
    pthread_cond_t cv;
    int state;
    libusb_device *dev;
    libusb_device_handle *h;
    int alt;
    uint8_t ep_in, ep_out;
    bool in_int, out_int;
    int in_size, out_size;
    struct libusb_transfer *in_x[IN_TRANSFERS];
    int in_active;
    struct libusb_transfer *out_x;
    bool out_busy;
    enum libusb_transfer_status out_status;
    atomic_bool online;
    atomic_bool need_close;

    /* device -> host conversion (usb thread only) */
    midi_parser in_parsers[16];
    midi_sink in_sinks[16];
    in_cable in_c[16];

    /* host -> device queue, guarded by qm */
    pthread_t out_thread;
    pthread_mutex_t qm;
    pthread_cond_t qcv;
    out_item *heap;
    size_t qlen, qcap;
    uint64_t seq;
    bool quit, q_reset, q_full_logged;

    /* host -> device conversion (out thread only) */
    midi_parser out_parsers[16];
    usbmidi_encoder enc[16];
};

typedef struct usb_open_info {
    libusb_device_handle *h;
    int alt;
    uint8_t ep_in, ep_out;
    bool in_int, out_int;
    int in_size, out_size;
} usb_open_info;

static struct {
    driver_options opt;
    libusb_context *ctx;
    MIDIClientRef client;
    slot *slots[MAX_SLOTS];
    int nslots;
    bool hotplug;
    libusb_hotplug_callback_handle hotplug_handle;
    pthread_t usb_thread;
    atomic_bool quit, arrived, reconnect_all, sleeping, sleep_req, woke;
    dispatch_semaphore_t sleep_done;
    io_connect_t power_port;
    IONotificationPortRef power_notify;
    io_object_t power_notifier;
    mach_timebase_info_data_t tb;
    char last_failure[256];
    int lock_fd;
} g;

/* ------------------------------------------------------------------------ */
/* helpers */

static uint64_t now_ms(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}

static uint64_t ticks_to_ns(uint64_t t)
{
    return t * g.tb.numer / g.tb.denom;
}

static uint64_t ns_to_ticks(uint64_t ns)
{
    return ns * g.tb.denom / g.tb.numer;
}

static void set_thread(const char *name)
{
    pthread_setname_np(name);
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

static SInt32 unique_id(const slot *s, int cable, char dir)
{
    char key[96];
    snprintf(key, sizeof key, "um1x-macos/%04x:%04x/%d/%d/%c", s->model->vid, s->model->pid,
             s->instance, cable, dir);
    uint32_t h = 2166136261u;                       /* FNV-1a */
    for (const char *p = key; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    return (SInt32)(h & 0x7fffffff) | 0x10000000;  /* positive, never 0 */
}

static int cable_count(uint16_t mask)
{
    return __builtin_popcount(mask);
}

static void port_name(const slot *s, int cable, uint16_t mask, char *out, size_t cap)
{
    if (cable_count(mask) <= 1)
        snprintf(out, cap, "%s", s->name);
    else
        snprintf(out, cap, "%s %d", s->name, cable + 1);
}

static void location_of(libusb_device *dev, char *out, size_t cap)
{
    uint8_t ports[8];
    int n = libusb_get_port_numbers(dev, ports, sizeof ports);
    int w = snprintf(out, cap, "%d", libusb_get_bus_number(dev));
    for (int i = 0; i < n && w > 0 && (size_t)w < cap; i++)
        w += snprintf(out + w, cap - (size_t)w, "%c%d", i ? '.' : '-', ports[i]);
}

static void log_failure(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_failure(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (strcmp(buf, g.last_failure) != 0) {   /* say it once, not every retry */
        log_msg("%s", buf);
        snprintf(g.last_failure, sizeof g.last_failure, "%s", buf);
    }
}

/* ------------------------------------------------------------------------ */
/* host -> device: queue, scheduling, encoding */

static bool item_before(const out_item *a, const out_item *b)
{
    return a->ts < b->ts || (a->ts == b->ts && a->seq < b->seq);
}

static bool heap_push(slot *s, const out_item *it)
{
    if (s->qlen == s->qcap) {
        size_t cap = s->qcap ? s->qcap * 2 : 1024;
        if (cap > QUEUE_MAX)
            return false;
        out_item *h = realloc(s->heap, cap * sizeof *h);
        if (!h)
            return false;
        s->heap = h;
        s->qcap = cap;
    }
    size_t i = s->qlen++;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!item_before(it, &s->heap[parent]))
            break;
        s->heap[i] = s->heap[parent];
        i = parent;
    }
    s->heap[i] = *it;
    return true;
}

static out_item heap_pop(slot *s)
{
    out_item top = s->heap[0], last = s->heap[--s->qlen];
    size_t i = 0;
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= s->qlen)
            break;
        if (c + 1 < s->qlen && item_before(&s->heap[c + 1], &s->heap[c]))
            c++;
        if (!item_before(&s->heap[c], &last))
            break;
        s->heap[i] = s->heap[c];
        i = c;
    }
    if (s->qlen)
        s->heap[i] = last;
    return top;
}

/* CoreMIDI receive block: runs on CoreMIDI's thread. */
static void out_enqueue(slot *s, uint8_t cable, const MIDIEventList *el)
{
    if (!atomic_load(&s->online))
        return;                         /* nothing to send to: drop */
    uint64_t now = mach_absolute_time();
    bool added = false;
    pthread_mutex_lock(&s->qm);
    const MIDIEventPacket *p = &el->packet[0];
    for (UInt32 i = 0; i < el->numPackets; i++) {
        out_item it = { .ts = p->timeStamp ? p->timeStamp : now, .cable = cable };
        for (UInt32 j = 0; j < p->wordCount;) {
            int n = ump_word_count(p->words[j]);
            if (j + (UInt32)n > p->wordCount)
                break;
            uint32_t type = p->words[j] >> 28;
            if (type >= 1 && type <= 3) {   /* the MIDI 1.0 kinds; utility/JR skipped */
                memcpy(it.w, &p->words[j], (size_t)n * 4);
                it.n = (uint8_t)n;
                it.seq = s->seq++;
                if (heap_push(s, &it)) {
                    added = true;
                } else if (!s->q_full_logged) {
                    log_msg("%s: output queue full, dropping MIDI", s->name);
                    s->q_full_logged = true;
                }
            }
            j += (UInt32)n;
        }
        p = MIDIEventPacketNext(p);
    }
    if (added)
        pthread_cond_signal(&s->qcv);
    pthread_mutex_unlock(&s->qm);
}

static void out_cb(struct libusb_transfer *x)
{
    slot *s = x->user_data;
    pthread_mutex_lock(&s->m);
    s->out_status = x->status;
    s->out_busy = false;
    if (x->status == LIBUSB_TRANSFER_NO_DEVICE || x->status == LIBUSB_TRANSFER_ERROR ||
        x->status == LIBUSB_TRANSFER_STALL)
        atomic_store(&s->need_close, true);
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->m);
}

/* Writes packets and waits for the device to take them (it NAKs while its
 * DIN output buffer is full, which paces large SysEx dumps for us). */
static bool usb_write(slot *s, uint8_t *buf, int len)
{
    pthread_mutex_lock(&s->m);
    if (s->state != ST_OPEN || !s->out_x || !s->ep_out) {
        pthread_mutex_unlock(&s->m);
        return false;
    }
    if (s->out_int)
        libusb_fill_interrupt_transfer(s->out_x, s->h, s->ep_out, buf, len, out_cb, s, OUT_TIMEOUT_MS);
    else
        libusb_fill_bulk_transfer(s->out_x, s->h, s->ep_out, buf, len, out_cb, s, OUT_TIMEOUT_MS);
    s->out_busy = true;
    int r = libusb_submit_transfer(s->out_x);
    if (r != 0) {
        s->out_busy = false;
        pthread_mutex_unlock(&s->m);
        log_msg("%s: OUT submit failed: %s", s->name, libusb_error_name(r));
        atomic_store(&s->need_close, true);
        libusb_interrupt_event_handler(g.ctx);
        return false;
    }
    while (s->out_busy)
        pthread_cond_wait(&s->cv, &s->m);
    enum libusb_transfer_status st = s->out_status;
    pthread_mutex_unlock(&s->m);
    if (st == LIBUSB_TRANSFER_COMPLETED)
        return true;
    if (st == LIBUSB_TRANSFER_TIMED_OUT)
        log_msg("%s: device did not accept MIDI output for %d ms", s->name, OUT_TIMEOUT_MS);
    else if (st != LIBUSB_TRANSFER_CANCELLED)
        log_msg("%s: OUT transfer failed (status %d)", s->name, st);
    if (atomic_load(&s->need_close))
        libusb_interrupt_event_handler(g.ctx);
    return false;
}

static void out_send_batch(slot *s, const out_item *items, int n)
{
    uint8_t buf[512];
    size_t chunk = s->out_size > 0 && (size_t)s->out_size <= sizeof buf ? (size_t)s->out_size : 64;
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        if (len + 16 > chunk) {         /* one UMP message makes at most 12 bytes */
            usb_write(s, buf, (int)len);
            len = 0;
        }
        usbmidi_encoder *e = &s->enc[items[i].cable];
        e->out = buf;
        e->out_len = len;
        e->out_cap = chunk;
        midi_sink sink = usbmidi_encoder_sink(e);
        ump_to_midi(items[i].w, items[i].n, &s->out_parsers[items[i].cable], &sink);
        len = e->out_len;
    }
    if (len)
        usb_write(s, buf, (int)len);
}

static void *out_thread_main(void *arg)
{
    slot *s = arg;
    char name[64];
    snprintf(name, sizeof name, "um1x out %s", s->name);
    set_thread(name);
    out_item batch[OUT_BATCH];
    uint64_t lead = ns_to_ticks(OUT_LEAD_NS);

    pthread_mutex_lock(&s->qm);
    while (!s->quit) {
        if (s->q_reset) {               /* device went away: forget everything */
            for (int c = 0; c < 16; c++) {
                midi_parser_reset(&s->out_parsers[c]);
                usbmidi_encoder_init(&s->enc[c], (uint8_t)c, NULL, 0);
            }
            s->qlen = 0;
            s->q_reset = false;
            s->q_full_logged = false;
        }
        if (s->qlen == 0) {
            pthread_cond_wait(&s->qcv, &s->qm);
            continue;
        }
        uint64_t now = mach_absolute_time();
        if (s->heap[0].ts > now + lead) {
            uint64_t ns = ticks_to_ns(s->heap[0].ts - now);
            struct timespec rel = { (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) };
            pthread_cond_timedwait_relative_np(&s->qcv, &s->qm, &rel);
            continue;
        }
        int n = 0;
        while (s->qlen && n < OUT_BATCH && s->heap[0].ts <= now + lead)
            batch[n++] = heap_pop(s);
        pthread_mutex_unlock(&s->qm);
        out_send_batch(s, batch, n);
        pthread_mutex_lock(&s->qm);
    }
    pthread_mutex_unlock(&s->qm);
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* device -> host */

static bool filtered(uint8_t status)
{
    if (g.opt.filter_clock && (status == 0xF8 || status == 0xFA || status == 0xFB || status == 0xFC))
        return true;
    return g.opt.filter_sensing && status == 0xFE;
}

static void in_message(void *ctx, const uint8_t *m, int len)
{
    in_cable *ic = ctx;
    if (filtered(m[0]))
        return;
    ump_builder_sink(&ic->ump).message(&ic->ump, m, len);
}

static void in_sysex(void *ctx, uint8_t b)
{
    in_cable *ic = ctx;
    ump_builder_sink(&ic->ump).sysex(&ic->ump, b);
}

static void in_flush(slot *s, int cable, MIDITimeStamp ts)
{
    in_cable *ic = &s->in_c[cable];
    if (ic->ump.count == 0)
        return;
    MIDIEndpointRef src = s->src[cable];
    if (src) {
        union { MIDIEventList list; uint8_t raw[sizeof(MIDIEventList) + IN_WORDS * 4 + 64]; } u;
        MIDIEventPacket *p = MIDIEventListInit(&u.list, kMIDIProtocol_1_0);
        for (int i = 0; i < ic->ump.count;) {
            int n = ump_word_count(ic->ump.words[i]);
            MIDIEventPacket *next = MIDIEventListAdd(&u.list, sizeof u, p, ts, (ByteCount)n, &ic->ump.words[i]);
            if (!next) {                /* list full: send it and start another */
                MIDIReceivedEventList(src, &u.list);
                p = MIDIEventListInit(&u.list, kMIDIProtocol_1_0);
                next = MIDIEventListAdd(&u.list, sizeof u, p, ts, (ByteCount)n, &ic->ump.words[i]);
            }
            p = next;
            i += n;
        }
        if (u.list.numPackets)
            MIDIReceivedEventList(src, &u.list);
    }
    ic->ump.count = 0;
    ic->ump.overflow = false;
}

static void in_process(slot *s, const uint8_t *buf, int len)
{
    MIDITimeStamp ts = mach_absolute_time();
    uint16_t touched = 0;
    for (int i = 0; i + 3 < len; i += 4) {
        int cable = usbmidi_decode_packet(buf + i, s->in_parsers, s->in_sinks, s->model->in_cables);
        if (cable < 0)
            continue;
        touched |= (uint16_t)(1u << cable);
        if (s->in_c[cable].ump.count > IN_WORDS - 8)
            in_flush(s, cable, ts);
    }
    for (int c = 0; touched; c++, touched >>= 1)
        if (touched & 1)
            in_flush(s, c, ts);
}

static void in_cb(struct libusb_transfer *x)
{
    slot *s = x->user_data;
    if (x->status == LIBUSB_TRANSFER_COMPLETED)
        in_process(s, x->buffer, x->actual_length);

    pthread_mutex_lock(&s->m);
    bool resubmit = s->state == ST_OPEN;
    switch (x->status) {
    case LIBUSB_TRANSFER_COMPLETED:
    case LIBUSB_TRANSFER_TIMED_OUT:
    case LIBUSB_TRANSFER_OVERFLOW:
        break;
    case LIBUSB_TRANSFER_CANCELLED:
        resubmit = false;
        break;
    default:                            /* NO_DEVICE, ERROR, STALL */
        if (resubmit)
            log_debug("%s: IN transfer status %d", s->name, x->status);
        resubmit = false;
        atomic_store(&s->need_close, true);
        break;
    }
    if (resubmit && libusb_submit_transfer(x) == 0) {
        pthread_mutex_unlock(&s->m);
        return;
    }
    if (resubmit)
        atomic_store(&s->need_close, true);
    s->in_active--;
    pthread_mutex_unlock(&s->m);
}

/* ------------------------------------------------------------------------ */
/* CoreMIDI endpoints */

static void set_offline(slot *s, bool offline)
{
    for (int c = 0; c < 16; c++) {
        if (s->src[c]) MIDIObjectSetIntegerProperty(s->src[c], kMIDIPropertyOffline, offline);
        if (s->dst[c]) MIDIObjectSetIntegerProperty(s->dst[c], kMIDIPropertyOffline, offline);
    }
}

static void set_props(slot *s, MIDIEndpointRef ep, int cable, char dir)
{
    CFStringRef manu = CFStringCreateWithCString(NULL, s->model->manufacturer, kCFStringEncodingUTF8);
    CFStringRef model = CFStringCreateWithCString(NULL, s->model->name, kCFStringEncodingUTF8);
    MIDIObjectSetStringProperty(ep, kMIDIPropertyManufacturer, manu);
    MIDIObjectSetStringProperty(ep, kMIDIPropertyModel, model);
    CFRelease(manu);
    CFRelease(model);
    /* A fixed unique ID lets apps (Logic) recognise the port after the driver
     * restarts, instead of treating it as a new device every login. */
    OSStatus st = MIDIObjectSetIntegerProperty(ep, kMIDIPropertyUniqueID, unique_id(s, cable, dir));
    if (st != noErr)
        log_debug("%s: unique ID not set (%d)", s->name, (int)st);
}

static bool create_endpoints(slot *s)
{
    const um_model *m = s->model;
    char name[96];
    for (int c = 0; c < 16; c++) {
        if (m->in_cables & (1u << c)) {
            port_name(s, c, m->in_cables, name, sizeof name);
            CFStringRef n = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
            OSStatus st = MIDISourceCreateWithProtocol(g.client, n, kMIDIProtocol_1_0, &s->src[c]);
            CFRelease(n);
            if (st != noErr) {
                log_msg("%s: cannot create CoreMIDI source (%d)", name, (int)st);
                return false;
            }
            set_props(s, s->src[c], c, 'i');
        }
        if (m->out_cables & (1u << c)) {
            port_name(s, c, m->out_cables, name, sizeof name);
            CFStringRef n = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
            uint8_t cable = (uint8_t)c;
            OSStatus st = MIDIDestinationCreateWithProtocol(g.client, n, kMIDIProtocol_1_0, &s->dst[c],
                ^(const MIDIEventList *el, void *ref) { (void)ref; out_enqueue(s, cable, el); });
            CFRelease(n);
            if (st != noErr) {
                log_msg("%s: cannot create CoreMIDI destination (%d)", name, (int)st);
                return false;
            }
            set_props(s, s->dst[c], c, 'o');
            /* Get scheduled events 2 ms early and send them on time from our
             * own thread, instead of when CoreMIDI gets round to it. */
            MIDIObjectSetIntegerProperty(s->dst[c], kMIDIPropertyAdvanceScheduleTimeMuSec, ADVANCE_US);
        }
    }
    return true;
}

static slot *slot_new(const um_model *m, int instance)
{
    if (g.nslots == MAX_SLOTS)
        return NULL;
    slot *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->model = m;
    s->instance = instance;
    if (instance == 0)
        snprintf(s->name, sizeof s->name, "%s", m->name);
    else
        snprintf(s->name, sizeof s->name, "%s (%d)", m->name, instance + 1);
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->cv, NULL);
    pthread_mutex_init(&s->qm, NULL);
    pthread_cond_init(&s->qcv, NULL);
    for (int c = 0; c < 16; c++) {
        s->in_c[c].s = s;
        ump_builder_init(&s->in_c[c].ump, 0, s->in_c[c].words, IN_WORDS);
        s->in_sinks[c] = (midi_sink){ in_message, in_sysex, &s->in_c[c] };
        usbmidi_encoder_init(&s->enc[c], (uint8_t)c, NULL, 0);
    }
    if (!create_endpoints(s)) {
        for (int c = 0; c < 16; c++) {
            if (s->src[c]) MIDIEndpointDispose(s->src[c]);
            if (s->dst[c]) MIDIEndpointDispose(s->dst[c]);
        }
        free(s);
        return NULL;
    }
    pthread_create(&s->out_thread, NULL, out_thread_main, s);
    g.slots[g.nslots++] = s;
    return s;
}

/* A free slot for a device of this model: the one it used last time (same
 * USB port), else the lowest free one, else a new one. */
static slot *pick_slot(const um_model *m, const char *location)
{
    slot *best = NULL;
    int count = 0;
    for (int i = 0; i < g.nslots; i++) {
        slot *s = g.slots[i];
        if (s->model != m)
            continue;
        count++;
        pthread_mutex_lock(&s->m);
        bool free_slot = s->state == ST_CLOSED;
        pthread_mutex_unlock(&s->m);
        if (!free_slot)
            continue;
        if (strcmp(s->location, location) == 0)
            return s;
        if (!best)
            best = s;
    }
    return best ? best : slot_new(m, count);
}

/* ------------------------------------------------------------------------ */
/* USB open / close */

static bool usb_open(libusb_device *dev, const um_model *m, const char *where, usb_open_info *info)
{
    struct libusb_config_descriptor *cfg = NULL;
    libusb_device_handle *h = NULL;
    bool claimed = false;
    memset(info, 0, sizeof *info);

    int r = libusb_open(dev, &h);
    if (r != 0) {
        log_failure("%s at %s: cannot open: %s", m->name, where, libusb_error_name(r));
        goto fail;
    }
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 &&
        (r = libusb_get_config_descriptor(dev, 0, &cfg)) != 0) {
        log_failure("%s at %s: no configuration descriptor: %s", m->name, where, libusb_error_name(r));
        goto fail;
    }
    int current = 0;
    if (libusb_get_configuration(h, &current) == 0 && current == 0) {
        r = libusb_set_configuration(h, cfg->bConfigurationValue);
        if (r != 0)
            log_debug("%s: set configuration: %s", m->name, libusb_error_name(r));
    }
    const struct libusb_interface *intf = NULL;
    for (int i = 0; i < cfg->bNumInterfaces; i++)
        if (cfg->interface[i].num_altsetting > 0 &&
            cfg->interface[i].altsetting[0].bInterfaceNumber == m->iface)
            intf = &cfg->interface[i];
    if (!intf) {
        log_failure("%s at %s: interface %d not found", m->name, where, m->iface);
        goto fail;
    }
    r = libusb_claim_interface(h, m->iface);
    if (r != 0) {
        log_failure("%s at %s: cannot claim interface %d: %s%s", m->name, where, m->iface,
                    libusb_error_name(r),
                    r == LIBUSB_ERROR_ACCESS || r == LIBUSB_ERROR_BUSY
                        ? " (another program or driver is using it)" : "");
        goto fail;
    }
    claimed = true;

    /* Like Linux snd-usb-audio: on Roland devices with two alt settings,
     * alt 1 has an interrupt IN endpoint, which Roland calls "light load"
     * mode and which delivers input with lower, steadier latency. */
    int alt = 0;
    if (!g.opt.force_bulk && m->vid == 0x0582 && intf->num_altsetting == 2) {
        const struct libusb_interface_descriptor *a1 = &intf->altsetting[1];
        bool has_int = false;
        for (int e = 0; e < a1->bNumEndpoints; e++)
            if ((a1->endpoint[e].bmAttributes & 3) == LIBUSB_TRANSFER_TYPE_INTERRUPT)
                has_int = true;
        if (a1->bNumEndpoints == 2 && has_int)
            alt = 1;
    }
    if (alt) {
        r = libusb_set_interface_alt_setting(h, m->iface, alt);
        if (r != 0) {
            log_msg("%s: alt setting %d failed (%s), using bulk mode", m->name, alt, libusb_error_name(r));
            alt = 0;
            libusb_set_interface_alt_setting(h, m->iface, 0);
        }
    }
    const struct libusb_interface_descriptor *id = &intf->altsetting[alt];
    for (int e = 0; e < id->bNumEndpoints; e++) {
        const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
        int type = ep->bmAttributes & 3;
        if (type != LIBUSB_TRANSFER_TYPE_BULK && type != LIBUSB_TRANSFER_TYPE_INTERRUPT)
            continue;
        if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
            if (!info->ep_in) {
                info->ep_in = ep->bEndpointAddress;
                info->in_int = type == LIBUSB_TRANSFER_TYPE_INTERRUPT;
                info->in_size = ep->wMaxPacketSize & 0x7ff;
            }
        } else if (!info->ep_out) {
            info->ep_out = ep->bEndpointAddress;
            info->out_int = type == LIBUSB_TRANSFER_TYPE_INTERRUPT;
            info->out_size = ep->wMaxPacketSize & 0x7ff;
        }
    }
    if (!info->ep_in && !info->ep_out) {
        log_failure("%s at %s: no MIDI endpoints on interface %d", m->name, where, m->iface);
        goto fail;
    }
    if (info->in_size < 4) info->in_size = 64;
    if (info->out_size < 4) info->out_size = 64;
    info->h = h;
    info->alt = alt;
    libusb_free_config_descriptor(cfg);
    return true;

fail:
    if (claimed)
        libusb_release_interface(h, m->iface);
    if (h)
        libusb_close(h);
    if (cfg)
        libusb_free_config_descriptor(cfg);
    return false;
}

static void slot_attach(slot *s, libusb_device *dev, const usb_open_info *info, const char *where)
{
    for (int c = 0; c < 16; c++) {
        midi_parser_reset(&s->in_parsers[c]);
        ump_builder_init(&s->in_c[c].ump, 0, s->in_c[c].words, IN_WORDS);
    }
    pthread_mutex_lock(&s->m);
    s->dev = libusb_ref_device(dev);
    s->h = info->h;
    s->alt = info->alt;
    s->ep_in = info->ep_in;
    s->ep_out = info->ep_out;
    s->in_int = info->in_int;
    s->out_int = info->out_int;
    s->in_size = info->in_size;
    s->out_size = info->out_size;
    s->out_x = libusb_alloc_transfer(0);
    s->out_busy = false;
    s->in_active = 0;
    snprintf(s->location, sizeof s->location, "%s", where);
    atomic_store(&s->need_close, false);
    s->state = ST_OPEN;
    if (s->ep_in) {
        for (int i = 0; i < IN_TRANSFERS; i++) {
            struct libusb_transfer *x = libusb_alloc_transfer(0);
            uint8_t *buf = malloc((size_t)s->in_size);
            if (!x || !buf) {
                free(buf);
                libusb_free_transfer(x);
                continue;
            }
            if (s->in_int)
                libusb_fill_interrupt_transfer(x, s->h, s->ep_in, buf, s->in_size, in_cb, s, 0);
            else
                libusb_fill_bulk_transfer(x, s->h, s->ep_in, buf, s->in_size, in_cb, s, 0);
            x->flags = LIBUSB_TRANSFER_FREE_BUFFER;
            int r = libusb_submit_transfer(x);
            if (r != 0) {
                log_msg("%s: IN submit failed: %s", s->name, libusb_error_name(r));
                libusb_free_transfer(x);
                continue;
            }
            s->in_x[i] = x;
            s->in_active++;
        }
    }
    pthread_mutex_unlock(&s->m);

    pthread_mutex_lock(&s->qm);
    s->q_reset = true;                  /* start the output side clean */
    pthread_cond_signal(&s->qcv);
    pthread_mutex_unlock(&s->qm);

    atomic_store(&s->online, true);
    set_offline(s, false);
    g.last_failure[0] = 0;
    log_msg("connected: %s (%04x:%04x) at USB %s, interface %d alt %d, IN 0x%02x %s, OUT 0x%02x %s",
            s->name, s->model->vid, s->model->pid, where, s->model->iface, s->alt,
            s->ep_in, s->in_int ? "interrupt" : "bulk", s->ep_out, s->out_int ? "interrupt" : "bulk");
}

static void slot_close(slot *s, const char *why)
{
    pthread_mutex_lock(&s->m);
    if (s->state != ST_OPEN) {
        pthread_mutex_unlock(&s->m);
        return;
    }
    s->state = ST_CLOSING;
    atomic_store(&s->online, false);
    for (int i = 0; i < IN_TRANSFERS; i++)
        if (s->in_x[i])
            libusb_cancel_transfer(s->in_x[i]);
    if (s->out_busy)
        libusb_cancel_transfer(s->out_x);
    pthread_mutex_unlock(&s->m);

    /* Let the cancellations complete before the handle goes away. */
    uint64_t deadline = now_ms() + 2000;
    bool drained = false;
    for (;;) {
        pthread_mutex_lock(&s->m);
        drained = s->in_active == 0 && !s->out_busy;
        pthread_mutex_unlock(&s->m);
        if (drained || now_ms() > deadline)
            break;
        struct timeval tv = { 0, 50000 };
        libusb_handle_events_timeout_completed(g.ctx, &tv, NULL);
    }

    libusb_release_interface(s->h, s->model->iface);
    libusb_close(s->h);

    pthread_mutex_lock(&s->m);
    if (drained) {                      /* otherwise leak them rather than free live transfers */
        for (int i = 0; i < IN_TRANSFERS; i++)
            libusb_free_transfer(s->in_x[i]);
        libusb_free_transfer(s->out_x);
    }
    memset(s->in_x, 0, sizeof s->in_x);
    s->out_x = NULL;
    s->h = NULL;
    libusb_unref_device(s->dev);
    s->dev = NULL;
    s->state = ST_CLOSED;
    atomic_store(&s->need_close, false);
    pthread_mutex_unlock(&s->m);

    pthread_mutex_lock(&s->qm);
    s->q_reset = true;
    pthread_cond_signal(&s->qcv);
    pthread_mutex_unlock(&s->qm);

    set_offline(s, true);
    log_msg("disconnected: %s (%s)", s->name, why);
}

static void close_all(const char *why)
{
    for (int i = 0; i < g.nslots; i++)
        slot_close(g.slots[i], why);
}

static bool device_in_use(libusb_device *dev)
{
    for (int i = 0; i < g.nslots; i++) {
        slot *s = g.slots[i];
        pthread_mutex_lock(&s->m);
        bool used = s->state != ST_CLOSED && s->dev == dev;
        pthread_mutex_unlock(&s->m);
        if (used)
            return true;
    }
    return false;
}

/* Opens every supported device not open yet. Returns false if one failed. */
static bool scan(void)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(g.ctx, &list);
    if (n < 0) {
        log_failure("cannot list USB devices: %s", libusb_error_name((int)n));
        return false;
    }
    bool ok = true;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0)
            continue;
        const um_model *m = um_find_model(d.idVendor, d.idProduct);
        if (!m || device_in_use(list[i]))
            continue;
        char where[32];
        location_of(list[i], where, sizeof where);
        usb_open_info info;
        if (!usb_open(list[i], m, where, &info)) {
            ok = false;
            continue;
        }
        slot *s = pick_slot(m, where);
        if (!s) {
            log_failure("%s at %s: no free port slot", m->name, where);
            libusb_release_interface(info.h, m->iface);
            libusb_close(info.h);
            ok = false;
            continue;
        }
        slot_attach(s, list[i], &info, where);
    }
    libusb_free_device_list(list, 1);
    return ok;
}

static int LIBUSB_CALL hotplug_cb(libusb_context *ctx, libusb_device *dev,
                                  libusb_hotplug_event event, void *user)
{
    (void)ctx; (void)user;
    struct libusb_device_descriptor d;
    if (libusb_get_device_descriptor(dev, &d) != 0 || !um_find_model(d.idVendor, d.idProduct))
        return 0;
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
        log_debug("hotplug: %04x:%04x arrived", d.idVendor, d.idProduct);
        atomic_store(&g.arrived, true);
    } else {
        log_debug("hotplug: %04x:%04x left", d.idVendor, d.idProduct);
        for (int i = 0; i < g.nslots; i++)
            if (g.slots[i]->dev == dev)
                atomic_store(&g.slots[i]->need_close, true);
    }
    return 0;
}

static uint64_t min_u64(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static void *usb_thread_main(void *arg)
{
    (void)arg;
    set_thread("um1x usb");
    uint64_t next_scan = now_ms();

    while (!atomic_load(&g.quit)) {
        uint64_t now = now_ms();
        if (next_scan > now) {
            uint64_t wait = min_u64(next_scan - now, SAFETY_SCAN_MS);
            struct timeval tv = { (time_t)(wait / 1000), (suseconds_t)(wait % 1000) * 1000 };
            libusb_handle_events_timeout_completed(g.ctx, &tv, NULL);
        }
        if (atomic_load(&g.quit))
            break;
        now = now_ms();

        if (atomic_exchange(&g.sleep_req, false)) {
            close_all("system is going to sleep");
            dispatch_semaphore_signal(g.sleep_done);
        }
        if (atomic_exchange(&g.woke, false)) {
            close_all("system woke up");
            next_scan = now + 1000;     /* give the hub a moment to come back */
        }
        if (atomic_exchange(&g.reconnect_all, false)) {
            close_all("reconnect requested");
            next_scan = now + 300;
        }
        for (int i = 0; i < g.nslots; i++) {
            if (atomic_load(&g.slots[i]->need_close)) {
                slot_close(g.slots[i], "unplugged or USB error");
                next_scan = min_u64(next_scan, now + 300);
            }
        }
        if (atomic_exchange(&g.arrived, false))
            next_scan = min_u64(next_scan, now + 150);

        if (atomic_load(&g.sleeping)) {
            next_scan = now + SAFETY_SCAN_MS;
            continue;
        }
        if (now >= next_scan) {
            bool ok = scan();
            next_scan = now_ms() + (!ok ? RETRY_MS : g.hotplug ? SAFETY_SCAN_MS : 1000);
        }
    }

    close_all("driver stopping");
    for (int i = 0; i < g.nslots; i++) {
        slot *s = g.slots[i];
        pthread_mutex_lock(&s->qm);
        s->quit = true;
        pthread_cond_signal(&s->qcv);
        pthread_mutex_unlock(&s->qm);
        pthread_join(s->out_thread, NULL);
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* main queue: power, signals, startup and shutdown */

static void power_cb(void *ref, io_service_t service, natural_t type, void *arg)
{
    (void)ref; (void)service;
    switch (type) {
    case kIOMessageCanSystemSleep:
        IOAllowPowerChange(g.power_port, (long)arg);
        break;
    case kIOMessageSystemWillSleep:
        log_msg("system will sleep");
        atomic_store(&g.sleeping, true);
        atomic_store(&g.sleep_req, true);
        libusb_interrupt_event_handler(g.ctx);
        dispatch_semaphore_wait(g.sleep_done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
        IOAllowPowerChange(g.power_port, (long)arg);
        break;
    case kIOMessageSystemHasPoweredOn:
        log_msg("system woke up");
        atomic_store(&g.sleeping, false);
        atomic_store(&g.woke, true);
        libusb_interrupt_event_handler(g.ctx);
        break;
    default:
        break;
    }
}

static void shutdown_and_exit(int sig)
{
    log_msg("stopping (signal %d)", sig);
    atomic_store(&g.quit, true);
    libusb_interrupt_event_handler(g.ctx);
    pthread_join(g.usb_thread, NULL);
    if (g.hotplug)
        libusb_hotplug_deregister_callback(g.ctx, g.hotplug_handle);
    for (int i = 0; i < g.nslots; i++)
        for (int c = 0; c < 16; c++) {
            if (g.slots[i]->src[c]) MIDIEndpointDispose(g.slots[i]->src[c]);
            if (g.slots[i]->dst[c]) MIDIEndpointDispose(g.slots[i]->dst[c]);
        }
    MIDIClientDispose(g.client);
    libusb_exit(g.ctx);
    if (g.power_notifier) {
        IODeregisterForSystemPower(&g.power_notifier);
        IOServiceClose(g.power_port);
        IONotificationPortDestroy(g.power_notify);
    }
    log_msg("stopped");
    exit(0);
}

static void on_signal(int sig, void (^handler)(void))
{
    signal(sig, SIG_IGN);
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sig, 0,
                                                   dispatch_get_main_queue());
    dispatch_source_set_event_handler(src, handler);
    dispatch_resume(src);
}

/* One driver per user: a second one could never claim the devices anyway. */
static bool single_instance(void)
{
    char dir[512], path[600];
    if (confstr(_CS_DARWIN_USER_TEMP_DIR, dir, sizeof dir) == 0)
        snprintf(dir, sizeof dir, "/tmp/");
    snprintf(path, sizeof path, "%sum1x-midi-driver.lock", dir);
    g.lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (g.lock_fd < 0)
        return true;                    /* cannot tell; carry on */
    if (flock(g.lock_fd, LOCK_EX | LOCK_NB) != 0) {
        char pid[32] = { 0 };
        pread(g.lock_fd, pid, sizeof pid - 1, 0);
        log_msg("another um1x-midi-driver is already running (pid %s)", pid[0] ? pid : "?");
        return false;
    }
    char pid[32];
    int n = snprintf(pid, sizeof pid, "%d\n", getpid());
    ftruncate(g.lock_fd, 0);
    pwrite(g.lock_fd, pid, (size_t)n, 0);
    return true;
}

int driver_run(const driver_options *opt)
{
    g.opt = *opt;
    mach_timebase_info(&g.tb);
    if (!single_instance())
        return 1;

    int r = libusb_init_context(&g.ctx, NULL, 0);
    if (r != 0) {
        log_msg("libusb init failed: %s", libusb_error_name(r));
        return 1;
    }
    if (log_verbose)
        libusb_set_option(g.ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);

    /* The MIDI server may not be up yet this early in a login. */
    OSStatus st = -1;
    for (int i = 0; i < 30 && st != noErr; i++) {
        st = MIDIClientCreateWithBlock(CFSTR("UM-1X driver"), &g.client, NULL);
        if (st != noErr)
            sleep(1);
    }
    if (st != noErr) {
        log_msg("cannot create CoreMIDI client (%d)", (int)st);
        return 1;
    }

    if (libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        r = libusb_hotplug_register_callback(
            g.ctx, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, 0,
            LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
            hotplug_cb, NULL, &g.hotplug_handle);
        g.hotplug = r == 0;
    }

    g.sleep_done = dispatch_semaphore_create(0);
    g.power_port = IORegisterForSystemPower(NULL, &g.power_notify, power_cb, &g.power_notifier);
    if (g.power_port)
        IONotificationPortSetDispatchQueue(g.power_notify, dispatch_get_main_queue());
    else
        log_msg("warning: no sleep/wake notifications; reconnects after sleep rely on hot-plug");

    signal(SIGPIPE, SIG_IGN);
    on_signal(SIGINT, ^{ shutdown_and_exit(SIGINT); });
    on_signal(SIGTERM, ^{ shutdown_and_exit(SIGTERM); });
    on_signal(SIGHUP, ^{
        log_msg("SIGHUP: reconnecting");
        atomic_store(&g.reconnect_all, true);
        libusb_interrupt_event_handler(g.ctx);
    });

    log_msg("started, pid %d, hot-plug %s, input mode %s%s%s", getpid(), g.hotplug ? "on" : "off (polling)",
            opt->force_bulk ? "bulk" : "interrupt where available",
            opt->filter_clock ? ", filtering clock" : "", opt->filter_sensing ? ", filtering active sensing" : "");
    pthread_create(&g.usb_thread, NULL, usb_thread_main, NULL);
    dispatch_main();
}
