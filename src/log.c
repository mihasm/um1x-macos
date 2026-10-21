/*
 * log.c - see log.h.
 */
#include "log.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LOG_ROTATE_BYTES (1024 * 1024)

bool log_verbose;

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static char *log_path;

static bool reopen(void)
{
    int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    fflush(stderr);
    dup2(fd, STDERR_FILENO);
    close(fd);
    return true;
}

bool log_open(const char *path)
{
    free(log_path);
    log_path = strdup(path);
    return log_path && reopen();
}

static void rotate_if_needed(void)
{
    struct stat st;
    if (!log_path || fstat(STDERR_FILENO, &st) != 0 || st.st_size < LOG_ROTATE_BYTES)
        return;
    char old[1024];
    snprintf(old, sizeof old, "%s.1", log_path);
    rename(log_path, old);
    reopen();
}

void log_msg(const char *fmt, ...)
{
    struct timeval tv;
    struct tm tm;
    char stamp[32];
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);

    pthread_mutex_lock(&log_lock);
    rotate_if_needed();
    fprintf(stderr, "%s.%03d ", stamp, (int)(tv.tv_usec / 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    pthread_mutex_unlock(&log_lock);
}
