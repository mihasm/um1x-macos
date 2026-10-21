/*
 * driver.h - the resident driver: USB devices <-> virtual CoreMIDI ports.
 */
#ifndef UM1X_DRIVER_H
#define UM1X_DRIVER_H

#include <stdbool.h>

typedef struct driver_options {
    bool force_bulk;        /* alt setting 0 (bulk IN) instead of interrupt IN */
    bool filter_clock;      /* drop F8 clock, FA/FB/FC start/continue/stop from the device */
    bool filter_sensing;    /* drop FE active sensing from the device */
} driver_options;

/* Runs until SIGINT or SIGTERM. SIGHUP reconnects every device.
 * Returns the process exit code. */
int driver_run(const driver_options *opt);

#endif
