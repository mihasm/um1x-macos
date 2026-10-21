/*
 * devices.h - USB MIDI devices this driver knows how to talk to.
 */
#ifndef UM1X_DEVICES_H
#define UM1X_DEVICES_H

#include <stdbool.h>
#include <stdint.h>

typedef struct um_model {
    uint16_t vid, pid;
    const char *name;        /* CoreMIDI port name */
    const char *manufacturer;
    uint8_t iface;           /* interface with the MIDI endpoints */
    uint16_t out_cables;     /* bit n set: cable n exists host -> device */
    uint16_t in_cables;      /* bit n set: cable n exists device -> host */
    bool tested;             /* checked on real hardware with this driver */
} um_model;

extern const um_model um_models[];
extern const int um_model_count;

const um_model *um_find_model(uint16_t vid, uint16_t pid);

#endif
