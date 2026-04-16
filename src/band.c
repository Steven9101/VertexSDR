// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#define _GNU_SOURCE
#include "band.h"
#include "dsp.h"
#include "audio.h"
#include "config.h"
#include "fft_backend.h"
#include "common.h"
#ifdef USE_VULKAN
#include "vk_waterfall.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fftw3.h>

struct BandState bands[MAX_BANDS];
int num_bands = 0;
int fftplaneffort = 0;

static int ilog2(int n) {
    int r = 0;
    while (n > 1) { n >>= 1; r++; }
    return r;
}

int band_fftlen_from_samplerate(int samplerate, int device_type,
                                 int *fftlen, int *fftlen2, int *maxzoom)
{
    int v = samplerate;
    if (v > 400000)
        v = (int)((double)v / 1000.0 + 0.5) * 1000;

    switch (v) {
        case 24000:   *fftlen = 768;   *fftlen2 = 1024; *maxzoom = 1; return 1;
        case 48000:   *fftlen = 1536;  *fftlen2 = 1024; *maxzoom = 1; return 1;
        case 59733:   *fftlen = 2048;  *fftlen2 = 1024; *maxzoom = 1; return 1;
        case 96000:
        case 101250:
        case 116000:  *fftlen = 3072;  *fftlen2 = 2048; *maxzoom = 2; return 1;
        case 192000:
        case 192308:
        case 200000:
        case 224000:  *fftlen = 6144;  *fftlen2 = 4096; *maxzoom = 3; return 1;
        case 250000:  *fftlen = 4096;  *fftlen2 = 0x2000; *maxzoom = 1; return 1;
        case 256000:
        case 512000:
        case 1024000:
        case 2048000:
            if (device_type == DEVTYPE_RTLSDR ||
                device_type == DEVTYPE_TCPSDR ||
                device_type == DEVTYPE_STDIN) {

                int flen = (v / 32000) << 10;

                *fftlen  = flen;
                *fftlen2 = flen >> 1;
                *maxzoom = ilog2(flen) - 10;
                return 1;
            }
            return 0;
        case 8000000: {

            *fftlen  = 131072;
            *fftlen2 = 65536;
            *maxzoom = 7;
            return 1;
        }
        case 20000000: {
            *fftlen  = 262144;
            *fftlen2 = 131072;
            *maxzoom = 8;
            return 1;
        }
        case 30000000: {
            *fftlen  = 4194304;
            *fftlen2 = 4194304;
            *maxzoom = 10;
            return 1;
        }
        case 60000000: {
            *fftlen  = 524288;
            *fftlen2 = 524288;
            *maxzoom = 9;
            return 1;
        }
        case 384000:
        case 384616:
        case 400000:  *fftlen = 12288; *fftlen2 = 0x2000; *maxzoom = 4; return 1;
        case 448000:

            *fftlen  = 16384;
            *fftlen2 = 8192;
            *maxzoom = 4;
            return 1;
        case 768000:
        case 769232:
        case 800000:
        case 1536000:
        case 2880000: {

            int flen = ((v + 192000) / 256000) << 13;
            *fftlen  = flen;
            *fftlen2 = 2 * (flen / 3);
            *maxzoom = ilog2(*fftlen2) - 9;
            return 1;
        }
        default:
            fprintf(stderr, "Samplerate %i is not supported yet; continuing anyway,"
                    " but this probably is not going to work well...\n", samplerate);
            *fftlen  = 1536;
            *fftlen2 = 1024;
            *maxzoom = 1;
            return 0;
    }
}

int band_init(struct BandState *b, int reinit)
{
    int samplerate = b->samplerate;
    int noniq      = b->noniq;
    int fftlen, fftlen2, maxzoom;

    if (noniq) {
        b->samplerate = samplerate / 2;
        b->progfreq_rad_per_sample = 0.0f;
        b->centerfreq_khz += (double)(samplerate / 2) / 2000.0;
        samplerate = b->samplerate;
    }

    b->device_type = DEVTYPE_ALSA;
    if (b->device_name && b->device_name[0] == '!') {
        if (strncasecmp(b->device_name + 1, "rtlsdr", 6) == 0)
            b->device_type = DEVTYPE_RTLSDR;
        else if (strncasecmp(b->device_name + 1, "tcpsdr", 6) == 0)
            b->device_type = DEVTYPE_TCPSDR;
        else if (strncasecmp(b->device_name + 1, "stdin", 5) == 0)
            b->device_type = DEVTYPE_STDIN;
    }

    if (b->centerfreq_khz == -9999.0) {
        fprintf(stderr, "Missing centerfrequency for band %s\n", b->name);
        b->centerfreq_khz = 0.0;
    }

    b->alsa_buf_frames = 10000;

    if (samplerate == -9999 || samplerate == 0) {
        fprintf(stderr, "Missing samplerate for band %s\n", b->name);
        samplerate = 48000;
        b->samplerate = samplerate;
        fftlen  = 1536;
        fftlen2 = 1024;
        maxzoom = 1;
        goto fftlen_set;
    }

    band_fftlen_from_samplerate(samplerate, b->device_type, &fftlen, &fftlen2, &maxzoom);

    if (b->extrazoom > 0) {
        if (b->extrazoom > 5) {
            fprintf(stderr, "Extrazoom %i not possible, setting to 0.\n", b->extrazoom);
            b->extrazoom = 0;
        } else if (b->extrazoom + maxzoom > 7) {
            fprintf(stderr, "Extrazoom %i not possible, setting to 0.\n", b->extrazoom);
            b->extrazoom = 0;
        } else {
            float gain_reduction = (float)(6 * b->extrazoom);
            b->gain_adj = b->gain_db - gain_reduction;
            fftlen2 <<= b->extrazoom;
        }
    } else {
        b->gain_adj = b->gain_db;
    }

fftlen_set:
    b->fftlen   = fftlen;
    b->fftlen2  = fftlen2;
    b->maxzoom  = maxzoom + b->extrazoom;
    b->audiolen = 256;
    b->Wffraction = 1;

    int fftlen_real_tmp = noniq ? 2 * fftlen : fftlen;
    b->audio_overlap = (fftlen_real_tmp / 2 > 1023) ? fftlen_real_tmp / 2 : 1024;

    b->Wffftlen  = noniq ? 2 * fftlen2 : fftlen;
    b->fftlen_real = noniq ? 2 * fftlen : fftlen;

    b->half_fftlen = b->fftlen_real / 4;

    b->freq_step_half = (double)samplerate / (double)fftlen;
    b->freq_step      = b->freq_step_half * 2.0;
    b->samplerate_ratio_256 = (int)((float)((float)samplerate * 256.0f) / (float)fftlen + 0.5f);

    if (b->progfreq_khz >= 0.0) {
        double progfreq_hz = b->progfreq_khz * 1000.0;
        b->progfreq_rad_per_sample = (float)(progfreq_hz * (2.0 * M_PI / samplerate));
    }

    b->peak_max = 0x7FFFFFFF;
    b->peak_min = -2147483647;

    b->read_pos   = 0;
    b->half_fftlen = b->fftlen_real / 4;
    b->sample_count = 0;
    b->fft_phase   = 0;
    b->fft_trigger = b->fftlen_real / 4;
    b->wf_ring_pos  = 0;
    b->wf_ring_fill = 0;
    b->agc_state  = 0;
    b->agc_hold   = 0;
    b->agc_counter = 0;
    b->fd2        = -1;
    b->audio_clients = 0;
    b->self_ptr   = b;

    if (b->hpf_freq > 0.0f && samplerate > 0) {
        b->dc_alpha = 1.0f - expf(-2.0f * (float)M_PI * b->hpf_freq / (float)samplerate);
    } else {
        b->dc_alpha = 0.0f;
    }
    b->dc_avg_i = 0.0f;
    b->dc_avg_q = 0.0f;

    if (!reinit) {
        b->noiseblanker_state = 0;
        b->users_at_start = 0;
    }

    size_t n_clients = (size_t)(b->audio_clients > 0 ? b->audio_clients : 1);
    size_t ring_sz = sizeof(int16_t) * 2 * (size_t)b->fftlen_real * n_clients;
    b->ring_buf = (int16_t *)malloc(ring_sz);
    if (!b->ring_buf) { perror("malloc ring_buf"); return -1; }
    memset(b->ring_buf, 0, ring_sz);

    if (noniq) {

        b->fft_in_r2c = (float *)malloc(sizeof(float) * 2 * fftlen);
        b->fft_out_cx = NULL;
        b->fft_work_buf = (float *)calloc(fftlen + 2049, sizeof(fftwf_complex));
        b->fft_out_r2c = (fftwf_complex *)(b->fft_work_buf + 1024);
        b->fft_filter_buf2 = (float *)malloc(sizeof(float) * (b->audio_overlap + 8 * fftlen2));
        b->fft_in2_r2c = (float *)malloc(sizeof(float) * 4 * fftlen2);
        b->fft_out2 = (fftwf_complex *)malloc(sizeof(fftwf_complex) * (fftlen2 + 1));
    } else {

        b->fft_in_cx = (fftwf_complex *)malloc(sizeof(fftwf_complex) * fftlen);
        b->fft_out_cx = (fftwf_complex *)malloc(sizeof(fftwf_complex) * fftlen);
        b->fft_work_buf = (float *)calloc(fftlen + 2048, sizeof(fftwf_complex));
        b->fft_out_r2c = (fftwf_complex *)(b->fft_work_buf + 1024);

        b->fft_filter_buf = (float *)calloc(4 * fftlen2, sizeof(fftwf_complex));
        b->fft_filter_buf2 = (float *)malloc(sizeof(float) * (fftlen2 + b->audio_overlap));
        b->fft_in2 = (fftwf_complex *)malloc(sizeof(fftwf_complex) * fftlen2);
        b->fft_out2 = (fftwf_complex *)malloc(sizeof(fftwf_complex) * fftlen2);
        b->wf_ring_pos = 0;
        b->wf_ring_fill = 0;
    }

    int window_size = b->Wffftlen;
    b->window_buf = NULL;
    if (window_size > 0) {
        b->window_buf = (float *)malloc(sizeof(float) * window_size);
        if (!b->window_buf) { perror("malloc window_buf"); return -1; }

        double N_d = (double)window_size;
        double half_plus_one = 0.5 * (N_d + 1.0);
        int half = window_size / 2;
        for (int i = 0; i < half; i++) {
            double x = (half_plus_one - (double)i) / N_d;
            double w = 0.35875
                     + 0.48829 * cos(2.0 * M_PI * x)
                     + 0.14128 * cos(4.0 * M_PI * x)
                     + 0.01168 * cos(6.0 * M_PI * x);
            b->window_buf[i]                    = (float)w;
            b->window_buf[window_size - 1 - i]  = (float)w;
        }

    }

    if (b->delay_samples > 0) {
        b->delay_buf = (float *)calloc((size_t)(b->delay_samples * 2), sizeof(float));
        if (!b->delay_buf) { perror("malloc delay_buf"); return -1; }
        b->delay_buf_ptr = b->delay_buf;
    }

    memset(b->user_blocks, 0, sizeof(b->user_blocks));

    for (int z = 0; z < WF_MAX_ZOOMS; z++) {
        b->wf_zoom_row[z]   = NULL;
        b->wf_zoom_power[z] = NULL;
    }
    b->wf_pyramid_valid = 0;
    for (int z = 0; z <= b->maxzoom && z < WF_MAX_ZOOMS; z++) {
        int px = 1024 << z;
        b->wf_zoom_row[z] = (uint8_t *)calloc((size_t)px, 1);
        b->wf_zoom_power[z] = (float *)calloc((size_t)px, sizeof(float));
        if (!b->wf_zoom_row[z] || !b->wf_zoom_power[z]) {
            perror("malloc wf_zoom pyramid");
            return -1;
        }
    }

#ifdef USE_VULKAN
    b->vk_wf = NULL;
    if (vk_waterfall_prepare_band(b) == 0)
        fprintf(stderr, "Vulkan waterfall enabled for band %d\n", b->band_index);
#endif

    unsigned int fflags = fftw_flags_from_effort(fftplaneffort);

    fprintf(stderr, "Planning FFTs for band %i...", b->band_index);
    fflush(stderr);

    b->fft_backend = FFT_BACKEND_FFTW;
    b->plan_fwd = NULL;
    b->plan_fwd2 = NULL;
    b->plan_inv2 = NULL;

    if (fft_backend_prepare_band(b, fftlen, fftlen2, fflags) != 0) {
        b->fft_backend = FFT_BACKEND_FFTW;
        if (noniq) {
            b->plan_fwd = fftwf_plan_dft_r2c_1d(2 * fftlen,
                                                  b->fft_in_r2c,
                                                  b->fft_out_r2c,
                                                  fflags);
            b->plan_fwd2 = fftwf_plan_dft_r2c_1d(2 * fftlen2,
                                                   b->fft_in2_r2c,
                                                   b->fft_out2,
                                                   fflags);
        } else {
            b->plan_fwd = fftwf_plan_dft_1d(fftlen,
                                             b->fft_in_cx,
                                             b->fft_out_cx,
                                             FFTW_FORWARD,
                                             fflags);
            b->plan_fwd2 = fftwf_plan_dft_1d(fftlen2,
                                              b->fft_in2,
                                              b->fft_out2,
                                              FFTW_FORWARD,
                                              fflags);
            b->plan_inv2 = fftwf_plan_dft_1d(fftlen2,
                                              b->fft_in2,
                                              b->fft_out2,
                                              FFTW_BACKWARD,
                                              fflags);
        }
    }

    fprintf(stderr, " done (%s).\n", fft_backend_name(b->fft_backend));

    dsp_init_band_demod_tables(b);

    b->started = 1;
    return 0;
}

int band_write_bandinfo_js(const char *pubdir)
{

    char tmpdir[512];
    snprintf(tmpdir, sizeof(tmpdir), "%s/tmp", pubdir);
    mkdir(tmpdir, 0755);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    unsigned int ts = (unsigned int)tv.tv_sec;

    char path[512];
    snprintf(path, sizeof(path), "%s/tmp/bandinfo.js", pubdir);

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }

    int nb = config.num_bands;
    fprintf(f, "var nbands=%d;\n", nb);
    fprintf(f, "var ini_freq=%f;\n", config.initial_freq);
    fprintf(f, "var ini_mode='%s';\n", config.initial_mode[0] ? config.initial_mode : "usb");
    fprintf(f, "var chseq=2;\n");
    fprintf(f, "var bandinfo= [\n");

    for (int i = 0; i < nb; i++) {
        struct BandState *b = &bands[i];

        double tuningstep_khz = (b->fftlen > 0)
            ? (double)b->samplerate / b->fftlen / 1000.0 : 0.001;
        double maxlinbw_half = config.allowwide
            ? (double)(b->band_demod_tables ? b->band_demod_tables[DT_SSB_WIDE].half_size : 256)
            : (double)(b->band_demod_tables ? b->band_demod_tables[DT_SSB_NARROW].half_size : 128);
        double maxlinbw_khz   = tuningstep_khz * maxlinbw_half;
        double vfo = (b->vfo_khz > 0.0) ? b->vfo_khz : b->centerfreq_khz;

        if (i > 0) fprintf(f, ",\n");
        fprintf(f, "  { centerfreq: %f,\n",   b->centerfreq_khz);
        fprintf(f, "    samplerate: %f,\n",    b->samplerate / 1000.0);
        fprintf(f, "    tuningstep: %f,\n",    tuningstep_khz);
        fprintf(f, "    maxlinbw: %f,\n",      maxlinbw_khz);
        fprintf(f, "    vfo: %f,\n",           vfo);
        fprintf(f, "    maxzoom: %i,\n",       b->maxzoom);
        fprintf(f, "    name: '%s',\n",        b->name);

        fprintf(f, "    scaleimgs: [");
        for (int z = 0; z <= b->maxzoom; z++) {
            int ntiles = 1 << z;
            if (z > 0) fprintf(f, ",");
            fprintf(f, "\n      [");
            for (int t = 0; t < ntiles; t++) {
                if (t > 0) fprintf(f, ",");
                fprintf(f, "\"tmp/b%dz%di%d.png?v=%u\"", i, z, t, ts);
            }
            fprintf(f, "]");
        }
        fprintf(f, "]\n  }");
    }
    fprintf(f, "\n];\n");
    fprintf(f, "var dxinfoavailable=0;\n");
    fprintf(f, "var idletimeout=%d;\n",
            config.idletimeout > 0 ? config.idletimeout * 1000 : 14400000);
    fprintf(f, "var has_mobile=1;\n");

    fclose(f);
    fprintf(stderr, "Writing frequency axis images... done\n");
    return 0;
}

void band_deinit(struct BandState *b)
{
    if (b->device_type == DEVTYPE_STDIN)
        stdin_stop_device(b);

    if (b->fd >= 0)  { close(b->fd);  b->fd  = -1; }
    if (b->fd2 >= 0) { close(b->fd2); b->fd2 = -1; }

    dsp_destroy_band_demod_tables(b);
    fft_backend_destroy_band(b);

    if (b->plan_inv2) { fftwf_destroy_plan(b->plan_inv2); b->plan_inv2 = NULL; }
    if (b->plan_fwd2) { fftwf_destroy_plan(b->plan_fwd2); b->plan_fwd2 = NULL; }
    if (b->plan_fwd)  { fftwf_destroy_plan(b->plan_fwd);  b->plan_fwd  = NULL; }

#ifdef USE_VULKAN
    vk_waterfall_destroy_band(b);
#endif

    for (int z = 0; z < WF_MAX_ZOOMS; z++) {
        free(b->wf_zoom_row[z]);   b->wf_zoom_row[z]   = NULL;
        free(b->wf_zoom_power[z]); b->wf_zoom_power[z] = NULL;
    }
    b->wf_pyramid_valid = 0;

    free(b->ring_buf);       b->ring_buf = NULL;
    free(b->fft_in_cx);      b->fft_in_cx = NULL;
    free(b->fft_in_r2c);     b->fft_in_r2c = NULL;
    free(b->fft_work_buf);   b->fft_work_buf = NULL;
    free(b->fft_filter_buf); b->fft_filter_buf = NULL;
    free(b->fft_filter_buf2); b->fft_filter_buf2 = NULL;
    free(b->fft_phase_corr); b->fft_phase_corr = NULL;
    free(b->fft_in2);        b->fft_in2 = NULL;
    free(b->fft_in2_r2c);    b->fft_in2_r2c = NULL;
    free(b->fft_out2);       b->fft_out2 = NULL;
    free(b->fft_out_cx);     b->fft_out_cx = NULL;
    free(b->window_buf);     b->window_buf = NULL;
    free(b->delay_buf);      b->delay_buf = NULL;

    if (b->alsa_handle) {
        snd_pcm_close(b->alsa_handle);
        b->alsa_handle = NULL;
    }

    b->started = 0;
    free(b->balance_buf);    b->balance_buf = NULL;
    free(b->spectrum_buf);   b->spectrum_buf = NULL;
}

int band_load_balance(struct BandState *b, const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "band_load_balance: can't open '%s': %s\n",
                filename, strerror(errno));
        return -1;
    }

    int fftlen2   = b->fftlen2;
    int fftlen_r2 = b->fftlen_real / 2;

    free(b->fft_phase_corr);
    b->fft_phase_corr = (fftwf_complex *)malloc(sizeof(fftwf_complex) * (size_t)fftlen_r2);
    if (!b->fft_phase_corr) { fclose(f); return -1; }

    free(b->balance_buf);
    b->balance_buf = (fftwf_complex *)malloc(sizeof(fftwf_complex) * (size_t)fftlen2);
    if (!b->balance_buf) { fclose(f); return -1; }

    memset(b->fft_phase_corr, 0, sizeof(fftwf_complex) * (size_t)fftlen_r2);
    memset(b->balance_buf,    0, sizeof(fftwf_complex) * (size_t)fftlen2);

    int   prev_bin2  = 0;
    int   prev_bin_r = 0;
    float prev_re    = 0.0f;
    float prev_im    = 0.0f;

    char line[296];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] <= 32 || line[0] == '#') continue;
        float freq_norm, gain_re, gain_im;
        if (sscanf(line, "%f %f %f", &freq_norm, &gain_re, &gain_im) != 3) {
            fprintf(stderr, "band_load_balance: parse error: %s", line);
            continue;
        }

        freq_norm /= (float)b->samplerate;
        int bin2 = (int)((double)fftlen2 * (freq_norm + 0.5) + 0.5);
        int span2 = bin2 - prev_bin2;

        if (span2 > 0) {

            if (prev_bin_r * fftlen2 < bin2 * fftlen_r2) {
                fftwf_complex *tbl2 = b->fft_phase_corr;
                int n = fftlen_r2;
                int span_r = fftlen_r2 / 2;
                int neg_r  = -(fftlen_r2 / 2);
                for (int i = (int)((long long)fftlen2 * (prev_bin_r + 1) / fftlen_r2);
                         i <= (int)((long long)fftlen2 * bin2 / fftlen_r2);
                         i++) {
                    int k = prev_bin_r;
                    if (k >= span_r) k += neg_r;
                    int idx = k ^ (n / 2);

                    float ratio = (float)((long long)prev_bin_r * fftlen2) / (float)fftlen_r2;
                    float t = ratio - (float)(int)ratio;
                    tbl2[idx][0] = (1.0f - t) * prev_re + t * gain_re;
                    tbl2[idx][1] = (1.0f - t) * prev_im + t * gain_im;
                    prev_bin_r++;
                }
            }

            fftwf_complex *tbl = b->balance_buf;
            for (int k = 0; k < span2; k++) {
                float t  = (float)k / (float)span2;
                int   p  = prev_bin2 + k;
                int   idx = (fftlen2 / 2) ^ p;
                tbl[idx][0] = (1.0f - t) * prev_re + t * gain_re;
                tbl[idx][1] = (1.0f - t) * prev_im + t * gain_im;
            }

            prev_re   = gain_re;
            prev_im   = gain_im;
            prev_bin2 = bin2;
        }
    }

    for (int k = prev_bin2; k < fftlen2; k++) {
        int idx = (fftlen2 / 2) ^ k;
        b->balance_buf[idx][0] = prev_re;
        b->balance_buf[idx][1] = prev_im;
    }

    fclose(f);
    return 0;
}

int band_load_equalize(struct BandState *b, const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "band_load_equalize: can't open '%s': %s\n",
                filename, strerror(errno));
        return -1;
    }

    int fftlen2 = b->fftlen2;

    free(b->spectrum_buf);
    b->spectrum_buf = (float *)malloc(sizeof(float) * (size_t)fftlen2);
    if (!b->spectrum_buf) { fclose(f); return -1; }

    for (int i = 0; i < fftlen2; i++) b->spectrum_buf[i] = 1.0f;

    int   prev_bin = 0;
    float prev_db  = 0.0f;

    char line[312];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] <= 32 || line[0] == '#') continue;
        float freq_hz, gain_db;
        if (sscanf(line, "%f %f", &freq_hz, &gain_db) != 2) {
            fprintf(stderr, "band_load_equalize: parse error: %s", line);
            continue;
        }

        freq_hz /= (float)(b->samplerate / 1000);
        int bin = (int)((double)fftlen2 * (freq_hz + 0.5) + 0.5);
        int span = bin - prev_bin;

        if (span > 0) {
            for (int k = 0; k < span; k++) {
                float t = (float)k / (float)span;
                float db = (1.0f - t) * prev_db + t * gain_db;
                float lin = exp10f(db / 20.0f);
                int   p   = prev_bin + k;
                int   idx = p ^ (fftlen2 / 2);
                int   mir = fftlen2 - 1 - idx;
                if (idx >= 0 && idx < fftlen2) b->spectrum_buf[idx] = lin;
                if (mir >= 0 && mir < fftlen2) b->spectrum_buf[mir] = lin;
            }
            prev_bin = bin;
            prev_db  = gain_db;
        }
    }

    {
        float lin = exp10f(prev_db / 20.0f);
        for (int k = prev_bin; k < fftlen2; k++) {
            int idx = k ^ (fftlen2 / 2);
            int mir = fftlen2 - 1 - idx;
            if (idx >= 0 && idx < fftlen2) b->spectrum_buf[idx] = lin;
            if (mir >= 0 && mir < fftlen2) b->spectrum_buf[mir] = lin;
        }
    }

    fclose(f);

    int step = fftlen2 / 10;
    if (step < 1) step = 1;

    return 0;
}
