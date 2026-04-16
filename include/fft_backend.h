// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#ifndef FFT_BACKEND_H
#define FFT_BACKEND_H

#include "band.h"
struct DemodTable;

void fft_backend_set_requested(int backend);
int fft_backend_get_active(void);
const char *fft_backend_name(int backend);

int fft_backend_init_global(void);
void fft_backend_shutdown_global(void);

int fft_backend_prepare_band(struct BandState *b, int fftlen, int fftlen2, unsigned int fftw_flags);
void fft_backend_destroy_band(struct BandState *b);

void fft_backend_execute_plan_fwd(struct BandState *b);
void fft_backend_execute_plan_fwd2(struct BandState *b);

int fft_backend_prepare_demod_ifft(struct DemodTable *dt, int fft_size, int c2r);
void fft_backend_destroy_demod_ifft(struct DemodTable *dt);
void fft_backend_execute_demod_ifft(struct DemodTable *dt);

#endif
