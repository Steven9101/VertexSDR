// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#ifndef AUDIO_H
#define AUDIO_H

#include "band.h"

int alsa_open_device(struct BandState *b);
int alsa_read_samples(struct BandState *b);

int rtltcp_open_device(struct BandState *b);
int rtltcp_read_samples(struct BandState *b);

int stdin_open_device(struct BandState *b);
int stdin_read_samples(struct BandState *b);
int stdin_get_measured_sps(void);
int stdin_stop_device(struct BandState *b);
void audio_reset_runtime_state(void);

#endif
