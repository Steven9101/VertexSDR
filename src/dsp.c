// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#include "dsp.h"
#include "band.h"
#include "client.h"
#include "common.h"
#include "config.h"
#include "fft_backend.h"
#include "waterfall.h"
#include "worker.h"
#ifdef USE_VULKAN
#include "vk_waterfall.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fftw3.h>
#include <sys/time.h>

static long long fft_exec_count   = 0;
static long long fft_exec_usec    = 0;
static long long demod_exec_count = 0;
static long long demod_avg_usec   = 0;

static long long g_timing_fft_us;
static long long g_timing_wf_us;
static long long g_timing_dispatch_us;
static long long g_timing_stdin_us;

static inline float wrap_phasef(float x)
{
    while (x > (float)M_PI) x -= (float)(2.0 * M_PI);
    while (x < (float)-M_PI) x += (float)(2.0 * M_PI);
    return x;
}

static inline double clampd(double x, double lo, double hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

void dsp_get_and_reset_timing(struct DspTimingStats *out)
{
    out->fft_us       = __sync_lock_test_and_set(&g_timing_fft_us, 0);
    out->waterfall_us = __sync_lock_test_and_set(&g_timing_wf_us, 0);
    out->dispatch_us  = __sync_lock_test_and_set(&g_timing_dispatch_us, 0);
    out->stdin_us     = __sync_lock_test_and_set(&g_timing_stdin_us, 0);
}

void dsp_add_stdin_us(long long us)
{
    __sync_fetch_and_add(&g_timing_stdin_us, us);
}

#include "filter_table.h"

struct DemodTable g_demod_tables[DT_COUNT];

void dsp_load_wisdom(void)
{
    FILE *f = fopen("fftw-wisdom.dat", "r");
    if (f) { fftwf_import_wisdom_from_file(f); fclose(f); }
}

void dsp_save_wisdom(void)
{
    FILE *f = fopen("fftw-wisdom.dat", "w");
    if (f) { fftwf_export_wisdom_to_file(f); fclose(f); }
}

static void demod_ssb_narrow(struct BandState *b, int tune_bin,
                             float *filter_buf, void *cs,
                             struct DemodTable *dt, int phase_flag);
static void demod_ssb_wide(struct BandState *b, int tune_bin,
                           float *filter_buf, void *cs,
                           struct DemodTable *dt, int phase_flag);
static void demod_am_narrow(struct BandState *b, int tune_bin,
                            float *filter_buf, void *cs,
                            struct DemodTable *dt, int phase_flag);
static void demod_am_wide(struct BandState *b, int tune_bin,
                          float *filter_buf, void *cs,
                          struct DemodTable *dt, int phase_flag);
static void demod_amsync_narrow(struct BandState *b, int tune_bin,
                                float *filter_buf, void *cs,
                                struct DemodTable *dt, int phase_flag);
static void demod_amsync_wide(struct BandState *b, int tune_bin,
                              float *filter_buf, void *cs,
                              struct DemodTable *dt, int phase_flag);
static void demod_fm(struct BandState *b, int tune_bin,
                     float *filter_buf, void *cs,
                     struct DemodTable *dt, int phase_flag);
static void demod_subband(struct BandState *b, int tune_bin,
                          float *filter_buf, void *cs,
                          struct DemodTable *dt, int phase_flag);
static void init_ssb_state(void *cs);
static void init_am_state(void *cs);
static void init_amsync_state(void *cs);
static void init_fm_state(void *cs);
static void init_noop(void *cs);

static unsigned int fftw_effort_flags(void)
{

    switch (fftplaneffort) {
        case 1: return FFTW_MEASURE;
        case 2: return FFTW_PATIENT;
        case 3: return FFTW_EXHAUSTIVE;
        default: return FFTW_ESTIMATE;
    }
}

void dsp_init_demod_tables(void)
{
    unsigned int flags = fftw_effort_flags();
    struct DemodTable *dt;

    memset(g_demod_tables, 0, sizeof(g_demod_tables));

    fprintf(stderr, "Planning demod FFTs (effort=%u)...", flags);
    fflush(stderr);

    dt = &g_demod_tables[DT_SSB_NARROW];
    dt->demod_fn  = demod_ssb_narrow;
    dt->init_fn   = init_ssb_state;
    dt->half_size = 128;
    dt->audiolen  = 128;
    dt->in_buf    = (fftwf_complex *)malloc(0x800);
    dt->out_r     = (float *)malloc(0x400);
    dt->out_cx_buf = NULL;
    dt->plan = fftwf_plan_dft_c2r_1d(256, dt->in_buf, dt->out_r, flags);
    fft_backend_prepare_demod_ifft(dt, 256, 1);
    fprintf(stderr, " SSB-N"); fflush(stderr);

    dt = &g_demod_tables[DT_SSB_WIDE];
    dt->demod_fn  = demod_ssb_wide;
    dt->init_fn   = init_ssb_state;
    dt->half_size = 256;
    dt->audiolen  = 256;
    dt->in_buf    = (fftwf_complex *)malloc(0x1000);
    dt->out_r     = (float *)malloc(0x800);
    dt->out_cx_buf = NULL;
    dt->plan = fftwf_plan_dft_c2r_1d(512, dt->in_buf, dt->out_r, flags);
    fft_backend_prepare_demod_ifft(dt, 512, 1);
    fprintf(stderr, " SSB-W"); fflush(stderr);

    dt = &g_demod_tables[DT_AM_NARROW];
    dt->demod_fn  = demod_am_narrow;
    dt->init_fn   = init_am_state;
    dt->half_size = 128;
    dt->audiolen  = 128;
    dt->output    = (float *)calloc(0x200, 1);
    dt->in_buf    = (fftwf_complex *)calloc(0x800, 1);
    dt->out_cx_buf = (fftwf_complex *)calloc(0x800, 1);
    dt->out_r     = NULL;
    dt->plan = fftwf_plan_dft_1d(256, dt->in_buf, dt->out_cx_buf, FFTW_BACKWARD, flags);
    fft_backend_prepare_demod_ifft(dt, 256, 0);
    fprintf(stderr, " AM-N"); fflush(stderr);

    dt = &g_demod_tables[DT_AM_WIDE];
    dt->demod_fn  = demod_am_wide;
    dt->init_fn   = init_am_state;
    dt->half_size = 256;
    dt->audiolen  = 256;
    dt->output    = (float *)calloc(0x400, 1);
    dt->in_buf    = (fftwf_complex *)calloc(0x1000, 1);
    dt->out_cx_buf = (fftwf_complex *)calloc(0x1000, 1);
    dt->out_r     = NULL;
    dt->plan = fftwf_plan_dft_1d(512, dt->in_buf, dt->out_cx_buf, FFTW_BACKWARD, flags);
    fft_backend_prepare_demod_ifft(dt, 512, 0);
    fprintf(stderr, " AM-W"); fflush(stderr);

    dt = &g_demod_tables[DT_FM];
    dt->demod_fn  = demod_fm;
    dt->init_fn   = init_fm_state;
    dt->half_size = 512;
    dt->audiolen  = 128;
    dt->output    = (float *)malloc(0x200);
    dt->in_buf    = (fftwf_complex *)calloc(0x2000, 1);
    dt->out_cx_buf = (fftwf_complex *)calloc(0x2000, 1);
    dt->out_r     = NULL;
    dt->plan = fftwf_plan_dft_1d(1024, dt->in_buf, dt->out_cx_buf,
                                  FFTW_BACKWARD, flags);
    fft_backend_prepare_demod_ifft(dt, 1024, 0);
    fprintf(stderr, " FM"); fflush(stderr);

    dt = &g_demod_tables[DT_SUBBAND_1K];
    dt->demod_fn  = demod_subband;
    dt->init_fn   = init_noop;
    dt->half_size = 512;
    dt->audiolen  = 512;
    dt->in_buf    = (fftwf_complex *)calloc(0x2000, 1);
    dt->out_cx_buf = (fftwf_complex *)calloc(0x2000, 1);
    dt->out_r     = NULL;
    dt->plan = fftwf_plan_dft_1d(1024, dt->in_buf, dt->out_cx_buf,
                                  FFTW_BACKWARD, flags);
    fft_backend_prepare_demod_ifft(dt, 1024, 0);
    fprintf(stderr, " SB1K"); fflush(stderr);

    dt = &g_demod_tables[DT_WIDEBAND_4K];
    dt->demod_fn  = demod_subband;
    dt->init_fn   = init_noop;
    dt->half_size = 2048;
    dt->audiolen  = 2048;
    dt->in_buf    = (fftwf_complex *)calloc(0x8000, 1);
    dt->out_cx_buf = (fftwf_complex *)calloc(0x8000, 1);
    dt->out_r     = NULL;
    dt->plan = fftwf_plan_dft_1d(4096, dt->in_buf, dt->out_cx_buf,
                                  FFTW_BACKWARD, flags);
    fft_backend_prepare_demod_ifft(dt, 4096, 0);
    fprintf(stderr, " WB4K"); fflush(stderr);

    dt = &g_demod_tables[DT_NB_128];
    dt->demod_fn  = demod_subband;
    dt->init_fn   = init_noop;
    dt->half_size = 64;
    dt->audiolen  = 64;
    dt->in_buf    = (fftwf_complex *)calloc(0x400, 1);
    dt->out_cx_buf = (fftwf_complex *)calloc(0x400, 1);
    dt->out_r     = NULL;
    dt->plan = fftwf_plan_dft_1d(128, dt->in_buf, dt->out_cx_buf,
                                  FFTW_BACKWARD, flags);
    fft_backend_prepare_demod_ifft(dt, 128, 0);
    fprintf(stderr, " NB128"); fflush(stderr);

    dt = &g_demod_tables[DT_WBCW];
    dt->demod_fn  = demod_subband;
    dt->init_fn   = g_demod_tables[DT_AM_NARROW].init_fn;
    dt->half_size = g_demod_tables[DT_AM_NARROW].half_size;
    dt->audiolen  = g_demod_tables[DT_AM_NARROW].audiolen;
    dt->in_buf    = g_demod_tables[DT_AM_NARROW].in_buf;
    dt->out_cx_buf = g_demod_tables[DT_AM_NARROW].out_cx_buf;
    dt->out_r     = NULL;
    dt->plan      = g_demod_tables[DT_AM_NARROW].plan;
    dt->fft_backend = g_demod_tables[DT_AM_NARROW].fft_backend;
    dt->vkfft_ifft  = g_demod_tables[DT_AM_NARROW].vkfft_ifft;

    fprintf(stderr, " done.\n");
}

void dsp_destroy_demod_tables(void)
{
    for (int i = 0; i < DT_COUNT; i++) {
        struct DemodTable *dt = &g_demod_tables[i];
        if (i == DT_WBCW) continue;
        if (i == DT_UNUSED_5A0 || i == DT_UNUSED_660) continue;
        fft_backend_destroy_demod_ifft(dt);
        if (dt->plan) { fftwf_destroy_plan(dt->plan); dt->plan = NULL; }
        free(dt->in_buf);     dt->in_buf = NULL;

        if (dt->output && dt->out_r &&
            (char *)dt->output >= (char *)dt->out_r &&
            (char *)dt->output <  (char *)dt->out_r + 0x1000) {
            dt->output = NULL;
        }
        free(dt->out_r);      dt->out_r = NULL;
        free(dt->out_cx_buf); dt->out_cx_buf = NULL;
        free(dt->output);     dt->output = NULL;
    }
}

static void init_ssb_state(void *cs) { (void)cs; }
static void init_am_state(void *cs) { (void)cs; }
static void init_amsync_state(void *cs)
{
    struct Client *c = (struct Client *)cs;
    if (!c) return;
    c->am_dc = 0.0f;
    c->amsync_dc = 0.0f;
    c->amsync_phase = 0.0f;
    c->amsync_freq = 0.0f;
    c->amsync_acq_freq = 0.0f;
    c->amsync_state = AMSYNC_UNLOCKED;
    c->amsync_lock_count = 0;
    c->amsync_coherence_ema = 0.0;
    c->amsync_truefreq_mhz = 0;
}
static void init_fm_state(void *cs) { (void)cs; }

static int round_to_pow2(int n)
{
    if (n <= 0) return 1;
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

static int next_fftw_friendly(int n)
{
    if (n <= 1) return 1;

    int best = round_to_pow2(n);
    for (int a = 1; a <= 2 * n; a *= 2)
        for (int b = a; b <= 2 * n; b *= 3)
            for (int c = b; c <= 2 * n; c *= 5)
                for (int d = c; d <= 2 * n; d *= 7) {
                    if (abs(d - n) < abs(best - n))
                        best = d;
                }
    return best;
}

static void init_demod_entry(struct DemodTable *dt, int dt_idx, int half_size,
                             int audiolen_override)
{
    unsigned int flags = fftw_effort_flags();
    int fft_size = 2 * half_size;

    memset(dt, 0, sizeof(*dt));
    dt->half_size = half_size;

    switch (dt_idx) {
    case DT_SSB_NARROW:
        dt->demod_fn = demod_ssb_narrow;
        dt->init_fn  = init_ssb_state;
        dt->audiolen = half_size;
        dt->in_buf   = (fftwf_complex *)malloc(sizeof(fftwf_complex) * fft_size);
        dt->out_r    = (float *)malloc(sizeof(float) * fft_size);
        dt->out_cx_buf = NULL;
        dt->plan = fftwf_plan_dft_c2r_1d(fft_size, dt->in_buf, dt->out_r, flags);
        fft_backend_prepare_demod_ifft(dt, fft_size, 1);
        break;

    case DT_SSB_WIDE:
        dt->demod_fn = demod_ssb_wide;
        dt->init_fn  = init_ssb_state;
        dt->audiolen = half_size;
        dt->in_buf   = (fftwf_complex *)malloc(sizeof(fftwf_complex) * fft_size);
        dt->out_r    = (float *)malloc(sizeof(float) * fft_size);
        dt->out_cx_buf = NULL;
        dt->plan = fftwf_plan_dft_c2r_1d(fft_size, dt->in_buf, dt->out_r, flags);
        fft_backend_prepare_demod_ifft(dt, fft_size, 1);
        break;

    case DT_AM_NARROW:
        dt->demod_fn = demod_am_narrow;
        dt->init_fn  = init_am_state;
        dt->audiolen = half_size;
        dt->output   = (float *)calloc(fft_size, sizeof(float));
        dt->in_buf   = (fftwf_complex *)calloc(fft_size, sizeof(fftwf_complex));
        dt->out_cx_buf = (fftwf_complex *)calloc(fft_size, sizeof(fftwf_complex));
        dt->out_r    = NULL;
        dt->plan = fftwf_plan_dft_1d(fft_size, dt->in_buf, dt->out_cx_buf,
                                      FFTW_BACKWARD, flags);
        fft_backend_prepare_demod_ifft(dt, fft_size, 0);
        break;

    case DT_AM_WIDE:
        dt->demod_fn = demod_am_wide;
        dt->init_fn  = init_am_state;
        dt->audiolen = half_size;
        dt->output   = (float *)calloc(fft_size, sizeof(float));
        dt->in_buf   = (fftwf_complex *)calloc(fft_size, sizeof(fftwf_complex));
        dt->out_cx_buf = (fftwf_complex *)calloc(fft_size, sizeof(fftwf_complex));
        dt->out_r    = NULL;
        dt->plan = fftwf_plan_dft_1d(fft_size, dt->in_buf, dt->out_cx_buf,
                                      FFTW_BACKWARD, flags);
        fft_backend_prepare_demod_ifft(dt, fft_size, 0);
        break;

    case DT_FM:
        dt->demod_fn = demod_fm;
        dt->init_fn  = init_fm_state;
        dt->audiolen = (audiolen_override > 0) ? audiolen_override : half_size / 4;
        if (dt->audiolen < 1) dt->audiolen = 1;
        dt->output   = (float *)malloc(sizeof(float) * fft_size);
        dt->in_buf   = (fftwf_complex *)calloc(fft_size, sizeof(fftwf_complex));
        dt->out_cx_buf = (fftwf_complex *)calloc(fft_size, sizeof(fftwf_complex));
        dt->out_r    = NULL;
        dt->plan = fftwf_plan_dft_1d(fft_size, dt->in_buf, dt->out_cx_buf,
                                      FFTW_BACKWARD, flags);
        fft_backend_prepare_demod_ifft(dt, fft_size, 0);
        break;

    case DT_SUBBAND_1K:
        dt->demod_fn = demod_subband;
        dt->init_fn  = init_noop;
        dt->audiolen = half_size;
        dt->in_buf   = (fftwf_complex *)calloc(fft_size, sizeof(fftwf_complex));
        dt->out_cx_buf = (fftwf_complex *)calloc(fft_size, sizeof(fftwf_complex));
        dt->out_r    = NULL;
        dt->plan = fftwf_plan_dft_1d(fft_size, dt->in_buf, dt->out_cx_buf,
                                      FFTW_BACKWARD, flags);
        fft_backend_prepare_demod_ifft(dt, fft_size, 0);
        break;

    case DT_WIDEBAND_4K:
        dt->demod_fn = demod_subband;
        dt->init_fn  = init_noop;
        dt->audiolen = half_size;
        dt->in_buf   = (fftwf_complex *)calloc(fft_size, sizeof(fftwf_complex));
        dt->out_cx_buf = (fftwf_complex *)calloc(fft_size, sizeof(fftwf_complex));
        dt->out_r    = NULL;
        dt->plan = fftwf_plan_dft_1d(fft_size, dt->in_buf, dt->out_cx_buf,
                                      FFTW_BACKWARD, flags);
        fft_backend_prepare_demod_ifft(dt, fft_size, 0);
        break;

    case DT_NB_128:
        dt->demod_fn = demod_subband;
        dt->init_fn  = init_noop;
        dt->audiolen = half_size;
        dt->in_buf   = (fftwf_complex *)calloc(fft_size, sizeof(fftwf_complex));
        dt->out_cx_buf = (fftwf_complex *)calloc(fft_size, sizeof(fftwf_complex));
        dt->out_r    = NULL;
        dt->plan = fftwf_plan_dft_1d(fft_size, dt->in_buf, dt->out_cx_buf,
                                      FFTW_BACKWARD, flags);
        fft_backend_prepare_demod_ifft(dt, fft_size, 0);
        break;

    default:
        break;
    }
}

static void destroy_demod_entry(struct DemodTable *dt, int dt_idx)
{
    if (dt_idx == DT_WBCW) return;
    if (dt_idx == DT_UNUSED_5A0 || dt_idx == DT_UNUSED_660) return;

    fft_backend_destroy_demod_ifft(dt);
    if (dt->plan) { fftwf_destroy_plan(dt->plan); dt->plan = NULL; }
    free(dt->in_buf); dt->in_buf = NULL;

    if (dt->output && dt->out_r &&
        (char *)dt->output >= (char *)dt->out_r &&
        (char *)dt->output <  (char *)dt->out_r + sizeof(float) * 4096) {
        dt->output = NULL;
    }
    free(dt->out_r);      dt->out_r = NULL;
    free(dt->out_cx_buf); dt->out_cx_buf = NULL;
    free(dt->output);     dt->output = NULL;
}

void dsp_init_band_demod_tables(struct BandState *b)
{

    const double ref_bin_width = 31.25;

    double actual_bin_width = (double)b->samplerate / (double)b->fftlen;
    double scale = ref_bin_width / actual_bin_width;

    if (scale > 0.95 && scale < 1.05) {
        b->band_demod_tables = g_demod_tables;
        b->band_demod_tables_owned = 0;
        fprintf(stderr, "Band %d: demod tables aliased to global (scale=%.3f)\n",
                b->band_index, scale);
        return;
    }

    struct DemodTable *dt_arr = (struct DemodTable *)calloc(DT_COUNT, sizeof(struct DemodTable));
    if (!dt_arr) {
        fprintf(stderr, "Band %d: failed to allocate per-band demod tables, using global\n",
                b->band_index);
        b->band_demod_tables = g_demod_tables;
        b->band_demod_tables_owned = 0;
        return;
    }

    fprintf(stderr, "Band %d: computing per-band demod tables (scale=%.3f, bin_width=%.2f Hz)...",
            b->band_index, scale, actual_bin_width);
    fflush(stderr);

    struct { int dt_idx; int ref_half; int ref_audiolen; int need_pow2; } modes[] = {
        { DT_SSB_NARROW,  128,   0, 0 },
        { DT_SSB_WIDE,    256,   0, 0 },
        { DT_AM_NARROW,   128,   0, 1 },
        { DT_AM_WIDE,     256,   0, 1 },
        { DT_FM,          512, 128, 1 },
        { DT_SUBBAND_1K,  512,   0, 1 },
        { DT_WIDEBAND_4K, 2048,  0, 1 },
        { DT_NB_128,       64,   0, 1 },
    };
    int n_modes = sizeof(modes) / sizeof(modes[0]);

    for (int i = 0; i < n_modes; i++) {
        int target = (int)(modes[i].ref_half * scale + 0.5);
        if (target < 64) target = 64;

        if (target > 512) target = 512;

        int half_size;
        if (modes[i].need_pow2) {
            half_size = round_to_pow2(target);

            if (half_size > target && half_size / 2 >= 64) {
                int lo = half_size / 2;
                if (target - lo < half_size - target)
                    half_size = lo;
            }
        } else {
            half_size = next_fftw_friendly(target);
        }

        if (half_size < 4) half_size = 4;

        int audiolen_override = 0;
        if (modes[i].ref_audiolen > 0) {
            audiolen_override = (int)(modes[i].ref_audiolen * scale + 0.5);
            if (audiolen_override < 1) audiolen_override = 1;
            if (audiolen_override > half_size) audiolen_override = half_size;
        }

        init_demod_entry(&dt_arr[modes[i].dt_idx], modes[i].dt_idx, half_size,
                         audiolen_override);

        int al = dt_arr[modes[i].dt_idx].audiolen;
        double bw = half_size * actual_bin_width;
        fprintf(stderr, " [%d]hs=%d,al=%d(bw=%.0fHz)", modes[i].dt_idx, half_size, al, bw);
    }

    dt_arr[DT_WBCW] = dt_arr[DT_AM_NARROW];
    dt_arr[DT_WBCW].demod_fn = demod_subband;

    b->band_demod_tables = dt_arr;
    b->band_demod_tables_owned = 1;

    fprintf(stderr, " done.\n");
}

void dsp_destroy_band_demod_tables(struct BandState *b)
{
    if (!b->band_demod_tables_owned) {
        b->band_demod_tables = NULL;
        return;
    }

    if (b->band_demod_tables) {
        for (int i = 0; i < DT_COUNT; i++)
            destroy_demod_entry(&b->band_demod_tables[i], i);
        free(b->band_demod_tables);
        b->band_demod_tables = NULL;
    }
    b->band_demod_tables_owned = 0;
}
static void init_noop(void *cs) { (void)cs; }

static void autonotch_apply(int half_size, fftwf_complex *in_buf,
                            struct Client *c, unsigned int peak_bin)
{
    int tracked_bin = (int)peak_bin;

    if (c->notch_tracked_bin == tracked_bin) {
        int consec_count = c->notch_consec_count;
        if (consec_count <= 4) c->notch_consec_count = consec_count + 1;
    } else {
        c->notch_consec_count = 0;
        c->notch_tracked_bin  = tracked_bin;
    }

    int result = c->notch_confirmed_bin;
    if (tracked_bin == result) {
        if (c->notch_hold_counter > 4)
            c->notch_hold_counter = 10;
    } else {
        int hold_counter = c->notch_hold_counter;
        if (hold_counter) {
            c->notch_hold_counter = hold_counter - 1;
        } else {
            c->notch_confirmed_bin = 0;
            if (c->notch_consec_count <= 4) return;
            c->notch_hold_counter  = 10;
            c->notch_confirmed_bin = tracked_bin;
            result = tracked_bin;
        }
    }

    if (!result) {
        if (c->notch_consec_count <= 4) return;
        c->notch_hold_counter  = 10;
        c->notch_confirmed_bin = tracked_bin;
        result = tracked_bin;
    }

    if ((unsigned int)(result + 3) > 6) {
        if (abs(result) < half_size - 4) {
            int idx = result & (2 * half_size - 1);
            fftwf_complex *base = in_buf;

            base[idx][0] = 0.0f;
            base[idx][1] = 0.0f;

            int bw = idx - 1; if (bw < 0) bw += 2 * half_size;
            int fw = (idx + 1) % (2 * half_size);
            base[bw][0] *= 0.035f; base[bw][1] *= 0.035f;
            base[fw][0] *= 0.035f; base[fw][1] *= 0.035f;

            bw = (idx - 2 + 2*half_size) % (2*half_size);
            fw = (idx + 2) % (2*half_size);
            base[bw][0] *= 0.355f; base[bw][1] *= 0.355f;
            base[fw][0] *= 0.355f; base[fw][1] *= 0.355f;

            bw = (idx - 3 + 2*half_size) % (2*half_size);
            fw = (idx + 3) % (2*half_size);
            base[bw][0] *= 0.806f; base[bw][1] *= 0.806f;
            base[fw][0] *= 0.806f; base[fw][1] *= 0.806f;

            bw = (idx - 4 + 2*half_size) % (2*half_size);
            fw = (idx + 4) % (2*half_size);
            base[bw][0] *= 0.984f; base[bw][1] *= 0.984f;
            base[fw][0] *= 0.984f; base[fw][1] *= 0.984f;
        }
    }
}

static void demod_ssb_core(struct BandState *b, int tune_bin,
                           float *filter_buf, struct Client *c,
                           struct DemodTable *dt, int phase_flag, int wideband_mode)
{
    int half = dt->half_size;
    float *spec = (float *)b->fft_out_r2c;
    fftwf_complex *din  = dt->in_buf;

    unsigned int peak_bin = 0;
    double peak_power = 0.0;

    if (half > 1) {
        float *fwd  = spec + 2 * tune_bin + 2;
        float *bwd  = spec + 2 * tune_bin - 2;
        float *filt = filter_buf + 512;

        for (int k = 1; k < half; k++) {
            float f_pos = filt[k];
            float f_neg = filt[-k];

            float re = fwd[0] * f_pos + bwd[0] * f_neg;
            float im = fwd[1] * f_pos - bwd[1] * f_neg;

            din[k][0] = re;
            din[k][1] = im;

            double pw = (double)(re * re + im * im);
            if (pw > peak_power) {
                peak_power = pw;
                peak_bin = (unsigned int)k;
            }
            fwd += 2;
            bwd -= 2;
        }
    }

    if (c && c->demod_autonotch && peak_bin > 0)
        autonotch_apply(half, din, c, peak_bin);

    din[0][0] = 0.0f; din[0][1] = 0.0f;
    din[half][0] = 0.0f; din[half][1] = 0.0f;

    fft_backend_execute_demod_ifft(dt);

    int offset = phase_flag ? 0 : half;
    float *out = dt->out_r + offset;
    dt->output = out;
    dt->audiolen = half;

    double sum_sq = 0.0;
    for (int i = 0; i < half; i++) {
        float s = out[i];
        sum_sq += (double)(s * s);
    }

    dt->power = sum_sq;

    dt->sqrt_power = (half > 0) ? sqrt(sum_sq * (128.0 / (double)half)) : 0.0;

    if (wideband_mode)
        dt->power *= 0.5;
}

static void demod_ssb_narrow(struct BandState *b, int tune_bin,
                             float *filter_buf, void *cs,
                             struct DemodTable *dt, int phase_flag)
{
    demod_ssb_core(b, tune_bin, filter_buf, (struct Client *)cs, dt, phase_flag, 0);
}

static void demod_ssb_wide(struct BandState *b, int tune_bin,
                           float *filter_buf, void *cs,
                           struct DemodTable *dt, int phase_flag)
{
    demod_ssb_core(b, tune_bin, filter_buf, (struct Client *)cs, dt, phase_flag, 1);
}

static void demod_am_core(struct BandState *b, int tune_bin,
                          float *filter_buf, struct Client *c,
                          struct DemodTable *dt, int phase_flag, int wideband_mode)
{
    int half = dt->half_size;
    float *spec = (float *)b->fft_out_r2c;
    fftwf_complex *din  = dt->in_buf;
    int N = b->fftlen;
    int mask = 2 * half - 1;

    unsigned int peak_bin = 0;
    double peak_power = 0.0;

    int first_bin = 1 - half;
    if (half > first_bin) {
        int src_bin = first_bin + tune_bin;
        float *filt = filter_buf + 512 + first_bin;

        for (int k = first_bin; k < half; k++) {
            int dst = k & mask;
            if (src_bin >= 0 && src_bin < N) {
                float *sp = spec + 2 * src_bin;
                float fw = *filt;
                din[dst][0] = sp[0] * fw;
                din[dst][1] = sp[1] * fw;

                if ((unsigned int)(k + 10) > 20u) {
                    double pw = (double)(din[dst][0] * din[dst][0]
                                      + din[dst][1] * din[dst][1]);
                    if (pw > peak_power) {
                        peak_power = pw;
                        peak_bin = (unsigned int)(dst);
                    }
                }
            } else {
                din[dst][0] = 0.0f;
                din[dst][1] = 0.0f;
            }
            ++src_bin;
            ++filt;
        }
    }

    if (c && c->demod_autonotch && peak_bin > 0)
        autonotch_apply(half, din, c, peak_bin);

    fft_backend_execute_demod_ifft(dt);

    fftwf_complex *out_cx = dt->out_cx_buf;
    int cx_offset = (phase_flag) ? 0 : half;

    float dc = c ? c->am_dc : 0.0f;
    double sum_sq = 0.0;
    float max_env = 0.0f;

    float *audio_out = dt->output;
    if (!audio_out) return;

    for (int i = 0; i < half; i++) {
        float re = out_cx[cx_offset + i][0];
        float im = out_cx[cx_offset + i][1];
        float mag_sq = re * re + im * im;
        sum_sq += (double)mag_sq;
        float env = sqrtf(mag_sq);

        float diff = env - dc;
        dc = (float)((double)dc + (double)diff * 0.01);
        float sample = env - dc;

        if (dt->output)
            dt->output[i] = sample;

        if (sample > max_env)
            max_env = sample;
    }

    if (c) c->am_dc = dc;

    sum_sq *= 2.0;
    dt->power = sum_sq;
    dt->sqrt_power = (double)(max_env + max_env);

    if (wideband_mode)
        dt->power *= 0.5;
}

static float wrap_pi(float x) {
    while (x >  M_PI) x -= 2.0f * M_PI;
    while (x < -M_PI) x += 2.0f * M_PI;
    return x;
}

static void demod_amsync_core(struct BandState *b, int tune_bin,
                              float *filter_buf, struct Client *c,
                              struct DemodTable *dt, int phase_flag, int wideband_mode)
{
    (void)wideband_mode;
    int half = dt->half_size;
    float *spec = (float *)b->fft_out_r2c;
    fftwf_complex *din = dt->in_buf;
    int N = b->fftlen;
    int mask = 2 * half - 1;

    unsigned int peak_bin = 0;
    double peak_power = 0.0;
    int peak_k = 0;

    int k0 = 1 - half;
    for (int k = k0; k < half; k++) {
        int dst = k & mask;
        int src_bin = k + tune_bin;
        float fw = (filter_buf + 512)[k];

        if (src_bin >= 0 && src_bin < N) {
            float *sp = spec + 2 * src_bin;
            din[dst][0] = sp[0] * fw;
            din[dst][1] = sp[1] * fw;

            double dist_weight = 1.0 / (1.0 + 0.05 * abs(k));
            double pw = (double)(din[dst][0] * din[dst][0] + din[dst][1] * din[dst][1]) * dist_weight;

            if (pw > peak_power) {
                peak_power = pw;
                peak_bin = dst;
                peak_k = k;
            }
        } else {
            din[dst][0] = 0.0f;
            din[dst][1] = 0.0f;
        }
    }

    if (c && c->demod_autonotch && peak_power > 0)
        autonotch_apply(half, din, c, peak_bin);

    fft_backend_execute_demod_ifft(dt);

    fftwf_complex *out_cx = dt->out_cx_buf;
    int cx_offset = phase_flag ? 0 : half;
    float *audio_out = dt->output;
    if (!audio_out) return;

    double sample_rate = (double)dt->audiolen * 2.0 * ((double)b->samplerate / (double)b->fftlen_real);
    if (b->noniq) sample_rate *= 2.0;
    double hz_per_rad = sample_rate / (2.0 * M_PI);

    float phase = c ? c->amsync_phase : 0.0f;
    float freq_rad = c ? c->amsync_freq : 0.0f;
    float sync_dc = c ? c->amsync_dc : 0.0f;
    int lock_count = c ? c->amsync_lock_count : 0;

    float zeta = 0.707f;
    float omega_n = (lock_count > 10) ? 0.02f : 0.15f;
    float Kp = 2.0f * zeta * omega_n;
    float Ki = omega_n * omega_n;

    double sum_re = 0, sum_im = 0, sum_mag = 0;
    float max_abs = 0;

    for (int i = 0; i < half; i++) {
        float re = out_cx[cx_offset + i][0];
        float im = out_cx[cx_offset + i][1];

        float cs = cosf(phase);
        float sn = sinf(phase);
        float mix_i = re * cs + im * sn;
        float mix_q = im * cs - re * sn;

        float mag = sqrtf(re*re + im*im);
        sum_re += mix_i;
        sum_im += mix_q;
        sum_mag += mag;

        float sync_diff = mix_i - sync_dc;
        sync_dc += sync_diff * 0.05f;
        float sample = mix_i - sync_dc;
        audio_out[i] = sample;
        if (fabsf(sample) > max_abs) max_abs = fabsf(sample);

        float error = atan2f(mix_q, mix_i);

        freq_rad += Ki * error;
        phase += freq_rad + Kp * error;
        phase = wrap_pi(phase);
    }

    double metric = (sum_mag > 1e-9) ? (hypot(sum_re, sum_im) / sum_mag) : 0.0;

    if (c) c->amsync_coherence_ema += 0.1 * (metric - c->amsync_coherence_ema);
    float coherence = c ? c->amsync_coherence_ema : (float)metric;

    if (coherence > 0.65) {
        if (lock_count < 50) lock_count++;
    } else {
        if (lock_count > 0) lock_count--;
    }

    if (lock_count == 0 && peak_power > 1e-6) {
        float fft_peak_rad = (2.0f * M_PI * peak_k) / (2.0f * half);
        freq_rad += 0.1f * (fft_peak_rad - freq_rad);
    }

    if (c) {
        c->amsync_phase = phase;
        c->amsync_freq = freq_rad;
        c->amsync_dc = sync_dc;
        c->amsync_lock_count = lock_count;
        c->amsync_state = (lock_count > 20) ? AMSYNC_LOCKED : (lock_count > 0 ? AMSYNC_LOCKING : AMSYNC_UNLOCKED);

        c->amsync_truefreq_mhz = llround(((c->tune_khz * 1000.0) + (freq_rad * hz_per_rad)) * 1000.0);
    }

    dt->power = sum_mag * sum_mag;
    dt->sqrt_power = max_abs * 2.0f;
}

static void demod_am_narrow(struct BandState *b, int tune_bin,
                            float *filter_buf, void *cs,
                            struct DemodTable *dt, int phase_flag)
{
    demod_am_core(b, tune_bin, filter_buf, (struct Client *)cs, dt, phase_flag, 0);
}

static void demod_am_wide(struct BandState *b, int tune_bin,
                          float *filter_buf, void *cs,
                          struct DemodTable *dt, int phase_flag)
{
    demod_am_core(b, tune_bin, filter_buf, (struct Client *)cs, dt, phase_flag, 1);
}

static void demod_amsync_narrow(struct BandState *b, int tune_bin,
                                float *filter_buf, void *cs,
                                struct DemodTable *dt, int phase_flag)
{
    demod_amsync_core(b, tune_bin, filter_buf, (struct Client *)cs, dt, phase_flag, 0);
}

static void demod_amsync_wide(struct BandState *b, int tune_bin,
                              float *filter_buf, void *cs,
                              struct DemodTable *dt, int phase_flag)
{
    demod_amsync_core(b, tune_bin, filter_buf, (struct Client *)cs, dt, phase_flag, 1);
}

void dsp_demod_amsync(struct BandState *b, int tune_bin,
                      float *filter_buf, struct Client *c,
                      struct DemodTable *dt, int phase_flag)
{
    if (!dt) return;
    if (dt->half_size <= 128)
        demod_amsync_narrow(b, tune_bin, filter_buf, c, dt, phase_flag);
    else
        demod_amsync_wide(b, tune_bin, filter_buf, c, dt, phase_flag);
}

static void demod_fm(struct BandState *b, int tune_bin,
                     float *filter_buf, void *cs,
                     struct DemodTable *dt, int phase_flag)
{
    struct Client *c = (struct Client *)cs;
    int N = b->fftlen;
    int fm_half = dt->half_size;
    int fm_out  = dt->audiolen;
    float *spec = (float *)b->fft_out_r2c;
    fftwf_complex *din  = dt->in_buf;
    fftwf_complex *dout = dt->out_cx_buf;
    float *out = dt->output;

    if (!din || !dout || !dt->plan || !out) {
        if (out) memset(out, 0, sizeof(float) * fm_out);
        return;
    }

    int first_bin = 1 - fm_half;
    int mask = 2 * fm_half - 1;
    if (fm_half > first_bin) {
        int src_bin = first_bin + tune_bin;
        float *filt = filter_buf + 512 + first_bin;

        for (int k = first_bin; k < fm_half; k++) {
            if (src_bin >= 0 && src_bin < N) {
                float *sp = (float *)spec + 2 * src_bin;
                float fw = *filt;
                int dst = k & mask;
                din[dst][0] = sp[0] * fw;
                din[dst][1] = sp[1] * fw;
            }
            ++src_bin;
            ++filt;
        }
    }

    fft_backend_execute_demod_ifft(dt);

    int start_bin = (phase_flag) ? 0 : fm_half;

    float p2_re, p2_im, p_re, p_im;
    if (c) {
        p2_re = c->demod_fm_prev2_re;
        p2_im = c->fm_prev2_im;
        p_re  = c->demod_prev_re;
        p_im  = c->demod_prev_im;
    } else {
        p2_re = p2_im = p_re = p_im = 0.0f;
    }

    float divisor = (float)(fm_half / fm_out);
    double total_power = 0.0;
    float noise_var = 0.0f;

    float lpf = 0.0f;

    for (int j = 0; j < fm_half; j++) {
        float re = dout[start_bin + j][0];
        float im = dout[start_bin + j][1];

        float mag_sq = re * re + im * im;
        total_power += (double)mag_sq;

        float mag = sqrtf(mag_sq + 1e-30f);
        float i_n = re / mag;
        float q_n = im / mag;

        float d = ((p2_re - i_n) * p_im - (p2_im - q_n) * p_re) / divisor;

        if (d > 0.99f) d = 0.99f;
        if (d < -0.99f) d = -0.99f;

        int decim = fm_half / fm_out;
        if (decim < 1) decim = 1;
        out[j / decim] = d;

        float diff = d - lpf;
        lpf = lpf + diff * 0.5f;
        noise_var += (d - lpf) * (d - lpf);

        p2_re = p_re; p2_im = p_im;
        p_re = i_n; p_im = q_n;
    }

    if (c) {
        c->demod_fm_prev2_re = p2_re;
        c->fm_prev2_im       = p2_im;
        c->demod_prev_re      = p_re;
        c->demod_prev_im      = p_im;
    }

    dt->power = 0.5 * total_power;
    dt->sqrt_power = 2.0;

    if (c) {
        int filter_span = c->filter_hi_bin - c->filter_lo_bin;
        double squelch_floor;
        if (c->band_ptr) {
            squelch_floor = -0.0000367 * ((double)filter_span * 0.03125) * ((double)filter_span * 0.03125)
                + (double)filter_span * 0.03125 * 0.001835
                - 0.0064
                - 0.0015;
        } else {
            squelch_floor = 0.0125;
        }
        float var_per_sample = noise_var / (float)fm_half;
        c->demod_fm_sq = (var_per_sample > squelch_floor) ? 1 : 0;
    }
}

static void demod_subband(struct BandState *b, int tune_bin,
                          float *filter_buf, void *cs,
                          struct DemodTable *dt, int phase_flag)
{
    int half = dt->half_size;
    int N = b->fftlen;
    fftwf_complex *spec = (fftwf_complex *)b->fft_out_r2c;
    fftwf_complex *din  = dt->in_buf;
    int W = 2 * half;

    int first_bin = 1 - half;
    if (half > first_bin) {
        for (int k = first_bin; k < half; k++) {
            int src_bin = k + tune_bin;
            int dst = k & (W - 1);
            if (src_bin >= 0 && src_bin < N) {
                float fw = filter_buf ? filter_buf[512 + k] : 1.0f;
                din[dst][0] = spec[src_bin][0] * fw;
                din[dst][1] = spec[src_bin][1] * fw;
            } else {
                din[dst][0] = 0.0f;
                din[dst][1] = 0.0f;
            }
        }
    }

    fft_backend_execute_demod_ifft(dt);

    int offset = (phase_flag) ? 0 : half;
    dt->output_cx = dt->out_cx_buf + offset;

    (void)cs;
}

void dsp_apply_mode_selection(struct Client *c, struct BandState *b, int js_mode)
{
    if (!c) return;

    if (!b && c->band_ptr)
        b = (struct BandState *)c->band_ptr;

    struct DemodTable *bdt = b ? b->band_demod_tables : NULL;
    if (!bdt) bdt = g_demod_tables;

    int demod_mode;
    if (js_mode == DEMOD_AM) demod_mode = DEMOD_AM;
    else if (js_mode == DEMOD_AMSYNC) demod_mode = DEMOD_AMSYNC;
    else if (js_mode == DEMOD_FM) demod_mode = DEMOD_FM;
    else demod_mode = DEMOD_LSB;

    int table_in_band = 0;
    if (c->demod_table) {
        if (c->demod_table == &bdt[DT_AM_NARROW] ||
            c->demod_table == &bdt[DT_AM_WIDE]   ||
            c->demod_table == &bdt[DT_SSB_NARROW]||
            c->demod_table == &bdt[DT_SSB_WIDE]  ||
            c->demod_table == &bdt[DT_FM]) {
            table_in_band = 1;
        }
    }
    if (c->demod_mode == demod_mode && table_in_band)
        return;

    struct DemodTable *cur = c->demod_table;
    int old_half_size = cur ? cur->half_size : 0;

    c->demod_mode = demod_mode;
    c->demod_filter_dirty = -1;

    switch (demod_mode) {
        case DEMOD_AM:
        case DEMOD_AMSYNC:
            c->demod_table = (c->carrier_freq <= 9000.0)
                ? &bdt[DT_AM_NARROW]
                : &bdt[DT_AM_WIDE];
            break;
        case DEMOD_FM:
            c->demod_table = &bdt[DT_FM];
            break;
        default:
            c->demod_table = (c->carrier_freq <= 9000.0)
                ? &bdt[DT_SSB_NARROW]
                : &bdt[DT_SSB_WIDE];
            break;
    }

    if (demod_mode == DEMOD_AMSYNC)
        init_amsync_state(c);
    else if (c->demod_table && c->demod_table->init_fn)
        c->demod_table->init_fn(c);

    int new_half_size = c->demod_table ? c->demod_table->half_size : old_half_size;
    if (b && b->fftlen_real > 0 && new_half_size != old_half_size) {
        c->carrier_freq = (double)new_half_size
                        * (2.0 * (double)b->samplerate / (double)b->fftlen_real);
        c->mode_changed = 1;
    }
}

void compute_passband_filter(struct Client *c, struct BandState *b)
{
    if (!c || !b) return;

    double lo_khz = c->lo_khz;
    double hi_khz = c->hi_khz;

    struct DemodTable *bdt = b->band_demod_tables;
    if (!bdt) bdt = g_demod_tables;

    double max_abs_edge_khz = fmax(fabs(hi_khz), fabs(lo_khz));

    struct DemodTable *cur = c->demod_table;
    if (!cur) {
        cur = &bdt[DT_SSB_NARROW];
        c->demod_table = cur;
    }

    int old_half_size = cur->half_size;
    if (cur == &bdt[DT_AM_WIDE]) {
        if (max_abs_edge_khz < 3.5) {
            c->demod_table = &bdt[DT_AM_NARROW];
            c->demod_filter_dirty = -1;
        }
    } else if (cur == &bdt[DT_AM_NARROW]) {
        if (max_abs_edge_khz > 3.5 && g_config.allowwide) {
            c->demod_table = &bdt[DT_AM_WIDE];
            c->demod_filter_dirty = -1;
        }
    } else if (cur == &bdt[DT_SSB_WIDE]) {
        if (max_abs_edge_khz < 3.5) {
            c->demod_table = &bdt[DT_SSB_NARROW];
            c->demod_filter_dirty = -1;
        }
    } else if (cur == &bdt[DT_SSB_NARROW]) {
        if (max_abs_edge_khz > 3.5 && g_config.allowwide) {
            c->demod_table = &bdt[DT_SSB_WIDE];
            c->demod_filter_dirty = -1;
        }
    }

    struct DemodTable *dt = c->demod_table;
    if (!dt) return;

    if (dt->half_size != old_half_size && b->fftlen_real > 0) {
        c->carrier_freq = (double)dt->half_size
                        * (2.0 * (double)b->samplerate / (double)b->fftlen_real);
        c->filter_lo_bin = 0x7FFFFFFF;
        c->filter_hi_bin = 0x7FFFFFFF;
        c->mode_changed = 1;
    }

    double fftlen_d = (double)b->fftlen;
    double sr_d     = (double)b->samplerate;

    double lo_bin_f = fftlen_d * lo_khz / sr_d * 1000.0;
    int lo_bin = (lo_bin_f <= 0.0) ? (int)(lo_bin_f - 0.5) : (int)(lo_bin_f + 0.5);

    double hi_bin_f = fftlen_d * hi_khz / sr_d * 1000.0;
    int hi_bin = (hi_bin_f <= 0.0) ? (int)(hi_bin_f - 0.5) : (int)(hi_bin_f + 0.5);

    if (lo_bin == c->filter_lo_bin && hi_bin == c->filter_hi_bin)
        return;

    int hi_table_base = 5122 - lo_bin;
    int lo_table_limit = 1024 - hi_bin;

    if (lo_bin > hi_bin || hi_table_base > 6143 || lo_table_limit < 0) {
        for (int i = 0; i < 1024; i++)
            c->filter_buf[i] = 1.0f;
        return;
    }

    int filter_half = dt->half_size;
    if (hi_bin >= filter_half - 1)
        hi_bin = filter_half - 2;
    int lo_edge = 2 - filter_half;
    if (lo_bin >= lo_edge)
        lo_edge = lo_bin;

    int hi_idx_base = 4098 - lo_edge;
    for (int i = 0; i < 1024; i++) {
        int hi_idx = hi_idx_base + i;
        int lo_idx = lo_edge - 3076 - hi_bin + hi_idx_base + i;
        float fh = (hi_idx >= 0 && hi_idx < 6144) ? filter_table[hi_idx] : 0.0f;
        float fl = (lo_idx >= 0 && lo_idx < 6144) ? filter_table[lo_idx] : 0.0f;
        c->filter_buf[i] = fh * fl;
    }

    c->filter_lo_bin = lo_edge;
    c->filter_hi_bin = hi_bin;

    int conv_mode = 2;
    if (max_abs_edge_khz >= 1.0) conv_mode = (max_abs_edge_khz < 2.7) ? 1 : 0;
    if (c->carrier_freq > 9000.0) conv_mode = 3;
    if (hi_bin * lo_edge > 0 && c->demod_mode != DEMOD_AM && c->demod_mode != DEMOD_AMSYNC)
        conv_mode |= 0x10;
    if (c->conv_type != conv_mode) {
        c->conv_type = conv_mode;
        c->conv_changed = 1;
    }

    double effective_bw_bins = (double)(hi_bin - lo_edge);
    if (hi_bin * lo_edge < 0) {
        int mirrored_lo = -lo_edge;
        effective_bw_bins = (hi_bin <= mirrored_lo) ? (double)mirrored_lo : (double)hi_bin;
    }
    c->squelch_threshold = (float)(
        0.00004332 * effective_bw_bins * effective_bw_bins
        + effective_bw_bins * 0.00404
        + 0.0504
        - 0.05
        - 0.09 * (0.00390625 * effective_bw_bins) * (0.00390625 * effective_bw_bins) *
                  (0.00390625 * effective_bw_bins) * (0.00390625 * effective_bw_bins)
    );
}

static void dsp_execute_spectrum_fft_with_phase(struct BandState *b, int phase_flag);

void dsp_execute_spectrum_fft(struct BandState *b)
{
    dsp_execute_spectrum_fft_with_phase(b, 0);
}

static void dsp_execute_spectrum_fft_with_phase(struct BandState *b, int phase_flag)
{

    if (b->audio_clients <= 0) return;

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);

    b->agc_state++;
    fft_backend_execute_plan_fwd(b);

    if (!b->noniq) {
        fftwf_complex *cx = b->fft_out_cx;
        fftwf_complex *r2c = b->fft_out_r2c;
        fftwf_complex *phase = b->fft_phase_corr;

        cx[0][0] = 0.0f;
        cx[0][1] = 0.0f;

        if (phase) {
            int N = b->fftlen;
            int half = N / 2;
            if (half > 1) {

                float *upper_out = (float *)(r2c + half + 1);
                float *lower_out = (float *)(r2c + half - 2);
                float *reverse_cx = (float *)(cx + N - 1);
                float *reverse_phase = (float *)(phase + N - 1);
                float *forward_cx = (float *)(cx + 1);
                float *forward_phase = (float *)(phase + 1);

                for (int k = 1; k < half; k++) {
                    float cx_im = reverse_cx[1];
                    float phase_im = reverse_phase[1];

                    upper_out[0] = forward_cx[0] - (reverse_cx[0] * reverse_phase[0] + cx_im * phase_im);

                    float cross_im = cx_im * reverse_phase[0];
                    upper_out[1] = forward_cx[1] - (phase_im * reverse_cx[0] - cross_im);

                    float forward_im = forward_cx[1];
                    float phase_forward_im = forward_phase[1];
                    lower_out[0] = reverse_cx[0] - (forward_cx[0] * forward_phase[0] + forward_im * phase_forward_im);

                    float cross_forward = forward_im * forward_phase[0];
                    lower_out[1] = reverse_cx[1] - (phase_forward_im * forward_cx[0] - cross_forward);

                    upper_out += 2;
                    lower_out -= 2;
                    reverse_cx -= 2;
                    reverse_phase -= 2;
                    forward_cx += 2;
                    forward_phase += 2;
                }
            }
        } else {

            int N = b->fftlen;
            int half = N / 2;
            size_t half_bytes = half * sizeof(fftwf_complex);
            memcpy(r2c, cx + half, half_bytes);
            memcpy(r2c + half, cx, half_bytes);
        }
    }

    gettimeofday(&t1, NULL);
    {
        long long dt = (t1.tv_usec + 1000000LL * (t1.tv_sec - t0.tv_sec)) - t0.tv_usec;
        fft_exec_count++;
        fft_exec_usec += dt;
        __sync_fetch_and_add(&g_timing_fft_us, dt);
    }

    dsp_dispatch_clients(b, phase_flag);
}

void dsp_process_fft_block(struct BandState *b)
{
    int current_half_fftlen = b->half_fftlen;
    int fftlen_real = b->fftlen_real;

    if (current_half_fftlen == fftlen_real) {
        b->sample_count = 0;
        b->half_fftlen = fftlen_real / 4;
        return;
    }

    if (current_half_fftlen == fftlen_real / 4)
        b->half_fftlen = 3 * fftlen_real / 4;
    else
        b->half_fftlen = fftlen_real;

    int phase_flag = (b->sample_count != fftlen_real / 4) ? 1 : 0;
    b->phase_flag = phase_flag;
    dsp_execute_spectrum_fft_with_phase(b, phase_flag);
}

void dsp_build_waterfall_pyramid(struct BandState *b)
{
    struct timeval wf_t0, wf_t1;
    gettimeofday(&wf_t0, NULL);

    int mz = b->maxzoom;
    if (mz < 1 || mz >= WF_MAX_ZOOMS) return;

    int top = mz - 1;
    int top_px = 1024 << top;
    int N  = b->fftlen;
    int N2 = b->fftlen2;

    if (!b->wf_zoom_row[top] || !b->wf_zoom_power[top]) return;

    fftwf_complex *fft_src = NULL;
    int fft_N = N2;
    int fft_dc_xor;

    if (!b->noniq && b->fft_filter_buf && b->fft_in2 && b->plan_fwd2 && b->window_buf) {

        int cap = 4 * N2;
        if (b->wf_ring_fill >= cap) {

            int start = b->wf_ring_pos - N2;
            if (start < 0) start += cap;
            fftwf_complex *ring = (fftwf_complex *)b->fft_filter_buf;
            int wn = b->Wffftlen;
            uint64_t wn_recip = N2 > 1 ? ((uint64_t)wn << 32) / (uint64_t)N2 : 0;
            for (int i = 0; i < N2; i++) {
                int src = start + i;
                if (src >= cap) src -= cap;
                int wi = (int)(((uint64_t)(unsigned)i * wn_recip) >> 32);
                if (wi >= wn) wi = wn - 1;
                float w = b->window_buf[wi];
                b->fft_in2[i][0] = ring[src][0] * w;
                b->fft_in2[i][1] = ring[src][1] * w;
            }
            fft_backend_execute_plan_fwd2(b);
            fft_src    = b->fft_out2;
            fft_N      = N2;
            fft_dc_xor = N2 / 2;
        }
    } else if (b->noniq && b->fft_in_r2c && b->fft_in2_r2c && b->plan_fwd2 && b->window_buf) {

        int n_r = 2 * N2;
        int fft_real = b->fftlen_real;
        int sc_r = b->sample_count;

        int off_r;
        if (sc_r >= n_r) {
            off_r = sc_r - n_r;
        } else {

            off_r = fft_real - n_r;
            if (off_r < 0) off_r = 0;
        }

        if (off_r + n_r > fft_real)
            off_r = fft_real - n_r;
        if (off_r < 0) off_r = 0;

        int wn  = b->Wffftlen;
        uint64_t wn_recip = n_r > 1 ? ((uint64_t)wn << 32) / (uint64_t)n_r : 0;
        for (int i = 0; i < n_r; i++) {
            int wi = (int)(((uint64_t)(unsigned)i * wn_recip) >> 32);
            if (wi >= wn) wi = wn - 1;
            b->fft_in2_r2c[i] = b->fft_in_r2c[off_r + i] * b->window_buf[wi];
        }
        fft_backend_execute_plan_fwd2(b);
        fft_src    = b->fft_out2;
        fft_N      = N2;
        fft_dc_xor = 0;
    } else {

        fft_src    = b->noniq ? b->fft_out_r2c : b->fft_out_cx;
        fft_N      = N;
        fft_dc_xor = b->noniq ? 0 : (N2 / 2);
    }

    if (!fft_src) {
        return;
    }

    float   *pwr_top = b->wf_zoom_power[top];

#ifdef USE_VULKAN
    if (b->vk_wf) {
        vk_waterfall_build_pyramid(b, (const float *)fft_src,
                                   fft_N, top_px, fft_dc_xor);
        goto pyramid_done;
    }
#endif

    if (fft_N == top_px) {
        for (int p = 0; p < top_px; p++) {
            int src = p ^ fft_dc_xor;
            if (src < 0 || src >= fft_N) { pwr_top[p] = 0.0f; continue; }
            pwr_top[p] = fft_src[src][0] * fft_src[src][0]
                       + fft_src[src][1] * fft_src[src][1];
        }
    } else {
        for (int p = 0; p < top_px; p++) {
            int sec_bin = p ^ fft_dc_xor;
            int mb_start = (int)((long long)sec_bin * fft_N / top_px);
            int mb_end   = (int)((long long)(sec_bin + 1) * fft_N / top_px);
            if (mb_end <= mb_start) mb_end = mb_start + 1;
            if (mb_end > fft_N) mb_end = fft_N;
            float sum = 0.0f;
            int   cnt = 0;
            for (int k = mb_start; k < mb_end; k++) {
                sum += fft_src[k][0] * fft_src[k][0]
                     + fft_src[k][1] * fft_src[k][1];
                cnt++;
            }
            pwr_top[p] = cnt > 0 ? sum / (float)cnt : 0.0f;
        }
    }

    if (mz < WF_MAX_ZOOMS && b->wf_zoom_power[mz]) {
        float *pwr_mz = b->wf_zoom_power[mz];
        for (int p = 0; p < top_px; p++) {
            pwr_mz[2 * p]     = pwr_top[p];
            pwr_mz[2 * p + 1] = pwr_top[p];
        }
    }

    for (int z = top - 1; z >= 0; z--) {
        int px = 1024 << z;
        if (!b->wf_zoom_power[z]) break;
        float *pwr_above = b->wf_zoom_power[z + 1];
        float *pwr_this  = b->wf_zoom_power[z];
        for (int i = 0; i < px; i++)
            pwr_this[i] = (pwr_above[2 * i] + pwr_above[2 * i + 1]) * 0.5f;
    }

#ifdef USE_VULKAN
pyramid_done:
#endif

    b->wf_pyramid_valid = 1;
    b->wf_pyramid_seq++;

    gettimeofday(&wf_t1, NULL);
    __sync_fetch_and_add(&g_timing_wf_us,
        (wf_t1.tv_usec + 1000000LL * (wf_t1.tv_sec - wf_t0.tv_sec)) - wf_t0.tv_usec);
}

void dsp_dispatch_clients(struct BandState *b, int phase_flag)
{
    enum {
        WF_REF_SAMPLERATE = 2048000,
        WF_REF_FFTLEN     = 65536,
        WF_REF_BUILD_DIV  = 4
    };
    const double wf_rate_boost = 1.25;
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);

    int has_wf_clients = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct Client *c = &clients[i];
        if (c->state != CLIENT_WEBSOCKET) continue;
        if (c->band_idx < 0 || c->band_idx >= MAX_BANDS) continue;
        if (&bands[c->band_idx] != b) continue;

        if (c->audio_format > 0) { has_wf_clients = 1; break; }
    }

    int wf_build_div = WF_REF_BUILD_DIV;
    if (b->samplerate > 0 && b->fftlen > 0) {
        double cur_ratio = (double)b->samplerate / (double)b->fftlen;
        double ref_ratio = (double)WF_REF_SAMPLERATE / (double)WF_REF_FFTLEN;
        double scaled = (double)WF_REF_BUILD_DIV * (cur_ratio / ref_ratio);
        scaled /= wf_rate_boost;
        if (scaled < 1.0) scaled = 1.0;
        if (scaled > 1024.0) scaled = 1024.0;
        wf_build_div = (int)(scaled + 0.5);
        if (wf_build_div < 1) wf_build_div = 1;
    }

    if (has_wf_clients &&
        (b->dispatch_counter % (unsigned int)wf_build_div) == 0)
        dsp_build_waterfall_pyramid(b);

    extern void client_dispatch_audio(struct BandState *b, int phase_flag);
    client_dispatch_audio(b, phase_flag);

    gettimeofday(&t1, NULL);
    long long dt_us = (t1.tv_usec + 1000000LL*(t1.tv_sec - t0.tv_sec)) - t0.tv_usec;
    __sync_fetch_and_add(&g_timing_dispatch_us, dt_us);
    demod_exec_count++;
    demod_avg_usec = (long long)((double)demod_avg_usec * 0.99 + (double)dt_us * 0.01);
}

void dsp_noise_blanker(float *samples, int n, float threshold)
{
    if (threshold <= 0.0f) return;
    float sum = 0.0f;
    for (int i = 0; i < n * 2; i++)
        sum += samples[i] * samples[i];
    float rms = sqrtf(sum / (float)(n * 2));
    float limit = threshold * rms;
    for (int i = 0; i < n; i++) {
        float re = samples[2*i];
        float im = samples[2*i+1];
        float mag = sqrtf(re*re + im*im);
        if (mag > limit) {
            samples[2*i]   = 0.0f;
            samples[2*i+1] = 0.0f;
        }
    }
}

void dsp_iq_correct(float *samples, int n,
                    float *dc_i, float *dc_q, float alpha,
                    float iq_gain_corr)
{
    if (alpha <= 0.0f) return;
    for (int i = 0; i < n; i++) {
        float fi = samples[2*i];
        float fq = samples[2*i+1];
        *dc_i = *dc_i + (fi - *dc_i) * alpha;
        *dc_q = *dc_q + (fq - *dc_q) * alpha;
        fi -= *dc_i;
        fq -= *dc_q;
        if (iq_gain_corr != 0.0f) fq += fi * iq_gain_corr;
        samples[2*i]   = fi;
        samples[2*i+1] = fq;
    }
}
