// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#include "logbook.h"
#include "config.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static char logbook_path[512];

static const char logbook_col_header[] =
    "date     UTC    freq   call           comments                        dxcc country        heard by\r\n";
static const char logbook_sep_header[] =
    "--------------------------------------------------------------------------------------------------\r\n";

void logbook_init(const char *pubdir)
{
    snprintf(logbook_path, sizeof(logbook_path), "%s/logbook.txt", pubdir);

    struct stat st;
    if (stat(logbook_path, &st) != 0) {
        FILE *f = fopen(logbook_path, "w");
        if (f) fclose(f);
    }
}

int logbook_handle_get(char *buf, int bufsize, int max_lines)
{
    int n = 0;

    {
        int hlen = (int)strlen(logbook_col_header);
        int slen = (int)strlen(logbook_sep_header);
        if (n + hlen < bufsize) { memcpy(buf + n, logbook_col_header, (size_t)hlen); n += hlen; }
        if (n + slen < bufsize) { memcpy(buf + n, logbook_sep_header, (size_t)slen); n += slen; }
    }

    FILE *f = fopen(logbook_path, "r");
    if (!f) return n;

    if (max_lines <= 0) {

        char line[256];
        while (n < bufsize - 256) {
            if (!fgets(line, sizeof(line), f)) break;
            int len = (int)strlen(line);
            if (n + len >= bufsize) break;
            memcpy(buf + n, line, (size_t)len);
            n += len;
        }
        fclose(f);
        return n;
    }

    fseeko(f, 0, SEEK_END);
    off_t file_size = ftello(f);

    off_t start_pos = 0;

    if (file_size > 0) {

        off_t pos = file_size;
        int   lines_found = 0;
        char  chunk[128];

        while (pos > 0 && lines_found < max_lines) {
            off_t chunk_start = pos - 127;
            if (chunk_start < 0) chunk_start = 0;
            int chunk_len = (int)(pos - chunk_start);

            fseeko(f, chunk_start, SEEK_SET);
            if (fread(chunk, 1, (size_t)chunk_len, f) != (size_t)chunk_len) break;
            chunk[chunk_len] = '\0';

            for (int i = chunk_len - 1; i >= 0 && lines_found < max_lines; i--) {
                if (chunk[i] == '\n') {

                    int line_start = i + 1;

                    const char *lp = chunk + line_start;
                    int llen = chunk_len - line_start;
                    if (llen > 0) {

                        if (memcmp(lp, "date ", 5) != 0 && memcmp(lp, "-----", 5) != 0)
                            lines_found++;
                        if (lines_found >= max_lines) {

                            start_pos = chunk_start + line_start;
                        }
                    }
                }
            }
            pos = chunk_start;
        }

        if (lines_found < max_lines)
            start_pos = 0;
    }

    fseeko(f, start_pos, SEEK_SET);
    int lines_out = 0;
    char line[256];
    while (lines_out < max_lines && n < bufsize - 256) {
        if (!fgets(line, sizeof(line), f)) break;
        if (feof(f) && line[0] == '\0') break;

        if (memcmp(line, "date ", 5) == 0 || memcmp(line, "-----", 5) == 0) {
            lines_out++;
            if (lines_out >= max_lines) break;
            continue;
        }
        int len = (int)strlen(line);
        if (n + len >= bufsize) break;
        memcpy(buf + n, line, (size_t)len);
        n += len;
        lines_out++;
    }
    fclose(f);
    return n;
}

int logbook_handle_insert(const char *call, const char *comment,
                          const char *freq, const char *name)
{
    if (!call || !call[0]) return -1;

    FILE *f = fopen(logbook_path, "a");
    if (!f) return -1;

    time_t now = time(NULL);
    struct tm tp;
    gmtime_r(&now, &tp);

    double freq_khz = freq ? strtod(freq, NULL) : 0.0;

    fprintf(f, "%04d%02d%02d %02d:%02d %9.1f %-10.16s %-32.32s , %-4.4s%-16.16s %s\r\n",
            tp.tm_year + 1900, tp.tm_mon + 1, tp.tm_mday,
            tp.tm_hour, tp.tm_min,
            freq_khz,
            call,
            comment ? comment : "",
            "",
            "",
            name ? name : "?");
    fclose(f);

    log_printf("logbook: %s at %.1f kHz by %s\n", call, freq_khz, name ? name : "?");
    return 0;
}
