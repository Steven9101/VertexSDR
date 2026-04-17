// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#ifndef WATERFALL_H
#define WATERFALL_H

#include <stdint.h>
#include <fftw3.h>

struct WaterfallFreqLabel {
    int x_pos;
    const char *label;
};

void waterfall_init_palette(void);

static inline uint8_t waterfall_power_to_idx(float power_sq, float gain_adj)
{
    if (power_sq <= 0.0f) return 0;
    union { float f; uint32_t u; } pu;
    pu.f = power_sq;
    int sv = (int)(int16_t)(pu.u >> 16);
    int idx = (sv + (int)(gain_adj * (128.0f / 3.0f)) - 17106) / 13;
    if (idx < 0)   return 0;
    if (idx > 255) return 255;
    return (uint8_t)idx;
}

void waterfall_generate_row(fftwf_complex *fft_out, int fftlen2,
                             uint8_t *row, float gain_adj, int noniq);

void waterfall_generate_row_scaled(fftwf_complex *fft_out, int fftlen,
                                    int out_width, uint8_t *row,
                                    float gain_adj, int noniq);

void waterfall_power_row_from_fft(fftwf_complex *fft_out, int fftlen,
                                   int out_width, float *power_out, int noniq);

void waterfall_generate_zoom_rows(float **power_bufs, uint8_t **zoom_rows,
                                   int fftlen2, int num_zooms, float gain_adj);

void waterfall_render_label(uint8_t *buf, int buf_w, int buf_h,
                             int x, int y, const char *text,
                             uint8_t r, uint8_t g, uint8_t b);

int waterfall_write_png(const char *path,
                         uint8_t **rows, int width, int height,
                         struct WaterfallFreqLabel *labels, int nlabels);

int waterfall_write_bandlabel_png(const char *path,
                                   double center_khz, double bw_khz,
                                   int width);

int waterfall_write_band_pngs(const char *outdir, int band_idx,
                               double center_khz, int samplerate, int maxzoom);

int waterfall_compress_row(const uint8_t *src, int len, uint8_t *dst, int format);

void waterfall_render_text_rows(const char *text, uint8_t out[32][1024]);

int waterfall_encode_row_f9(const uint8_t *src, uint8_t *prev_row,
                             int width, uint8_t *dst);

int waterfall_encode_row_f10(const uint8_t *src, uint8_t *prev_row,
                              uint8_t *prev_n, int width, uint8_t *dst);

#endif
