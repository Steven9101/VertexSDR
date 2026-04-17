// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdio.h>

#ifdef BUILD_VERSION_STRING
#define VERSION_STRING BUILD_VERSION_STRING
#else
#define VERSION_STRING "unknown-64"
#endif

extern volatile int g_configreload;

#endif
