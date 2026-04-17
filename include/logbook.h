// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#ifndef LOGBOOK_H
#define LOGBOOK_H

void logbook_init(const char *pubdir);

int logbook_handle_get(char *buf, int bufsize, int max_lines);

int logbook_handle_insert(const char *call, const char *comment,
                          const char *freq, const char *name);

#endif
