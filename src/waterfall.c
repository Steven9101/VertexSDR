// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#include "waterfall.h"
#include "band.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <png.h>

#include "wf_font_data.h"

typedef struct { uint8_t r, g, b; } RGB;

static RGB wf_palette[256];

void waterfall_init_palette(void)
{
    for (int i = 0; i < 256; i++) {
        float t = (float)i / 255.0f;
        float r, g, b;
        if (t < 0.125f) {
            r = 0.0f; g = 0.0f; b = 4.0f * t / 0.5f;
        } else if (t < 0.375f) {
            r = 0.0f; g = (t - 0.125f) / 0.25f; b = 1.0f;
        } else if (t < 0.625f) {
            r = (t - 0.375f) / 0.25f; g = 1.0f; b = 1.0f - (t - 0.375f) / 0.25f;
        } else if (t < 0.875f) {
            r = 1.0f; g = 1.0f - (t - 0.625f) / 0.25f; b = 0.0f;
        } else {
            r = 1.0f; g = (t - 0.875f) / 0.125f; b = (t - 0.875f) / 0.125f;
        }
        wf_palette[i].r = (uint8_t)(r * 255.0f);
        wf_palette[i].g = (uint8_t)(g * 255.0f);
        wf_palette[i].b = (uint8_t)(b * 255.0f);
    }

    wf_palette[0].r = 0; wf_palette[0].g = 0; wf_palette[0].b = 0;
}

void waterfall_generate_row(fftwf_complex *fft_out, int fftlen2,
                             uint8_t *row, float gain_adj, int noniq)
{
    int dc_xor = noniq ? 0 : (fftlen2 / 2);

    for (int i = 0; i < fftlen2; i++) {
        int src = i ^ dc_xor;
        float re = fft_out[src][0];
        float im = fft_out[src][1];
        float power = re * re + im * im;
        row[i] = waterfall_power_to_idx(power, gain_adj);
    }
}

void waterfall_generate_row_scaled(fftwf_complex *fft_out, int fftlen,
                                    int out_width, uint8_t *row,
                                    float gain_adj, int noniq)
{
    if (out_width <= 0 || fftlen <= 0 || !fft_out || !row) return;

    int dc_xor = noniq ? 0 : (fftlen / 2);

    for (int p = 0; p < out_width; p++) {
        int bin_start = (int)((long long)p       * fftlen / out_width);
        int bin_end   = (int)((long long)(p + 1) * fftlen / out_width);
        if (bin_end <= bin_start) bin_end = bin_start + 1;
        if (bin_end > fftlen)     bin_end = fftlen;

        uint8_t best = 0;
        for (int b2 = bin_start; b2 < bin_end; b2++) {
            int src = b2 ^ dc_xor;
            float re = fft_out[src][0];
            float im = fft_out[src][1];
            float pwr = re * re + im * im;
            uint8_t idx = waterfall_power_to_idx(pwr, gain_adj);
            if (idx > best) best = idx;
        }
        row[p] = best;
    }
}

void waterfall_power_row_from_fft(fftwf_complex *fft_out, int fftlen,
                                   int out_width, float *power_out, int noniq)
{
    if (out_width <= 0 || fftlen <= 0 || !fft_out || !power_out) return;

    int dc_xor = noniq ? 0 : (fftlen / 2);

    for (int p = 0; p < out_width; p++) {
        int bin_start = (int)((long long)p       * fftlen / out_width);
        int bin_end   = (int)((long long)(p + 1) * fftlen / out_width);
        if (bin_end <= bin_start) bin_end = bin_start + 1;
        if (bin_end > fftlen)     bin_end = fftlen;

        float sum = 0.0f;
        int   cnt = 0;
        for (int b2 = bin_start; b2 < bin_end; b2++) {
            int src = b2 ^ dc_xor;
            float re = fft_out[src][0];
            float im = fft_out[src][1];
            sum += re * re + im * im;
            cnt++;
        }
        power_out[p] = (cnt > 0) ? (sum / (float)cnt) : 0.0f;
    }
}

void waterfall_generate_zoom_rows(float **power_bufs, uint8_t **zoom_rows,
                                   int fftlen2, int num_zooms, float gain_adj)
{
    int width = fftlen2;
    for (int z = 1; z < num_zooms; z++) {
        width /= 2;
        for (int i = 0; i < width; i++) {
            float avg = (power_bufs[z-1][2*i] + power_bufs[z-1][2*i+1]) * 0.5f;
            power_bufs[z][i] = avg;
            zoom_rows[z][i]  = waterfall_power_to_idx(avg, gain_adj);
        }
    }
}

#define FONT_W      6
#define FONT_H      8
#define FONT_CHARS  128

static const uint8_t font_bitmaps[FONT_CHARS][FONT_H] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x20, 0x00},
    {0x50, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x50, 0xF8, 0x50, 0xF8, 0x50, 0x00},
    {0x20, 0x78, 0xA0, 0x70, 0x28, 0xF0, 0x20, 0x00},
    {0x00, 0x00, 0xC8, 0x90, 0x20, 0x48, 0x98, 0x00},
    {0x00, 0x40, 0xA0, 0x40, 0xA8, 0x90, 0x68, 0x00},
    {0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x20, 0x40, 0x40, 0x40, 0x40, 0x40, 0x20, 0x00},
    {0x20, 0x10, 0x10, 0x10, 0x10, 0x10, 0x20, 0x00},
    {0x00, 0x00, 0x50, 0x20, 0xF8, 0x20, 0x50, 0x00},
    {0x00, 0x00, 0x20, 0x20, 0xF8, 0x20, 0x20, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x20, 0x40},
    {0x00, 0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00},
    {0x08, 0x08, 0x10, 0x20, 0x40, 0x80, 0x80, 0x00},
    {0x20, 0x50, 0x88, 0x88, 0x88, 0x50, 0x20, 0x00},
    {0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00},
    {0x70, 0x88, 0x08, 0x10, 0x20, 0x40, 0xF8, 0x00},
    {0xF8, 0x08, 0x10, 0x30, 0x08, 0x88, 0x70, 0x00},
    {0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10, 0x00},
    {0xF8, 0x80, 0xF0, 0x08, 0x08, 0x88, 0x70, 0x00},
    {0x30, 0x40, 0x80, 0xF0, 0x88, 0x88, 0x70, 0x00},
    {0xF8, 0x08, 0x10, 0x10, 0x20, 0x20, 0x20, 0x00},
    {0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70, 0x00},
    {0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0x60, 0x00},
    {0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x00},
    {0x00, 0x00, 0x20, 0x00, 0x00, 0x20, 0x20, 0x40},
    {0x10, 0x20, 0x40, 0x80, 0x40, 0x20, 0x10, 0x00},
    {0x00, 0x00, 0xF8, 0x00, 0x00, 0x00, 0xF8, 0x00},
    {0x40, 0x20, 0x10, 0x08, 0x10, 0x20, 0x40, 0x00},
    {0x70, 0x88, 0x10, 0x20, 0x20, 0x00, 0x20, 0x00},
    {0x70, 0x88, 0xB8, 0xA8, 0xB8, 0x80, 0x78, 0x00},
    {0x70, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88, 0x00},
    {0xF0, 0x88, 0x88, 0xF0, 0x88, 0x88, 0xF0, 0x00},
    {0x70, 0x88, 0x80, 0x80, 0x80, 0x88, 0x70, 0x00},
    {0xF0, 0x88, 0x88, 0x88, 0x88, 0x88, 0xF0, 0x00},
    {0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0xF8, 0x00},
    {0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0x80, 0x00},
    {0x70, 0x88, 0x80, 0xB8, 0x88, 0x88, 0x70, 0x00},
    {0x88, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88, 0x00},
    {0x70, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00},
    {0x08, 0x08, 0x08, 0x08, 0x08, 0x88, 0x70, 0x00},
    {0x88, 0x90, 0xA0, 0xC0, 0xA0, 0x90, 0x88, 0x00},
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xF8, 0x00},
    {0x88, 0xD8, 0xD8, 0xA8, 0xA8, 0x88, 0x88, 0x00},
    {0x88, 0xC8, 0xC8, 0xA8, 0x98, 0x98, 0x88, 0x00},
    {0x70, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00},
    {0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80, 0x80, 0x00},
    {0x70, 0x88, 0x88, 0x88, 0xA8, 0x98, 0x78, 0x00},
    {0xF0, 0x88, 0x88, 0xF0, 0xA0, 0x90, 0x88, 0x00},
    {0x70, 0x88, 0x80, 0x70, 0x08, 0x88, 0x70, 0x00},
    {0xF8, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00},
    {0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00},
    {0x88, 0x88, 0x88, 0x50, 0x50, 0x50, 0x20, 0x00},
    {0x88, 0x88, 0x88, 0xA8, 0xA8, 0xA8, 0x70, 0x00},
    {0x88, 0x50, 0x50, 0x20, 0x50, 0x50, 0x88, 0x00},
    {0x88, 0x88, 0x50, 0x50, 0x20, 0x20, 0x20, 0x00},
    {0xF8, 0x08, 0x10, 0x20, 0x40, 0x80, 0xF8, 0x00},
    {0x70, 0x40, 0x40, 0x40, 0x40, 0x40, 0x70, 0x00},
    {0x80, 0x80, 0x40, 0x20, 0x10, 0x08, 0x08, 0x00},
    {0x70, 0x10, 0x10, 0x10, 0x10, 0x10, 0x70, 0x00},
    {0x20, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8},
    {0x40, 0x40, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x70, 0x08, 0x78, 0x88, 0x78, 0x00},
    {0x80, 0x80, 0xF0, 0x88, 0x88, 0x88, 0xF0, 0x00},
    {0x00, 0x00, 0x78, 0x80, 0x80, 0x80, 0x78, 0x00},
    {0x08, 0x08, 0x78, 0x88, 0x88, 0x88, 0x78, 0x00},
    {0x00, 0x00, 0x70, 0x88, 0xF8, 0x80, 0x78, 0x00},
    {0x30, 0x48, 0x40, 0xE0, 0x40, 0x40, 0x40, 0x00},
    {0x00, 0x00, 0x78, 0x88, 0x88, 0x78, 0x08, 0xF0},
    {0x80, 0x80, 0x80, 0xF0, 0x88, 0x88, 0x88, 0x00},
    {0x20, 0x00, 0x60, 0x20, 0x20, 0x20, 0x70, 0x00},
    {0x10, 0x00, 0x30, 0x10, 0x10, 0x10, 0x90, 0x60},
    {0x80, 0x80, 0x88, 0x90, 0xE0, 0x90, 0x88, 0x00},
    {0x60, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00},
    {0x00, 0x00, 0xD0, 0xA8, 0xA8, 0xA8, 0x88, 0x00},
    {0x00, 0x00, 0xF0, 0x88, 0x88, 0x88, 0x88, 0x00},
    {0x00, 0x00, 0x70, 0x88, 0x88, 0x88, 0x70, 0x00},
    {0x00, 0x00, 0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80},
    {0x00, 0x00, 0x78, 0x88, 0x88, 0x78, 0x08, 0x08},
    {0x00, 0x00, 0xB8, 0xC0, 0x80, 0x80, 0x80, 0x00},
    {0x00, 0x00, 0x78, 0x80, 0x70, 0x08, 0xF0, 0x00},
    {0x40, 0x40, 0xE0, 0x40, 0x40, 0x48, 0x30, 0x00},
    {0x00, 0x00, 0x88, 0x88, 0x88, 0x88, 0x78, 0x00},
    {0x00, 0x00, 0x88, 0x88, 0x50, 0x50, 0x20, 0x00},
    {0x00, 0x00, 0x88, 0xA8, 0xA8, 0xA8, 0x70, 0x00},
    {0x00, 0x00, 0x88, 0x50, 0x20, 0x50, 0x88, 0x00},
    {0x00, 0x00, 0x88, 0x88, 0x88, 0x78, 0x08, 0xF0},
    {0x00, 0x00, 0xF8, 0x10, 0x20, 0x40, 0xF8, 0x00},
    {0x30, 0x40, 0x40, 0x80, 0x40, 0x40, 0x30, 0x00},
    {0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00},
    {0x60, 0x10, 0x10, 0x08, 0x10, 0x10, 0x60, 0x00},
    {0x68, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

static void font_render_char(uint8_t *buf, int buf_w, int buf_h,
                              int x, int y, unsigned char c,
                              uint8_t r, uint8_t g, uint8_t b2)
{
    if (c >= FONT_CHARS) return;
    for (int row = 0; row < FONT_H; row++) {
        int py = y + row;
        if (py < 0 || py >= buf_h) continue;
        uint8_t bits = font_bitmaps[(int)c][row];
        for (int col = 0; col < FONT_W; col++) {
            int px = x + col;
            if (px < 0 || px >= buf_w) continue;
            if (bits & (0x80 >> col)) {
                int off = (py * buf_w + px) * 3;
                buf[off+0] = r;
                buf[off+1] = g;
                buf[off+2] = b2;
            }
        }
    }
}

void waterfall_render_label(uint8_t *buf, int buf_w, int buf_h,
                             int x, int y, const char *text,
                             uint8_t r, uint8_t g, uint8_t b2)
{
    int cx = x;
    for (const unsigned char *cp = (const unsigned char *)text; *cp; cp++) {
        font_render_char(buf, buf_w, buf_h, cx, y, *cp, r, g, b2);
        cx += FONT_W + 1;
    }
}

int waterfall_write_png(const char *path,
                         uint8_t **rows, int width, int height,
                         struct WaterfallFreqLabel *labels __attribute__((unused)),
                         int nlabels __attribute__((unused)))
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    png_structp png = png_create_write_struct("1.2.49", NULL, NULL, NULL);
    if (!png) { fclose(fp); return -1; }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(fp); return -1; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return -1;
    }

    png_init_io(png, fp);

    png_set_IHDR(png, info, (png_uint_32)width, (png_uint_32)height,
                 8, PNG_COLOR_TYPE_PALETTE, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    png_color pal[256];
    for (int i = 0; i < 256; i++) {
        pal[i].red   = wf_palette[i].r;
        pal[i].green = wf_palette[i].g;
        pal[i].blue  = wf_palette[i].b;
    }
    png_set_PLTE(png, info, pal, 256);
    png_write_info(png, info);

    for (int y = 0; y < height; y++)
        png_write_row(png, rows[y]);

    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 0;
}

#define BANDLABEL_H 44
#define AXIS_LABEL_Y 6
#define BANDPLAN_LINE_Y 32
#define BANDPLAN_TEXT_Y 34
#define BANDLABEL_MARGIN_X 8

static void bl_draw_char(uint8_t *buf, int width, int x, int row_base,
                          unsigned char c, uint8_t color)
{
    if (c >= FONT_CHARS) return;
    for (int row = 0; row < FONT_H; row++) {
        int py = row_base + row;
        if (py < 0 || py >= BANDLABEL_H) continue;
        uint8_t bits = font_bitmaps[(int)c][row];
        for (int col = 0; col < FONT_W; col++) {
            int px = x + col;
            if (px < 0 || px >= width) continue;
            if (bits & (0x80 >> col))
                buf[py * width + px] = color;
        }
    }
}

static void bl_draw_str(uint8_t *buf, int width, int x, int row_base,
                         const char *s, uint8_t color)
{
    int cx = x;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        bl_draw_char(buf, width, cx, row_base, *p, color);
        cx += FONT_W + 1;
    }
}

static void bl_draw_char_compact(uint8_t *buf, int width, int x, int row_base,
                                 unsigned char c, uint8_t color)
{
    if (c >= FONT_CHARS) return;
    for (int row = 0; row < 6; row++) {
        int py = row_base + row;
        int src_row = row + 1;
        if (py < 0 || py >= BANDLABEL_H) continue;
        uint8_t bits = font_bitmaps[(int)c][src_row];
        for (int col = 0; col < FONT_W; col++) {
            int px = x + col;
            if (px < 0 || px >= width) continue;
            if (bits & (0x80 >> col))
                buf[py * width + px] = color;
        }
    }
}

static void bl_draw_str_compact(uint8_t *buf, int width, int x, int row_base,
                                const char *s, uint8_t color)
{
    int cx = x;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        bl_draw_char_compact(buf, width, cx, row_base, *p, color);
        cx += FONT_W + 1;
    }
}

struct BandplanEntry {
    double min_khz;
    double max_khz;
    const char *short_label;
    const char *long_label;
    uint8_t color;
};

static const struct BandplanEntry bandplan_entries[] = {
    {  148.5,   283.5,  "LW",            "longwave",                 2 },
    {  526.5,  1606.5,  "MW",            "mediumwave",               2 },
    { 2300.0,  2495.0,  "120m",          "120m broadcast",           2 },
    { 3200.0,  3400.0,  "90m",           "90m broadcast",            2 },
    { 3900.0,  4000.0,  "75m",           "75m broadcast",            2 },
    { 4750.0,  5060.0,  "60m",           "60m broadcast",            2 },
    { 5900.0,  6200.0,  "49m",           "49m broadcast",            2 },
    { 7200.0,  7450.0,  "41m",           "41m broadcast",            2 },
    { 9400.0,  9900.0,  "31m",           "31m broadcast",            2 },
    {11600.0, 12100.0,  "25m",           "25m broadcast",            2 },
    {13570.0, 13870.0,  "22m",           "22m broadcast",            2 },
    {15100.0, 15800.0,  "19m",           "19m broadcast",            2 },
    {17480.0, 17900.0,  "16m",           "16m broadcast",            2 },
    {18900.0, 19020.0,  "15m",           "15m broadcast",            2 },
    {21450.0, 21850.0,  "13m",           "13m broadcast",            2 },
    {25600.0, 26100.0,  "11m",           "11m broadcast",            2 },
    {  135.7,   137.8,  "2200m",         "2200m licensed amateurs",  3 },
    {  472.0,   479.0,  "630m",          "630m licensed amateurs",   3 },
    { 1810.0,  1880.0,  "160m",          "160m licensed amateurs",   3 },
    { 3500.0,  3800.0,  "80m",           "80m licensed amateurs",    3 },
    { 5351.5,  5366.5,  "60m",           "60m licensed amateurs",    3 },
    { 7000.0,  7200.0,  "40m",           "40m licensed amateurs",    3 },
    {10100.0, 10150.0,  "30m",           "30m licensed amateurs",    3 },
    {14000.0, 14350.0,  "20m",           "20m licensed amateurs",    3 },
    {18068.0, 18168.0,  "17m",           "17m licensed amateurs",    3 },
    {21000.0, 21450.0,  "15m",           "15m licensed amateurs",    3 },
    {24890.0, 24990.0,  "12m",           "12m licensed amateurs",    3 },
    {26960.0, 27410.0,  "CB",            "Citizens Band",            4 },
    {28000.0, 29700.0,  "10m",           "10m licensed amateurs",    3 },
};

static void bandlabel_overlay_bandplan(uint8_t *buf, int width,
                                       double f_lo, double f_hi)
{
    const double span = f_hi - f_lo;
    if (span <= 0.0) return;

    const double tile_span = span * 1024.0 / (double)width;

    for (size_t i = 0; i < sizeof(bandplan_entries) / sizeof(bandplan_entries[0]); i++) {
        const struct BandplanEntry *bp = &bandplan_entries[i];
        double lo = bp->min_khz > f_lo ? bp->min_khz : f_lo;
        double hi = bp->max_khz < f_hi ? bp->max_khz : f_hi;
        if (hi <= lo) continue;

        int x0 = (int)(((lo - f_lo) / span) * (double)width + 0.5);
        int x1 = (int)(((hi - f_lo) / span) * (double)width + 0.5);
        if (x1 <= x0) x1 = x0 + 1;
        if (x0 < 0) x0 = 0;
        if (x1 > width) x1 = width;

        for (int x = x0; x < x1; x++)
            buf[BANDPLAN_LINE_Y * width + x] = bp->color;

        int bar_w = x1 - x0;
        int clipped = (bp->min_khz < f_lo || bp->max_khz > f_hi);
        int long_w = (int)strlen(bp->long_label) * (FONT_W + 1) - 1;
        int short_w = (int)strlen(bp->short_label) * (FONT_W + 1) - 1;
        const char *label = NULL;
        int text_w = 0;
        int tx = 0;
        if (tile_span > 3000.0) {
            label = bp->short_label;
            text_w = short_w;
            int cx = (x0 + x1) / 2;
            tx = cx - text_w / 2;
        } else {
            if (bar_w >= long_w + 8 && !clipped) label = bp->long_label;
            else if (bar_w >= short_w + 6) label = bp->short_label;
            if (label) {
                text_w = (int)strlen(label) * (FONT_W + 1) - 1;
                tx = x0 + (bar_w - text_w) / 2;
            }
        }
        if (!label) continue;

        if (clipped && tile_span <= 600.0)
            continue;

        if (tx < BANDLABEL_MARGIN_X) tx = BANDLABEL_MARGIN_X;
        if (tx + text_w >= width - BANDLABEL_MARGIN_X)
            tx = width - BANDLABEL_MARGIN_X - text_w - 1;
        if (tx < BANDLABEL_MARGIN_X)
            continue;

        bl_draw_str(buf, width, tx, BANDPLAN_TEXT_Y, label, bp->color);
    }
}

static void bandlabel_fill(uint8_t *buf, int width,
                            int samplerate, double center_khz)
{

    static const double v54[] = {
        100000.0,
        10000.0,
        5000.0,
        1000.0,
        500.0,
        100.0,
        50.0,
        10.0,
        5.0,
        1.0,
        0.1,
        0.01,
        0.001,
        0.0,
    };

    static const double v55[] = {
        1000000.0,
        50000.0,
        5000.0,
        5000.0,
        500.0,
        500.0,
        50.0,
        50.0,
        5.0,
        5.0,
        0.5,
        0.05,
        0.005,
        0.0005,
        0.0,
    };

    static const double v56[] = {
        100000.0,
        10000.0,
        1000.0,
        1000.0,
        100.0,
        100.0,
        10.0,
        10.0,
        1.0,
        1.0,
        0.1,
        0.01,
        0.001,
        0.0001,
        0.0,
    };

    static const int v52[] = { 0,0,0,0,0,0,0,0,0,0,0,1,2,3,4 };

    double v5  = (double)samplerate / 2000.0;
    double v42 = (double)samplerate / 1000.0;
    double v47 = center_khz - v5;
    double v48 = center_khz + v5;

    int v6 = (int)(log10(v48 > 0.0 ? v48 : 1.0) + 1.0);
    if (v47 < 0.0) v6++;
    int v8 = 2 * (3 * v6 + 3);

    double v7 = 100000.0;
    int v11 = 0;
    int i   = 0;
    for (;;) {
        double result = v7 * (double)(width / v8);
        v11 = i;
        i++;
        if (result <= v42) break;
        if (i > 13) return;
        v7 = v54[i];
    }

    double v49 = (v11 >= 1) ? v54[v11 - 1] : v54[0];

    double v41 = v56[v11];
    double v14 = v55[v11];

    #define F2PX(f) ((int)(((f) - v47) / v42 * (double)width))

    {
        int v12 = (int)ceil(v47 / v41);
        double v13 = (double)v12 * v41;
        while (v13 <= v48) {
            int px = F2PX(v13);
            if (px >= 0 && px < width) {
                buf[0 * width + px] = 1;
                if (v49 == v14)
                    buf[1 * width + px] = 1;
            }
            v12++;
            v13 = (double)v12 * v41;
        }
    }

    {
        int v18 = (int)ceil(v47 / v14);
        double f = (double)v18 * v14;
        while (f <= v48) {

            double rem = fmod(fabs(f / v49) + 1e-9, 1.0);
            if (rem > 0.02 && rem < 0.98) { v18++; f = (double)v18 * v14; continue; }

            int px = F2PX(f);

            for (int row = 0; row <= 4; row++)
                if (px >= 0 && px < width)
                    buf[row * width + px] = 1;

            char s[32];
            int ndec = (v11 < 15) ? v52[v11] : 0;
            snprintf(s, sizeof(s), "%.*f", ndec, f);
            int len = (int)strlen(s);

            int text_w = len * (FONT_W + 1) - 1;
            int label_x = px - text_w / 2;
            if (label_x < BANDLABEL_MARGIN_X)
                label_x = BANDLABEL_MARGIN_X;
            if (label_x + text_w >= width - BANDLABEL_MARGIN_X)
                label_x = width - BANDLABEL_MARGIN_X - text_w - 1;
            if (label_x >= BANDLABEL_MARGIN_X &&
                label_x + text_w < width - BANDLABEL_MARGIN_X)
                bl_draw_str(buf, width, label_x, AXIS_LABEL_Y, s, 1);

            v18++;
            f = (double)v18 * v14;
        }
    }

    bandlabel_overlay_bandplan(buf, width, v47, v48);

    #undef F2PX
}

static int bl_write_tile_png(const char *fname, const uint8_t *buf,
                               int stride, int tile_x)
{
    static png_color pal4[5] = {
        {  0,   0,   0},
        {255, 255, 255},
        {192,   0, 192},
        {  0, 128,   0},
        {  0,   0, 255},
    };

    FILE *fp = fopen(fname, "wb");
    if (!fp) return -1;

    png_structp png = png_create_write_struct(
        PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return -1; }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(fp); return -1; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp); return -1;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, 1024, BANDLABEL_H, 8,
                 PNG_COLOR_TYPE_PALETTE,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_set_PLTE(png, info, pal4, 5);
    png_write_info(png, info);

    uint8_t row_tmp[1024];
    for (int row = 0; row < BANDLABEL_H; row++) {
        memcpy(row_tmp, buf + row * stride + tile_x * 1024, 1024);
        png_write_row(png, row_tmp);
    }

    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 0;
}

int waterfall_write_bandlabel_png(const char *path_prefix,
                                   double center_khz, double bw_khz,
                                   int width __attribute__((unused)))
{
    int samplerate = (int)(bw_khz * 2000.0);

    int maxzoom = 0;
    if      (samplerate >= 3200000) maxzoom = 5;
    else if (samplerate >= 1600000) maxzoom = 4;
    else if (samplerate >=  800000) maxzoom = 3;
    else if (samplerate >=  400000) maxzoom = 2;
    else if (samplerate >=  200000) maxzoom = 1;

    for (int zoom = 0; zoom <= maxzoom; zoom++) {
        int w     = 1024 << zoom;
        int ntile = 1    << zoom;

        uint8_t *buf = (uint8_t *)calloc((size_t)(w * BANDLABEL_H), 1);
        if (!buf) return -1;

        bandlabel_fill(buf, w, samplerate, center_khz);

        for (int tile = 0; tile < ntile; tile++) {
            char fname[512];
            if (ntile == 1 && zoom == 0)
                snprintf(fname, sizeof(fname), "%s", path_prefix);
            else
                snprintf(fname, sizeof(fname), "%sb%iz%ii%i.png",
                         path_prefix, 0, zoom, tile);

            int rc = bl_write_tile_png(fname, buf, w, tile);
            if (rc != 0) { free(buf); return -1; }
        }
        free(buf);
    }
    return 0;
}

typedef struct { uint8_t *p; int bit; } Bitwriter;
static void bw_init(Bitwriter *w, uint8_t *p) { w->p = p; w->bit = 0; *p = 0; }
static void bw_write(Bitwriter *w, unsigned int val, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--) {
        int b = (int)((val >> i) & 1u);
        *w->p |= (uint8_t)(b << (7 - (w->bit & 7)));
        w->bit++;
        if ((w->bit & 7) == 0) { w->p++; *w->p = 0; }
    }
}
static int bw_bytes(Bitwriter *w) { return (w->bit + 7) / 8; }

static int wf9_encode_symbol(Bitwriter *bw, int r, int m)
{

    static const struct { int r; unsigned int bits; int nbits; } tbl0[] = {
        {  0, 0x0, 1 }, {  1, 0x4, 3 }, { -1, 0x5, 3 }, {  3, 0x6, 3 },
        { -3, 0xE, 4 },
        {  5, 0x78, 7 }, {  7, 0x79, 7 }, { -5, 0x7C, 7 }, { -7, 0x7D, 7 },
        {  9, 0xF4, 8 }, { 11, 0xF5, 8 }, { 13, 0xF6, 8 }, { 15, 0xF7, 8 },
        { -9, 0xFC, 8 }, {-11, 0xFD, 8 }, {-13, 0xFE, 8 }, {-15, 0xFF, 8 },
    };

    static const struct { int r; unsigned int bits; int nbits; } tbl1[] = {
        {  0, 0x0, 1 }, {  1, 0x2, 2 }, { -1, 0x3, 2 },
    };

    static const struct { int r; unsigned int bits; int nbits; } tbl2[] = {
        {  0, 0x0, 1 }, {  3, 0x4, 3 }, { -3, 0x5, 3 }, {  5, 0x6, 3 },
        { -5, 0xE, 4 },
        {  7, 0x78, 7 }, {  9, 0x79, 7 }, { 11, 0x7A, 7 },
        { -7, 0x7C, 7 }, { -9, 0x7D, 7 }, {-11, 0x7E, 7 },
        { 13, 0xF6, 8 }, { 15, 0xF7, 8 }, {-13, 0xFE, 8 }, {-15, 0xFF, 8 },
    };

    if (m == 1) {
        for (int i = 0; i < 3; i++) {
            if (tbl1[i].r == r) { bw_write(bw, tbl1[i].bits, tbl1[i].nbits); return r; }
        }

        bw_write(bw, 0x0, 1); return 0;
    }
    if (m == 2) {
        for (int i = 0; i < 15; i++) {
            if (tbl2[i].r == r) { bw_write(bw, tbl2[i].bits, tbl2[i].nbits); return r; }
        }

        bw_write(bw, 0x0, 1); return 0;
    }

    for (int i = 0; i < 17; i++) {
        if (tbl0[i].r == r) { bw_write(bw, tbl0[i].bits, tbl0[i].nbits); return r; }
    }
    bw_write(bw, 0x0, 1); return 0;
}

static int wf9_best_r(int prev_p, int s, int target, int m)
{
    static const int cands0[] = { 0,1,-1,3,-3,5,7,-5,-7,9,11,13,15,-9,-11,-13,-15 };
    static const int cands1[] = { 0,1,-1 };
    static const int cands2[] = { 0,3,-3,5,-5,7,9,11,-7,-9,-11,13,15,-13,-15 };
    const int *cands;
    int ncands;
    if (m == 1) { cands = cands1; ncands = 3; }
    else if (m == 2) { cands = cands2; ncands = 15; }
    else { cands = cands0; ncands = 17; }

    int best_r = 0, best_err = 0x7FFFFFFF;
    for (int i = 0; i < ncands; i++) {
        int r    = cands[i];
        int new_s = s + r * 16;
        int new_p = prev_p + new_s;
        if (new_p < 8)   new_p = 8;
        if (new_p > 248) new_p = 248;
        int err = new_p - target;
        if (err < 0) err = -err;
        if (err < best_err) { best_err = err; best_r = r; }
    }
    return best_r;
}

static int wf10_best_r(int prev_p, int target, int m, int n)
{
    static const int cands0[] = { 0,1,-1,3,-3,5,7,-5,-7,9,11,13,15,-9,-11,-13,-15 };
    static const int cands1[] = { 0,1,-1 };
    static const int cands2[] = { 0,3,-3,5,-5,7,9,11,-7,-9,-11,13,15,-13,-15 };
    const int *cands;
    int ncands;
    if (m == 1) { cands = cands1; ncands = 3; }
    else if (m == 2) { cands = cands2; ncands = 15; }
    else { cands = cands0; ncands = 17; }

    int best_r = 0;
    int best_err = 0x7FFFFFFF;
    for (int i = 0; i < ncands; i++) {
        int r = cands[i];
        int delta = r << 4;
        if (delta == 16 || delta == -16)
            delta >>= n;
        int new_p = prev_p + delta;
        if (new_p < 0) new_p = 0;
        if (new_p > 255) new_p = 255;
        int err = new_p - target;
        if (err < 0) err = -err;
        if (err < best_err) {
            best_err = err;
            best_r = r;
        }
    }
    return best_r;
}

int waterfall_encode_row_f9(const uint8_t *src, uint8_t *prev_row,
                             int width, uint8_t *dst)
{
    if (width <= 0 || width > 1024) return 0;

    Bitwriter bw;
    bw_init(&bw, dst);

    int s = 0;
    int m = 0;

    for (int k = 0; k < width; k++) {
        int target = (int)src[k];
        int prev_p = (int)prev_row[k];

        int r = wf9_best_r(prev_p, s, target, m);

        wf9_encode_symbol(&bw, r, m);

        s += r << 4;
        int new_p = prev_p + s;
        if (new_p < 8)   new_p = 8;
        if (new_p > 248) new_p = 248;
        prev_row[k] = (uint8_t)new_p;

        if (r == 0)           m = 0;
        else if (r == 1 || r == -1) m = 1;
        else                  m = 2;
    }

    return bw_bytes(&bw);
}

int waterfall_encode_row_f10(const uint8_t *src, uint8_t *prev_row,
                              uint8_t *prev_n, int width, uint8_t *dst)
{
    if (!src || !prev_row || !prev_n || !dst) return 0;
    if (width <= 0 || width > 1024) return 0;

    Bitwriter bw;
    bw_init(&bw, dst);

    int m = 0;

    for (int k = 0; k < width; k++) {
        int target = (int)src[k];
        int prev_p = (int)prev_row[k];
        int n = (int)prev_n[k];
        if (n < 0) n = 0;
        if (n > 4) n = 4;

        int r = wf10_best_r(prev_p, target, m, n);

        wf9_encode_symbol(&bw, r, m);

        int delta = r << 4;
        if (delta == 16 || delta == -16)
            delta >>= n;
        int new_p = prev_p + delta;
        if (new_p < 0) new_p = 0;
        if (new_p > 255) new_p = 255;
        prev_row[k] = (uint8_t)new_p;

        if (delta == 0) {
            if (n < 4) n++;
        } else {
            n = 0;
        }
        prev_n[k] = (uint8_t)n;

        if (r == 0) m = 0;
        else if (r == 1 || r == -1) m = 1;
        else m = 2;
    }

    return bw_bytes(&bw);
}

int waterfall_compress_row(const uint8_t *src, int len, uint8_t *dst, int format)
{
    (void)format;

    if (len <= 0) return 0;
    dst[0] = 0x00;
    dst[1] = (uint8_t)(len & 0xFF);
    dst[2] = (uint8_t)((len >> 8) & 0xFF);
    memcpy(dst + 3, src, (size_t)len);
    return len + 3;
}

int waterfall_write_band_pngs(const char *outdir, int band_idx,
                               double center_khz, int samplerate, int maxzoom)
{
    for (int zoom = 0; zoom <= maxzoom; zoom++) {
        int w     = 1024 << zoom;
        int ntile = 1    << zoom;

        uint8_t *buf = (uint8_t *)calloc((size_t)(w * BANDLABEL_H), 1);
        if (!buf) return -1;

        bandlabel_fill(buf, w, samplerate, center_khz);

        for (int tile = 0; tile < ntile; tile++) {
            char fname[512];
            snprintf(fname, sizeof(fname), "%s/b%iz%ii%i.png",
                     outdir, band_idx, zoom, tile);

            int rc = bl_write_tile_png(fname, buf, w, tile);
            if (rc != 0) { free(buf); return -1; }
        }
        free(buf);
    }
    return 0;
}

void waterfall_render_text_rows(const char *text, uint8_t out[32][1024])
{

    uint8_t s[32][1024];
    memset(s, 0, sizeof(s));

#define GS(r, c) (((r) >= 0 && (r) < 32 && (c) >= 0 && (c) < 1024) ? s[(r)][(c)] : 0)

    int v1 = 0;
    while (text[v1] && v1 < 80) v1++;
    int v2 = 512 - 6 * v1;
    int v3 = 6 * v1 + 512;

    for (int i = 2; i != 30; i++) {
        if (v2 >= v3) continue;
        const char *v5 = text;
        int v6 = v2;
        int v7 = 0;
        do {
            unsigned char ch = (unsigned char)*v5;

            int fi = -6 + 6 * (i >> 1) + (v7 >> 1);
            if (fi >= 0 && fi < 48 && wf_font[ch][fi] > 32)
                if (v6 >= 0 && v6 < 1024)
                    s[i][v6] = 2;
            v7++;
            if (v7 > 11) { v5++; v7 = 0; }
            v6++;
        } while (v6 != v3);
    }

    int v8  = v3 + 1;
    int v33 = v2 - 3;
    for (int v9 = 2; v9 != 32; v9++) {
        if (v33 > v8) continue;
        int v10 = v2 - 2;
        int v12;
        do {
            v12 = v10;
            int tr = v9 - 1, tc = v10 - 1;
            if (tr >= 0 && tr < 20 && tc >= 0 && tc < 1024 && GS(tr, tc) != 2) {

                int sum = (GS(v9,   tc) & 2)
                        + (GS(v9-2, tc) & 2)
                        + (GS(tr, v10  ) & 2)
                        + (GS(tr, v10-2) & 2);
                if (sum > 3)
                    s[tr][tc] = 4;
            }
            v10++;
        } while (v8 >= v12);
    }

    for (int v14 = 2; v14 < 32; v14++) {
        if (v33 > v8) continue;
        int v17 = v2 - 2;
        int v22;
        do {
            v22 = v17;
            int tr = v14 - 1, tc = v17 - 1;
            if (tr >= 0 && tr < 20 && tc >= 0 && tc < 1024 && GS(tr, tc) == 0) {
                uint8_t v27 = (uint8_t)(
                    GS(tr,      tc-2)  |
                    GS(v14+1,   tc  )  |
                    GS(v14-3,   tc  )  |
                    GS(v14-2,   v17 )  |
                    GS(v14,     v17 )
                );
                uint8_t combined = (uint8_t)(v27
                    | GS(v14,   v17-2)
                    | GS(v14-2, v17-2)
                    | GS(tr,    tc+2 )
                );
                if ((combined & 6) != 0)
                    s[tr][tc] = 1;
            }
            v17++;
        } while (v22 <= v8);
    }

#undef GS

    memcpy(out, s, 32 * 1024);
}
