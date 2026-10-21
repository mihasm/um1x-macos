/*
 * log.h - timestamped logging to stderr or to a self-rotating log file.
 */
#ifndef UM1X_LOG_H
#define UM1X_LOG_H

#include <stdbool.h>

extern bool log_verbose;

/* Sends stderr (and so everything logged) to path, appending. The file is
 * rotated to path.1 once it grows past 1 MB. Returns false on error. */
bool log_open(const char *path);

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define log_debug(...) do { if (log_verbose) log_msg(__VA_ARGS__); } while (0)

#endif
