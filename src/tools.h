/*
 * tools.h - diagnostic commands: list, monitor, send, test-out, devices.
 * They talk to CoreMIDI like any other app, so they work while the driver
 * runs and show exactly what Logic and other apps get.
 */
#ifndef UM1X_TOOLS_H
#define UM1X_TOOLS_H

#include <stdbool.h>
#include <stdint.h>

int tool_list(void);
int tool_devices(void);
int tool_monitor(const char *pattern, bool show_all, bool raw, double seconds);
/* delay_ms > 0: stamp the MIDI that far in the future (tests scheduling). */
int tool_send(const char *pattern, const uint8_t *bytes, int n, int delay_ms);
int tool_test_out(const char *pattern);

/* One hex byte ("90", "0x3c"); returns -1 if it is not one. */
int tool_parse_hex(const char *s, uint8_t *out);

#endif
