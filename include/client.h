// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#ifndef CLIENT_H
#define CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "band.h"
#include "dsp.h"

#define MAX_CLIENTS     512

#define CLIENT_BUF_SIZE 131072

#define CLIENT_FREE       0
#define CLIENT_HTTP       1
#define CLIENT_WEBSOCKET  2
#define CLIENT_CLOSING    3

#define DEMOD_AM   1
#define DEMOD_AMSYNC 2
#define DEMOD_FM   4
#define DEMOD_LSB  0
#define DEMOD_USB  0
#define DEMOD_CW   0

#define WS_OP_CONT   0x00
#define WS_OP_TEXT   0x01
#define WS_OP_BINARY 0x02
#define WS_OP_CLOSE  0x08
#define WS_OP_PING   0x09
#define WS_OP_PONG   0x0A

struct ClientDemodState {
    void *self_ptr;
    int   _pad_8[2];

    float prev_re;
    float prev_im;
    int   fm_squelch_state;

    float fm_prev2_re;

};

struct Client {

    int   state;
    int   fd;
    int   band_idx;
    int   client_type;

    double tune_norm;

    int   _pad24[2];

    int   tune_bin;
    int   last_tune_bin;

    int   _pad40[6];

    char  description[64];

    float smeter_power;
    float _pad136;
    float agc_gain;
    int   _pad144;

    double tune_khz;
    int    mode;
    int    bw_hz;
    double lo_khz;
    double hi_khz;
    int    squelch;
    int    autonotch;
    int    mute;
    float  ppm;
    float  ppml;
    int    agc_mode;
    float  gain_linear;
    int    band_idx_req;

    uint8_t  outbuf[CLIENT_BUF_SIZE];
    int      outbuf_len;
    int      outbuf_sent;

    uint8_t  inbuf[CLIENT_BUF_SIZE];
    int      inbuf_len;

    uint16_t audio_seqno;
    int      write_errors;
    unsigned int audio_drop_frames;
    time_t   last_audio_drop_log;
    char     username[32];

    int      zoom;
    int      wf_offset;
    int      audio_format;
    int      wf_format;
    int      wf_speed;
    int      wf_slow;
    int      wf_counter;
    int      wf_width;
    int      wf_scale;
    long long last_wf_us;
    unsigned int wf_last_pyramid_seq;
    int      uu_chseq;
    time_t   last_active;
    int      last_audio_rate;
    int      last_out_samples;

    int      js_mode;
    int      last_sent_mode;

    float    filter_buf[1024];

    int      filter_lo_bin;
    int      filter_hi_bin;
    int      filter_dirty;
    int      filter_mode_lo;
    void    *band_ptr;

    int      notch_tracked_bin;
    int      notch_consec_count;
    int      notch_confirmed_bin;
    int      notch_hold_counter;

    int      stream_write_len;
    int      stream_init_pad;

    int      demod_mode;
    struct DemodTable *demod_table;

    void    *demod_self_ptr;
    int      demod_pad_8[2];
    float    demod_prev_re;
    float    demod_prev_im;
    int      demod_fm_sq;
    float    demod_fm_prev2_re;
    double   carrier_freq;
    int      demod_agc_mode;
    int      demod_filter_dirty;

    int      pred_h[20];
    int      pred_x[20];
    int      pred_accum;
    int      block_size;
    int      quant_mode;
    int      header_counter;
    int      rate_changed;
    int      mode_changed;
    int      conv_changed;
    int      conv_type;
    int      demod_squelch;
    int      squelch_counter;
    int      demod_autonotch;
    int      demod_mute;
    float    squelch_threshold;
    float    demod_ppm;
    float    demod_ppml;
    float    compress_ema;

    float    fm_prev2_im;

    float    am_dc;
    float    amsync_dc;
    float    amsync_phase;
    float    amsync_freq;
    float    amsync_acq_freq;
    int      amsync_state;
    int      amsync_lock_count;
    double   amsync_coherence_ema;
    long long amsync_truefreq_mhz;

    float    audio_accum[256];
    int      audio_accum_pos;

    uint8_t  wf_prev_row[1024];
    uint8_t  wf_prev_n[1024];
    int      wf_prev_valid;
};

extern struct Client clients[MAX_CLIENTS];
extern int           num_clients;

void client_init_all(void);
struct Client *client_assign_fd(int fd, const char *ip_str);
void client_close(struct Client *c);
void client_ws_recv(struct Client *c, const uint8_t *buf, int len);
int  client_ws_send_binary(struct Client *c, const uint8_t *data, size_t len);
int  client_ws_send_text(struct Client *c, const char *text);
void client_flush_outbuf(struct Client *c);
void client_dispatch_audio(struct BandState *b, int phase_flag);
void client_parse_command(struct Client *c, const char *cmd, int len);

#endif
