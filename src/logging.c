// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#include "logging.h"
#include "config.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

FILE *log_stream = NULL;
char  log_dest[64] = "";

extern struct WebSDRConfig g_config;

static char log_filename[256] = "";
static char log_prev_filename[256] = "";

#define DSP_LOGBUF_SIZE 4096
static char  dsp_logbuf[DSP_LOGBUF_SIZE];
static int   dsp_logbuf_len = 0;

static time_t log_last_rotate = 0;

static void *gzip_thread(void *arg)
{
    char *path = (char *)arg;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "gzip -f -- '%s'", path);
    system(cmd);
    free(path);
    return NULL;
}

void logging_init(void)
{
    log_stream = stderr;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    log_last_rotate = tv.tv_sec;

    if (g_config.logfileinterval > 0) {
        if (g_config.log_dir[0])
            snprintf(log_filename, sizeof(log_filename),
                     "%s/log-%ld.txt", g_config.log_dir, (long)tv.tv_sec);
        else
            snprintf(log_filename, sizeof(log_filename),
                     "log/log-%ld.txt", (long)tv.tv_sec);
        FILE *f = fopen(log_filename, "a");
        if (!f) {
            fprintf(stderr, "Can't open logfile for writing: %s\n", log_filename);
            fflush(stderr);
        } else {
            log_stream = f;
        }
    }

    struct tm tm_info;
    localtime_r(&tv.tv_sec, &tm_info);
    snprintf(log_dest, sizeof(log_dest), "%02d:%02d:%02d",
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
}

void logging_rotate(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm tm_info;
    localtime_r(&tv.tv_sec, &tm_info);
    snprintf(log_dest, sizeof(log_dest), "%02d:%02d:%02d",
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);

    if (g_config.logfileinterval <= 0) return;
    if (log_stream == stderr) return;
    if ((tv.tv_sec - log_last_rotate) < g_config.logfileinterval) return;

    snprintf(log_prev_filename, sizeof(log_prev_filename), "%s", log_filename);

    log_last_rotate = tv.tv_sec;
    if (g_config.log_dir[0])
        snprintf(log_filename, sizeof(log_filename),
                 "%s/log-%ld.txt", g_config.log_dir, (long)tv.tv_sec);
    else
        snprintf(log_filename, sizeof(log_filename),
                 "log/log-%ld.txt", (long)tv.tv_sec);

    log_stream = freopen(log_filename, "a", log_stream);
    if (!log_stream) {
        fprintf(stderr, "Can't open logfile for writing: %s\n", log_filename);
        fflush(stderr);

        log_stream = fopen("/dev/null", "a");
        if (!log_stream) log_stream = stderr;
        return;
    }

    char *old = strdup(log_prev_filename);
    if (old) {
        pthread_t th;
        int rc = pthread_create(&th, NULL, gzip_thread, old);
        if (rc != 0) {
            fprintf(stderr, "Can't start thread for zipping logfile (%i)\n", rc);
            free(old);
        } else {
            pthread_detach(th);
        }
    }
}

void log_printf(const char *fmt, ...)
{
    if (!log_stream) log_stream = stderr;

    if (fmt[0] != '2')
        fprintf(log_stream, "%s, ", log_dest);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_stream, fmt, ap);
    va_end(ap);

    fflush(log_stream);
}

void log_printf_dsp(const char *fmt, ...)
{
    if (fmt[0] != '2' && dsp_logbuf_len <= (DSP_LOGBUF_SIZE - 20)) {
        dsp_logbuf_len += snprintf(dsp_logbuf + dsp_logbuf_len,
                                    (size_t)(DSP_LOGBUF_SIZE - dsp_logbuf_len),
                                    "%s, ", log_dest);
    }

    int avail = DSP_LOGBUF_SIZE - dsp_logbuf_len;
    if (avail <= 0) return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dsp_logbuf + dsp_logbuf_len, (size_t)avail, fmt, ap);
    va_end(ap);

    if (n < 0) {
        fprintf(stderr, "Error in log_printf_dsp()\n");
        return;
    }

    if (n >= avail) {

        strncpy(dsp_logbuf + DSP_LOGBUF_SIZE - 12, "\n[dropped]\n", 12);
        dsp_logbuf_len = DSP_LOGBUF_SIZE - 1;
    } else {
        dsp_logbuf_len += n;
        if (dsp_logbuf_len > DSP_LOGBUF_SIZE)
            dsp_logbuf_len = DSP_LOGBUF_SIZE;
    }
}

void log_flush_dsp(void)
{
    if (dsp_logbuf_len <= 0) return;

    char tmp[DSP_LOGBUF_SIZE + 1];
    int  len = dsp_logbuf_len;

    if (len > DSP_LOGBUF_SIZE) len = DSP_LOGBUF_SIZE;
    memcpy(tmp, dsp_logbuf, (size_t)len);
    tmp[len] = '\0';
    dsp_logbuf_len = 0;

    if (len == DSP_LOGBUF_SIZE - 1) {

        log_printf("lbi %i\n", len);
    }
    log_printf("2%s", tmp);
}
