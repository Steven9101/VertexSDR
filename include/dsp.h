// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#ifndef DSP_H
#define DSP_H

#include <stdint.h>
#include <fftw3.h>
#include "band.h"

#define DT_AM_NARROW     0
#define DT_SSB_NARROW    1
#define DT_SUBBAND_1K    2
#define DT_UNUSED_5A0    3
#define DT_AM_WIDE       4
#define DT_UNUSED_660    5
#define DT_WIDEBAND_4K   6
#define DT_FM            7
#define DT_NB_128        8
#define DT_WBCW          9
#define DT_SSB_WIDE     10
#define DT_COUNT        11

struct Client;

struct DemodTable {
    void   (*demod_fn)(struct BandState *b, int tune_bin,
                       float *filter_buf, void *cs,
                       struct DemodTable *dt, int phase_flag);
    void   (*init_fn)(void *cs);
    int      half_size;
    int      audiolen;
    fftwf_complex *in_buf;
    fftwf_complex *out_cx_buf;
    fftwf_plan     plan;
    float         *out_r;
    float         *output;
    fftwf_complex *output_cx;
    double         power;
    double         sqrt_power;
    int            fft_backend;
    void          *vkfft_ifft;
};

enum {
    AMSYNC_LOCKED = 0,
    AMSYNC_LOCKING = 1,
    AMSYNC_UNLOCKED = 2
};

extern struct DemodTable g_demod_tables[DT_COUNT];

void dsp_load_wisdom(void);
void dsp_save_wisdom(void);
void dsp_init_demod_tables(void);
void dsp_destroy_demod_tables(void);
void dsp_init_band_demod_tables(struct BandState *b);
void dsp_destroy_band_demod_tables(struct BandState *b);
void dsp_apply_mode_selection(struct Client *c, struct BandState *b, int js_mode);
void compute_passband_filter(struct Client *c, struct BandState *b);

void dsp_process_fft_block(struct BandState *b);
void dsp_execute_spectrum_fft(struct BandState *b);
void dsp_dispatch_clients(struct BandState *b, int phase_flag);
void dsp_build_waterfall_pyramid(struct BandState *b);
void dsp_demod_amsync(struct BandState *b, int tune_bin,
                      float *filter_buf, struct Client *c,
                      struct DemodTable *dt, int phase_flag);

void dsp_demodulate(struct BandState *b, int tune_bin,
                    int lo_bin, int hi_bin,
                    int mode, float *out, int audiolen,
                    float *prev_re, float *prev_im,
                    float *am_dc, int phase_flag,
                    float *filter_buf,
                    int *out_samples,
                    float *fm_prev2_re, float *fm_prev2_im);

struct DspTimingStats {
    long long fft_us;
    long long waterfall_us;
    long long dispatch_us;
    long long stdin_us;
};
void dsp_get_and_reset_timing(struct DspTimingStats *out);
void dsp_add_stdin_us(long long us);

void dsp_noise_blanker(float *samples, int n, float threshold);
void dsp_iq_correct(float *samples, int n,
                    float *dc_i, float *dc_q, float alpha, float iq_gain_corr);

#endif
