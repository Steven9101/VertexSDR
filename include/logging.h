// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#ifndef LOGGING_H
#define LOGGING_H

#include <stdio.h>

extern FILE *log_stream;

extern char  log_dest[64];

void logging_init(void);
void logging_rotate(void);
void log_printf(const char *fmt, ...);
void log_printf_dsp(const char *fmt, ...);
void log_flush_dsp(void);

#endif
