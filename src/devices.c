/*
 * devices.c - the device table.
 *
 * Old Roland/Edirol interfaces in their "vendor" (non class-compliant) mode
 * expose a vendor-specific interface that nevertheless speaks plain USB-MIDI
 * 1.0 event packets on one bulk/interrupt endpoint pair. The facts below
 * (USB IDs, MIDI interface number, cable masks) are taken from the Linux
 * kernel's sound/usb/quirks-table.h, entries using
 * QUIRK_MIDI_FIXED_ENDPOINT. Only MIDI-only products are listed: audio
 * interfaces are left out so this driver never grabs a device that has a
 * real macOS driver of its own.
 *
 * Only the UM-1 family has been tested with this driver. The others are
 * expected to work the same way but are unverified; reports are welcome.
 */
#include "devices.h"

#include <stddef.h>

#define ROLAND 0x0582

const um_model um_models[] = {
    /* vid    pid     name           manufacturer iface out     in      tested */
    { ROLAND, 0x0009, "UM-1X",       "Roland",    2, 0x0001, 0x0001, true  }, /* Linux: "EDIROL UM-1" */
    { ROLAND, 0x0002, "UM-4",        "Roland",    2, 0x000f, 0x000f, false },
    { ROLAND, 0x0003, "SC-8850",     "Roland",    2, 0x003f, 0x003f, false },
    { ROLAND, 0x0004, "U-8",         "Roland",    2, 0x0005, 0x0005, false },
    { ROLAND, 0x0005, "UM-2",        "Roland",    2, 0x0003, 0x0003, false },
    { ROLAND, 0x0007, "SC-8820",     "Roland",    2, 0x0013, 0x0013, false },
    { ROLAND, 0x0008, "PC-300",      "Roland",    2, 0x0001, 0x0001, false },
    { ROLAND, 0x000b, "SK-500",      "Roland",    2, 0x0013, 0x0013, false },
    { ROLAND, 0x0012, "XV-5050",     "Roland",    0, 0x0001, 0x0001, false },
    { ROLAND, 0x0014, "UM-880",      "Roland",    0, 0x01ff, 0x01ff, false },
    { ROLAND, 0x001b, "MMP-2",       "Roland",    2, 0x0001, 0x0001, false },
    { ROLAND, 0x001d, "V-Synth",     "Roland",    0, 0x0001, 0x0001, false },
    { ROLAND, 0x0023, "UM-550",      "Roland",    0, 0x003f, 0x003f, false },
    { ROLAND, 0x0027, "SD-20",       "Roland",    0, 0x0003, 0x0007, false },
    { ROLAND, 0x0029, "SD-80",       "Roland",    0, 0x000f, 0x000f, false },
    { ROLAND, 0x002d, "XV-2020",     "Roland",    0, 0x0001, 0x0001, false },
    { ROLAND, 0x002f, "VariOS",      "Roland",    0, 0x0007, 0x0007, false },
    { ROLAND, 0x0033, "PCR",         "Roland",    0, 0x0003, 0x0007, false },
    { ROLAND, 0x0037, "Digital Piano", "Roland",  0, 0x0001, 0x0001, false },
    { ROLAND, 0x0040, "GI-20",       "Roland",    0, 0x0001, 0x0001, false },
    { ROLAND, 0x0042, "RS-70",       "Roland",    0, 0x0001, 0x0001, false },
    { ROLAND, 0x0048, "UR-80",       "Roland",    0, 0x0003, 0x0007, false },
    { ROLAND, 0x004d, "PCR-A",       "Roland",    0, 0x0003, 0x0007, false },
    { ROLAND, 0x0065, "PCR-1",       "Roland",    0, 0x0001, 0x0003, false },
    { ROLAND, 0x006d, "Fantom-X",    "Roland",    0, 0x0001, 0x0001, false },
    { ROLAND, 0x0075, "DR-880",      "BOSS",      0, 0x0001, 0x0001, false },
    { ROLAND, 0x0080, "G-70",        "Roland",    0, 0x0001, 0x0001, false },
    { ROLAND, 0x008b, "PC-50",       "Roland",    0, 0x0001, 0x0001, false },
};

const int um_model_count = (int)(sizeof um_models / sizeof um_models[0]);

const um_model *um_find_model(uint16_t vid, uint16_t pid)
{
    for (int i = 0; i < um_model_count; i++)
        if (um_models[i].vid == vid && um_models[i].pid == pid)
            return &um_models[i];
    return NULL;
}
