// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#ifndef BAND_H
#define BAND_H

#include <stdint.h>
#include <fftw3.h>
#include <alsa/asoundlib.h>

#define MAX_BANDS       8
#define WF_MAX_ZOOMS   11
#define DEMOD_SSB_NARROW 128
#define DEMOD_SSB_WIDE   256
#define DEMOD_AM_NARROW  128
#define DEMOD_AM_WIDE    256
#define DEMOD_FFT_FM     1024

#define DEVTYPE_ALSA    0
#define DEVTYPE_RTLSDR  5
#define DEVTYPE_TCPSDR  6
#define DEVTYPE_STDIN   7
#define FFT_BACKEND_FFTW   0
#define FFT_BACKEND_VKFFT  1

struct BandState {
    int band_index;
    int device_type;
    int device_ch;
    char name[16];

    double centerfreq_hz;

    double centerfreq_khz;
    double vfo_khz;
    double progfreq_khz;
    int samplerate;
    int samplerate_ratio_256;
    double freq_step;
    double freq_step_half;

    int noniq;
    int swapiq;
    float gain_db;
    float gain_adj;
    int audiolen;
    int fftlen_real;
    int fftlen;
    int Wffftlen;
    int fftlen2;
    int Wffraction;
    int audio_overlap;
    int maxzoom;
    int extrazoom;
    int sample_count;
    int fft_backend;
    int phase_flag;
    int half_fftlen;
    int read_pos;
    int fd;
    int fd2;
    int alsa_buf_frames;
    int peak_max;
    int peak_min;

    fftwf_plan plan_fwd;
    fftwf_plan plan_fwd2;
    fftwf_plan plan_inv2;
    void *vkfft_fwd;
    void *vkfft_fwd2;

    int16_t *ring_buf;
    float *delay_buf;
    float *delay_buf_ptr;
    int delay_samples;
    float hpf_freq;

    fftwf_complex *fft_in_cx;
    float *fft_in_r2c;
    fftwf_complex *fft_out_cx;
    fftwf_complex *fft_out_r2c;
    float *fft_work_buf;
    float *fft_filter_buf;
    float *fft_filter_buf2;
    fftwf_complex *fft_in2;
    float *fft_in2_r2c;
    fftwf_complex *fft_out2;
    float *window_buf;
    fftwf_complex *balance_buf;
    fftwf_complex *fft_phase_corr;
    float *spectrum_buf;

    struct DemodTable *band_demod_tables;
    int band_demod_tables_owned;

    const char *device_name;
    const char *file_path;
    char *antenna_str;
    char *audioformat_str;
    char *stdinformat_str;
    void *rtl_handle;
    int tcp_port;
    int ppm_correction;
    int pad_alsa[2];
    int started;
    int pad_alsa2;
    int rtltcp_fd;
    snd_pcm_t *alsa_handle;

    int noiseblanker;

    int num_users;
    int total_users_ever;
    int users_at_start;
    uint64_t last_active_ts;
    int audio_clients;
    struct BandState *self_ptr;

    int fft_trigger;
    int fft_phase;

    int wf_ring_pos;
    int wf_ring_fill;

    unsigned int dispatch_counter;

    int wf_slow_min;

    int agc_state;
    int agc_hold;
    int agc_decay;
    float dc_avg_i;
    float dc_avg_q;
    float dc_alpha;
    float noise_blank_thresh;
    float noise_blank_enable;
    float file_frac_accum;

    int agc_gain;
    int agc_counter;
    float progfreq_rad_per_sample;
    float progfreq_phase;
    int noiseblanker_state;

    uint8_t *wf_zoom_row[WF_MAX_ZOOMS];
    float   *wf_zoom_power[WF_MAX_ZOOMS];
    int      wf_pyramid_valid;
    unsigned int wf_pyramid_seq;
    void    *vk_wf;

    uint8_t  wf_text_rows[32][1024];
    int      wf_text_pending;
    int      wf_text_row_applied;

    uint8_t user_blocks[1049084 - 480];
};

extern struct BandState bands[MAX_BANDS];
extern int num_bands;
extern int fftplaneffort;

static inline unsigned int fftw_flags_from_effort(int effort) {
    switch(effort) {
        case 1: return FFTW_MEASURE;
        case 2: return FFTW_PATIENT;
        case 3: return FFTW_EXHAUSTIVE;
        default: return FFTW_ESTIMATE;
    }
}

int band_fftlen_from_samplerate(int samplerate, int device_type,
                                 int *fftlen, int *fftlen2, int *maxzoom);

int  band_init(struct BandState *b, int reinit);
void band_deinit(struct BandState *b);
int  band_write_bandinfo_js(const char *pubdir);
int  band_load_balance(struct BandState *b, const char *filename);
int  band_load_equalize(struct BandState *b, const char *filename);

int alsa_open_device(struct BandState *b);
int alsa_read_samples(struct BandState *b);
int rtltcp_open_device(struct BandState *b);
int rtltcp_read_samples(struct BandState *b);

#endif
