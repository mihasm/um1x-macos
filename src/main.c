/*
 * um1x-midi-driver - user-space CoreMIDI driver for the Roland/Edirol UM-1X
 * (and other vendor-mode Roland MIDI interfaces).
 * https://github.com/mihasm/um1x-macos
 */
#include "driver.h"
#include "log.h"
#include "tools.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef UM1X_VERSION
#define UM1X_VERSION "dev"
#endif

static void usage(FILE *f)
{
    fprintf(f,
        "um1x-midi-driver " UM1X_VERSION " - CoreMIDI driver for the Roland/Edirol UM-1X USB MIDI interface\n"
        "https://github.com/mihasm/um1x-macos\n"
        "\n"
        "usage:\n"
        "  um1x-midi-driver [run] [options]   run the driver (what the login agent does)\n"
        "      -v, --verbose          log more detail\n"
        "      -l, --log FILE         log to FILE (rotated at 1 MB) instead of stderr\n"
        "      --bulk                 read input with bulk transfers (alt setting 0) instead\n"
        "                             of the interrupt endpoint\n"
        "      --filter-clock         drop MIDI clock, start, stop and continue from the device\n"
        "      --filter-sensing       drop active sensing from the device\n"
        "\n"
        "  um1x-midi-driver list              supported USB devices plugged in, and all CoreMIDI ports\n"
        "  um1x-midi-driver devices           the table of supported devices\n"
        "  um1x-midi-driver monitor [-a] [-r] [-t SECONDS] [NAME]\n"
        "                                     print MIDI arriving from the driver's ports (or from\n"
        "                                     sources whose name contains NAME); -a also shows clock\n"
        "                                     and active sensing, -r raw UMP words\n"
        "  um1x-midi-driver send [-d NAME] [-a MS] HEX... | -f FILE.syx\n"
        "                                     send raw MIDI bytes, e.g. send 90 3c 64, or a file;\n"
        "                                     -a stamps them MS milliseconds in the future\n"
        "  um1x-midi-driver test-out [-d NAME]\n"
        "                                     play a short test phrase on the instrument\n"
        "  um1x-midi-driver version | help\n");
}

static int cmd_run(int argc, char **argv)
{
    static const struct option opts[] = {
        { "verbose", no_argument, NULL, 'v' },
        { "log", required_argument, NULL, 'l' },
        { "bulk", no_argument, NULL, 'b' },
        { "filter-clock", no_argument, NULL, 'c' },
        { "filter-sensing", no_argument, NULL, 's' },
        { "help", no_argument, NULL, 'h' },
        { 0 },
    };
    driver_options o = { 0 };
    int c;
    while ((c = getopt_long(argc, argv, "vl:h", opts, NULL)) != -1) {
        switch (c) {
        case 'v': log_verbose = true; break;
        case 'l':
            if (!log_open(optarg)) {
                fprintf(stderr, "cannot open log file %s\n", optarg);
                return 1;
            }
            break;
        case 'b': o.force_bulk = true; break;
        case 'c': o.filter_clock = true; break;
        case 's': o.filter_sensing = true; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (optind < argc) {
        usage(stderr);
        return 2;
    }
    log_msg("um1x-midi-driver " UM1X_VERSION);
    return driver_run(&o);
}

static int cmd_monitor(int argc, char **argv)
{
    bool all = false, raw = false;
    double seconds = 0;
    int c;
    while ((c = getopt(argc, argv, "art:h")) != -1) {
        switch (c) {
        case 'a': all = true; break;
        case 'r': raw = true; break;
        case 't': seconds = atof(optarg); break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    return tool_monitor(optind < argc ? argv[optind] : NULL, all, raw, seconds);
}

static int cmd_send(int argc, char **argv, bool test)
{
    const char *dest = NULL, *file = NULL;
    int c, delay = 0;
    while ((c = getopt(argc, argv, "a:d:f:h")) != -1) {
        switch (c) {
        case 'a': delay = atoi(optarg); break;
        case 'd': dest = optarg; break;
        case 'f': file = optarg; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (test)
        return tool_test_out(dest);
    if (file) {
        FILE *f = fopen(file, "rb");
        if (!f) {
            perror(file);
            return 1;
        }
        size_t cap = 1 << 16, len = 0, got;
        uint8_t *data = malloc(cap);
        while ((got = fread(data + len, 1, cap - len, f)) > 0)
            if ((len += got) == cap)
                data = realloc(data, cap *= 2);
        fclose(f);
        return tool_send(dest, data, (int)len, delay);
    }
    int n = argc - optind;
    if (n <= 0) {
        fprintf(stderr, "send: no bytes given, e.g. send 90 3c 64\n");
        return 2;
    }
    uint8_t *bytes = malloc((size_t)n);
    for (int i = 0; i < n; i++) {
        if (tool_parse_hex(argv[optind + i], &bytes[i]) != 0) {
            fprintf(stderr, "send: \"%s\" is not a hex byte\n", argv[optind + i]);
            return 2;
        }
    }
    return tool_send(dest, bytes, n, delay);
}

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        printf("um1x-midi-driver %s\n", UM1X_VERSION);
        return 0;
    }
    const char *cmd = argc > 1 && argv[1][0] != '-' ? argv[1] : "run";
    int sub_argc = argc > 1 && argv[1][0] != '-' ? argc - 1 : argc;
    char **sub_argv = argc > 1 && argv[1][0] != '-' ? argv + 1 : argv;

    if (!strcmp(cmd, "run")) return cmd_run(sub_argc, sub_argv);
    if (!strcmp(cmd, "list")) return tool_list();
    if (!strcmp(cmd, "devices")) return tool_devices();
    if (!strcmp(cmd, "monitor")) return cmd_monitor(sub_argc, sub_argv);
    if (!strcmp(cmd, "send")) return cmd_send(sub_argc, sub_argv, false);
    if (!strcmp(cmd, "test-out")) return cmd_send(sub_argc, sub_argv, true);
    if (!strcmp(cmd, "version")) { printf("um1x-midi-driver %s\n", UM1X_VERSION); return 0; }
    if (!strcmp(cmd, "help")) { usage(stdout); return 0; }
    fprintf(stderr, "unknown command \"%s\"\n\n", cmd);
    usage(stderr);
    return 2;
}
