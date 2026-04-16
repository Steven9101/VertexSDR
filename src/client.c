// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#include "client.h"
#include "band.h"
#include "config.h"
#include "dsp.h"
#include "waterfall.h"
#include "common.h"
#include "logging.h"
#include "chat.h"
#include "audio.h"

extern void stats_add_bytes(int type, long long n);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <poll.h>
#include <arpa/inet.h>
#include <math.h>

struct Client clients[MAX_CLIENTS];
int           num_clients = 0;
static float  g_prev_audio_peak[MAX_CLIENTS];
static unsigned int g_audio_overflow_count[MAX_CLIENTS];
static unsigned int g_audio_squelch_silence_count[MAX_CLIENTS];
static unsigned int g_audio_mute_silence_count[MAX_CLIENTS];
static long long g_amsync_last_sent_mhz[MAX_CLIENTS];
static int g_amsync_last_sent_state[MAX_CLIENTS];
static unsigned int g_amsync_keepalive_frames[MAX_CLIENTS];

static const uint8_t wf_scale2_remap[256] = {
    0x00, 0x01, 0x03, 0x04, 0x06, 0x07, 0x09, 0x0a, 0x0c, 0x0d, 0x0f, 0x10, 0x12, 0x13, 0x15, 0x16,
    0x18, 0x19, 0x1b, 0x1c, 0x1e, 0x1f, 0x21, 0x22, 0x24, 0x25, 0x27, 0x28, 0x2a, 0x2b, 0x2d, 0x2e,
    0x30, 0x31, 0x33, 0x34, 0x36, 0x37, 0x39, 0x3a, 0x3c, 0x3d, 0x3f, 0x40, 0x42, 0x43, 0x45, 0x46,
    0x48, 0x49, 0x4b, 0x4c, 0x4e, 0x4f, 0x51, 0x52, 0x54, 0x55, 0x57, 0x58, 0x5a, 0x5b, 0x5d, 0x5e,
    0x60, 0x61, 0x63, 0x64, 0x66, 0x67, 0x69, 0x6a, 0x6c, 0x6d, 0x6f, 0x70, 0x72, 0x73, 0x75, 0x76,
    0x78, 0x79, 0x7b, 0x7c, 0x7e, 0x7f, 0x81, 0x82, 0x84, 0x85, 0x87, 0x88, 0x8a, 0x8b, 0x8d, 0x8e,
    0x90, 0x91, 0x93, 0x94, 0x96, 0x97, 0x99, 0x9a, 0x9c, 0x9d, 0x9f, 0xa0, 0xa2, 0xa3, 0xa5, 0xa6,
    0xa8, 0xa9, 0xab, 0xac, 0xae, 0xaf, 0xb1, 0xb2, 0xb4, 0xb5, 0xb7, 0xb8, 0xba, 0xbb, 0xbd, 0xbe,
    0xc0, 0xc0, 0xc1, 0xc1, 0xc2, 0xc2, 0xc3, 0xc3, 0xc4, 0xc4, 0xc5, 0xc5, 0xc6, 0xc6, 0xc7, 0xc7,
    0xc8, 0xc8, 0xc9, 0xc9, 0xca, 0xca, 0xcb, 0xcb, 0xcc, 0xcc, 0xcd, 0xcd, 0xce, 0xce, 0xcf, 0xcf,
    0xd0, 0xd0, 0xd1, 0xd1, 0xd2, 0xd2, 0xd3, 0xd3, 0xd4, 0xd4, 0xd5, 0xd5, 0xd6, 0xd6, 0xd7, 0xd7,
    0xd8, 0xd8, 0xd9, 0xd9, 0xda, 0xda, 0xdb, 0xdb, 0xdc, 0xdc, 0xdd, 0xdd, 0xde, 0xde, 0xdf, 0xdf,
    0xe0, 0xe0, 0xe1, 0xe1, 0xe2, 0xe2, 0xe3, 0xe3, 0xe4, 0xe4, 0xe5, 0xe5, 0xe6, 0xe6, 0xe7, 0xe7,
    0xe8, 0xe8, 0xe9, 0xe9, 0xea, 0xea, 0xeb, 0xeb, 0xec, 0xec, 0xed, 0xed, 0xee, 0xee, 0xef, 0xef,
    0xf0, 0xf0, 0xf1, 0xf1, 0xf2, 0xf2, 0xf3, 0xf3, 0xf4, 0xf4, 0xf5, 0xf5, 0xf6, 0xf6, 0xf7, 0xf7,
    0xf8, 0xf8, 0xf9, 0xf9, 0xfa, 0xfa, 0xfb, 0xfb, 0xfc, 0xfc, 0xfd, 0xfd, 0xfe, 0xfe, 0xff, 0xff,
};

static const uint8_t wf_scale3_remap[256] = {
    0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x04, 0x04, 0x05, 0x05, 0x06, 0x06, 0x07, 0x07,
    0x08, 0x08, 0x09, 0x09, 0x0a, 0x0a, 0x0b, 0x0b, 0x0c, 0x0c, 0x0d, 0x0d, 0x0e, 0x0e, 0x0f, 0x0f,
    0x10, 0x10, 0x11, 0x11, 0x12, 0x12, 0x13, 0x13, 0x14, 0x14, 0x15, 0x15, 0x16, 0x16, 0x17, 0x17,
    0x18, 0x18, 0x19, 0x19, 0x1a, 0x1a, 0x1b, 0x1b, 0x1c, 0x1c, 0x1d, 0x1d, 0x1e, 0x1e, 0x1f, 0x1f,
    0x20, 0x20, 0x21, 0x21, 0x22, 0x22, 0x23, 0x23, 0x24, 0x24, 0x25, 0x25, 0x26, 0x26, 0x27, 0x27,
    0x28, 0x28, 0x29, 0x29, 0x2a, 0x2a, 0x2b, 0x2b, 0x2c, 0x2c, 0x2d, 0x2d, 0x2e, 0x2e, 0x2f, 0x2f,
    0x30, 0x30, 0x31, 0x31, 0x32, 0x32, 0x33, 0x33, 0x34, 0x34, 0x35, 0x35, 0x36, 0x36, 0x37, 0x37,
    0x38, 0x38, 0x39, 0x39, 0x3a, 0x3a, 0x3b, 0x3b, 0x3c, 0x3c, 0x3d, 0x3d, 0x3e, 0x3e, 0x3f, 0x3f,
    0x41, 0x42, 0x44, 0x45, 0x47, 0x48, 0x4a, 0x4b, 0x4d, 0x4e, 0x50, 0x51, 0x53, 0x54, 0x56, 0x57,
    0x59, 0x5a, 0x5c, 0x5d, 0x5f, 0x60, 0x62, 0x63, 0x65, 0x66, 0x68, 0x69, 0x6b, 0x6c, 0x6e, 0x6f,
    0x71, 0x72, 0x74, 0x75, 0x77, 0x78, 0x7a, 0x7b, 0x7d, 0x7e, 0x80, 0x81, 0x83, 0x84, 0x86, 0x87,
    0x89, 0x8a, 0x8c, 0x8d, 0x8f, 0x90, 0x92, 0x93, 0x95, 0x96, 0x98, 0x99, 0x9b, 0x9c, 0x9e, 0x9f,
    0xa1, 0xa2, 0xa4, 0xa5, 0xa7, 0xa8, 0xaa, 0xab, 0xad, 0xae, 0xb0, 0xb1, 0xb3, 0xb4, 0xb6, 0xb7,
    0xb9, 0xba, 0xbc, 0xbd, 0xbf, 0xc0, 0xc2, 0xc3, 0xc5, 0xc6, 0xc8, 0xc9, 0xcb, 0xcc, 0xce, 0xcf,
    0xd1, 0xd2, 0xd4, 0xd5, 0xd7, 0xd8, 0xda, 0xdb, 0xdd, 0xde, 0xe0, 0xe1, 0xe3, 0xe4, 0xe6, 0xe7,
    0xe9, 0xea, 0xec, 0xed, 0xef, 0xf0, 0xf2, 0xf3, 0xf5, 0xf6, 0xf8, 0xf9, 0xfb, 0xfc, 0xfe, 0xff,
};

void client_init_all(void)
{
    memset(clients, 0, sizeof(clients));
    memset(g_prev_audio_peak, 0, sizeof(g_prev_audio_peak));
    memset(g_audio_overflow_count, 0, sizeof(g_audio_overflow_count));
    memset(g_audio_squelch_silence_count, 0, sizeof(g_audio_squelch_silence_count));
    memset(g_audio_mute_silence_count, 0, sizeof(g_audio_mute_silence_count));
    memset(g_amsync_last_sent_mhz, 0, sizeof(g_amsync_last_sent_mhz));
    memset(g_amsync_keepalive_frames, 0, sizeof(g_amsync_keepalive_frames));
    for (int i = 0; i < MAX_CLIENTS; i++)
        g_amsync_last_sent_state[i] = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd       = -1;
        clients[i].band_idx = -1;
    }
}

struct Client *client_assign_fd(int fd, const char *ip_str)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].state == CLIENT_FREE) {
            memset(&clients[i], 0, sizeof(struct Client));
            clients[i].fd       = fd;
            clients[i].state    = CLIENT_WEBSOCKET;
            clients[i].band_idx = -1;
            clients[i].mode     = DEMOD_LSB;
            clients[i].js_mode  = 0;
            clients[i].bw_hz    = 2700;
            clients[i].lo_khz   = -3.0;
            clients[i].hi_khz   = -0.3;
            clients[i].zoom     = 0;
            clients[i].wf_speed      = 1;
            clients[i].wf_slow       = 4;
            clients[i].wf_counter    = 0;
            clients[i].wf_scale      = 1;
            clients[i].last_wf_us    = 0;
            clients[i].demod_prev_re = 0.0f;
            clients[i].demod_prev_im = 0.0f;
            clients[i].agc_gain        = 0.0f;
            clients[i].audio_format    = 0;
            clients[i].wf_format       = 0;
            clients[i].last_sent_mode  = -1;
            clients[i].last_active     = time(NULL);
            clients[i].uu_chseq       = chat_get_chseq();

            clients[i].demod_table    = &g_demod_tables[DT_SSB_NARROW];
            clients[i].demod_mode     = 0;
            clients[i].demod_self_ptr = &clients[i];
            clients[i].carrier_freq   = 8000.0;
            clients[i].header_counter = 1;
            static const int blk_sizes[4] = {40, 128, 256, 512};
            int afmt = g_config.audioformat;
            if (afmt < 0) afmt = 0;
            if (afmt > 3) afmt = 3;
            clients[i].block_size      = blk_sizes[afmt];
            clients[i].mode_changed    = 0;
            clients[i].last_audio_rate = 0;
            clients[i].rate_changed    = 1;
            clients[i].conv_changed    = 1;
            clients[i].demod_filter_dirty = -1;
            clients[i].filter_lo_bin   = 0x7FFFFFFF;
            clients[i].filter_hi_bin   = 0x7FFFFFFF;
            memset(clients[i].pred_h, 0, sizeof(clients[i].pred_h));
            memset(clients[i].pred_x, 0, sizeof(clients[i].pred_x));
            clients[i].pred_accum      = 0;
            g_amsync_last_sent_mhz[i] = 0;
            g_amsync_last_sent_state[i] = -1;
            g_amsync_keepalive_frames[i] = 0;
            if (ip_str)
                snprintf(clients[i].description, sizeof(clients[i].description),
                         "%s", ip_str);
            num_clients++;
            return &clients[i];
        }
    }
    return NULL;
}

void client_close(struct Client *c)
{
    int idx = (int)(c - clients);
    if (idx >= 0 && idx < MAX_CLIENTS) {
        g_amsync_last_sent_mhz[idx] = 0;
        g_amsync_last_sent_state[idx] = -1;
        g_amsync_keepalive_frames[idx] = 0;
    }
    if (c->band_idx >= 0 && c->band_idx < MAX_BANDS)
        bands[c->band_idx].audio_clients--;
    c->uu_chseq = chat_get_pending_chseq();
    c->fd       = -1;
    c->state    = CLIENT_FREE;
    c->band_idx = -1;
    num_clients--;
    if (num_clients < 0) num_clients = 0;
}

int client_ws_send_binary(struct Client *c, const uint8_t *data, size_t len)
{
    if (c->state != CLIENT_WEBSOCKET || c->fd < 0) return -1;
    uint8_t hdr[10];
    int hdrlen;
    hdr[0] = 0x82;
    if (len <= 125) {
        hdr[1] = (uint8_t)len;
        hdrlen = 2;
    } else if (len <= 65535) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len & 0xFF);
        hdrlen = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++)
            hdr[2+i] = (uint8_t)((len >> (56 - 8*i)) & 0xFF);
        hdrlen = 10;
    }
    int avail = (int)(sizeof(c->outbuf) - c->outbuf_len);
    if (avail < hdrlen + (int)len) return -1;
    memcpy(c->outbuf + c->outbuf_len, hdr,  hdrlen);
    c->outbuf_len += hdrlen;
    memcpy(c->outbuf + c->outbuf_len, data, len);
    c->outbuf_len += (int)len;
    return 0;
}

int client_ws_send_text(struct Client *c, const char *text)
{
    size_t len = strlen(text);
    uint8_t hdr[4];
    int hdrlen;
    hdr[0] = 0x81;
    if (len <= 125) { hdr[1] = (uint8_t)len; hdrlen = 2; }
    else { hdr[1] = 126; hdr[2] = (uint8_t)(len >> 8); hdr[3] = (uint8_t)(len & 0xFF); hdrlen = 4; }
    int avail = (int)(sizeof(c->outbuf) - c->outbuf_len);
    if (avail < hdrlen + (int)len) return -1;
    memcpy(c->outbuf + c->outbuf_len, hdr,  hdrlen);
    c->outbuf_len += hdrlen;
    memcpy(c->outbuf + c->outbuf_len, text, len);
    c->outbuf_len += (int)len;
    return 0;
}

void client_flush_outbuf(struct Client *c)
{
    if (c->outbuf_len <= c->outbuf_sent) {
        c->outbuf_len  = 0;
        c->outbuf_sent = 0;
        return;
    }
    int to_send = c->outbuf_len - c->outbuf_sent;
    ssize_t n   = write(c->fd, c->outbuf + c->outbuf_sent, (size_t)to_send);
    if (n > 0) {
        c->outbuf_sent += (int)n;
        if (c->outbuf_sent >= c->outbuf_len) {
            c->outbuf_len  = 0;
            c->outbuf_sent = 0;
            c->write_errors = 0;
        }
    } else if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            c->write_errors++;
            if (c->write_errors > 1000)
                client_close(c);
        } else {
            client_close(c);
        }
    }
}

static int client_ws_send_binary_retry(struct Client *c, const uint8_t *data, size_t len,
                                       int is_audio_frame)
{
    if (client_ws_send_binary(c, data, len) == 0)
        return 0;

    client_flush_outbuf(c);
    if (client_ws_send_binary(c, data, len) == 0)
        return 0;

    if (is_audio_frame) {
        c->write_errors++;
        c->audio_drop_frames++;
        time_t now = time(NULL);
        if (now - c->last_audio_drop_log >= 2) {
            c->last_audio_drop_log = now;
            log_printf("fd=%d -%s-: audio frame drops=%u write_errors=%d outbuf=%d/%zu\n",
                       c->fd, c->description,
                       c->audio_drop_frames, c->write_errors,
                       c->outbuf_len - c->outbuf_sent,
                       sizeof(c->outbuf));
        }
    }
    return -1;
}

static void append_amsync_status_tag(uint8_t *frame, int *fpos, const struct Client *c)
{
    if (!frame || !fpos || !c || c->demod_mode != DEMOD_AMSYNC)
        return;
    long long v = c->amsync_truefreq_mhz;
    if (v < 0)
        v = 0;
    if (v > 0x0FFFFFFFFFFFLL)
        v = 0x0FFFFFFFFFFFLL;
    int status = c->amsync_state;
    if (status < 0) status = AMSYNC_UNLOCKED;
    if (status > 15) status = 15;

    frame[(*fpos)++] = 0x85;
    frame[(*fpos)++] = (uint8_t)((status << 4) | ((v >> 40) & 0x0F));
    frame[(*fpos)++] = (uint8_t)((v >> 32) & 0xFF);
    frame[(*fpos)++] = (uint8_t)((v >> 24) & 0xFF);
    frame[(*fpos)++] = (uint8_t)((v >> 16) & 0xFF);
    frame[(*fpos)++] = (uint8_t)((v >> 8) & 0xFF);
    frame[(*fpos)++] = (uint8_t)(v & 0xFF);
}

static int client_ws_send_control(struct Client *c, uint8_t opcode,
                                  const uint8_t *payload, int plen)
{
    if (!c || c->fd < 0 || plen < 0 || plen > 125)
        return -1;

    uint8_t frame[2 + 125];
    int n = 0;
    frame[n++] = (uint8_t)(0x80 | (opcode & 0x0F));
    frame[n++] = (uint8_t)plen;
    if (plen > 0 && payload)
        memcpy(frame + n, payload, (size_t)plen), n += plen;

    int off = 0;
    int eagain_tries = 0;
    const int eagain_try_max = 50;
    const int eagain_poll_ms = 2;
    while (off < n) {
        ssize_t wr = write(c->fd, frame + off, (size_t)(n - off));
        if (wr > 0) {
            off += (int)wr;
            eagain_tries = 0;
            continue;
        }
        if (wr < 0 && errno == EINTR)
            continue;
        if (wr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (eagain_tries >= eagain_try_max)
                return -1;
            struct pollfd pfd;
            pfd.fd = c->fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, eagain_poll_ms);
            if (pr > 0)
                continue;
            eagain_tries++;
            continue;
        }
        return -1;
    }
    return 0;
}

static int parse_ws_frame(struct Client *c, const uint8_t *buf, int len)
{
    if (len < 2) return 0;
    int op   = (buf[0] & 0x0F);
    int mask = (buf[1] & 0x80) != 0;
    int plen = (buf[1] & 0x7F);
    int hdrlen = 2;
    if (plen == 126) { if (len < 4) return 0; plen = (buf[2] << 8) | buf[3]; hdrlen = 4; }
    else if (plen == 127) { if (len < 10) return 0; plen = (int)((buf[2]<<24)|(buf[3]<<16)|(buf[4]<<8)|buf[5]); hdrlen = 10; }
    if (mask) hdrlen += 4;
    if (len < hdrlen + plen) return 0;
    uint8_t payload[CLIENT_BUF_SIZE];
    if (plen > (int)sizeof(payload) - 1) plen = (int)sizeof(payload) - 1;
    memcpy(payload, buf + hdrlen, (size_t)plen);
    if (mask) { const uint8_t *key = buf + hdrlen - 4; for (int i = 0; i < plen; i++) payload[i] ^= key[i & 3]; }
    payload[plen] = '\0';
    switch (op) {
        case WS_OP_TEXT: client_parse_command(c, (char *)payload, plen); break;
        case WS_OP_CLOSE: client_close(c); break;
        case WS_OP_PING:

            if (client_ws_send_control(c, WS_OP_PONG, payload, plen) != 0)
                client_close(c);
            break;
        case WS_OP_PONG:
            break;
        default: break;
    }
    return hdrlen + plen;
}

void client_ws_recv(struct Client *c, const uint8_t *buf, int len)
{
    c->last_active = time(NULL);
    int avail = (int)(sizeof(c->inbuf) - c->inbuf_len);
    if (len > avail) len = avail;
    if (len <= 0) {
        client_close(c);
        return;
    }
    memcpy(c->inbuf + c->inbuf_len, buf, (size_t)len);
    c->inbuf_len += len;
    int pos = 0;
    while (pos < c->inbuf_len) {
        int consumed = parse_ws_frame(c, c->inbuf + pos, c->inbuf_len - pos);
        if (consumed <= 0) break;
        pos += consumed;
    }
    if (pos > 0) {
        memmove(c->inbuf, c->inbuf + pos, (size_t)(c->inbuf_len - pos));
        c->inbuf_len -= pos;
    }
}

static int qs_get_int(const char *s, const char *key)
{
    size_t klen = strlen(key);
    const char *p = strchr(s, '?');
    if (!p) return -1;
    for (p++; p && *p > 32; ) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
            return (int)strtol(p + klen + 1, NULL, 10);
        p = strchr(p, '&'); if (!p) return -1; p++;
    }
    return -1;
}
static int qs_get_int_start(const char *s, const char *key)
{
    size_t klen = strlen(key);
    const char *p = strchr(s, '?');
    if (!p) return (int)0x80000001;
    for (p++; p && *p > 32; ) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
            return (int)strtol(p + klen + 1, NULL, 10);
        p = strchr(p, '&'); if (!p) return (int)0x80000001; p++;
    }
    return (int)0x80000001;
}
static double qs_get_double(const char *s, const char *key)
{
    size_t klen = strlen(key);
    const char *p = strchr(s, '?');
    if (!p) return 0.0/0.0;
    for (p++; p && *p > 32; ) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
            return strtod(p + klen + 1, NULL);
        p = strchr(p, '&'); if (!p) return 0.0/0.0; p++;
    }
    return 0.0/0.0;
}
static int qs_get_str(const char *s, const char *key, char *buf, int buflen)
{
    size_t klen = strlen(key);
    const char *p = strchr(s, '?');
    if (!p) return -1;
    for (p++; p && *p > 32; ) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *src = p + klen + 1;
            char *dst = buf; char *end = buf + buflen - 1;
            while (*src > 32 && *src != '&' && dst < end) {
                if (*src == '%' && src[1] && src[2]) {
                    char hex[3] = { src[1], src[2], 0 };
                    *dst++ = (char)strtol(hex, NULL, 16); src += 3;
                } else if (*src == '+') { *dst++ = ' '; src++; }
                else { *dst++ = *src++; }
            }
            *dst = '\0'; return (int)(dst - buf);
        }
        p = strchr(p, '&'); if (!p) return -1; p++;
    }
    return -1;
}

static void client_reset_audio_state_on_band_change(struct Client *c)
{
    c->demod_filter_dirty = -1;
    c->filter_lo_bin = 0x7FFFFFFF;
    c->filter_hi_bin = 0x7FFFFFFF;
    c->last_audio_rate = 0;
    c->audio_accum_pos = 0;
}

static void client_update_tune_bin_from_freq(struct Client *c, const struct BandState *b)
{
    if (!c || !b || b->fftlen <= 0 || b->samplerate <= 0) return;
    double v = (c->tune_khz - b->centerfreq_khz) / ((double)b->samplerate / 1000.0);
    double tune_norm = 0.0;
    if (v >= -0.5) {
        tune_norm = (v <= 0.5) ? (v + 0.5) : 1.0;
    }
    int tune_bin = (int)((double)b->fftlen * tune_norm + 0.5);
    if (tune_bin >= b->fftlen) tune_bin = b->fftlen - 1;
    if (tune_bin < 0) tune_bin = 0;
    c->tune_norm = tune_norm;
    c->tune_bin = tune_bin;
    c->last_tune_bin = tune_bin;
}

static void cmd_param(struct Client *c, const char *qs)
{
    char namebuf[32];
    if (qs_get_str(qs, "name", namebuf, sizeof(namebuf)) > 1) {
        char clean[32]; int ci = 0;
        for (int i = 0; namebuf[i] && ci < 31; i++)
            if ((unsigned char)namebuf[i] >= 32) clean[ci++] = namebuf[i];
        clean[ci] = '\0';
        if (strcmp(c->username, clean) != 0) {
            snprintf(c->username, sizeof(c->username), "%s", clean);
            c->uu_chseq = chat_get_pending_chseq();
        }
    }
    double f  = qs_get_double(qs, "f");
    double lo = qs_get_double(qs, "lo");
    double hi = qs_get_double(qs, "hi");
    int band  = qs_get_int(qs, "band");
    int mode  = qs_get_int(qs, "mode");
    int sq    = qs_get_int(qs, "squelch");
    int an    = qs_get_int(qs, "autonotch");
    int mute  = qs_get_int(qs, "mute");
    int band_changed = 0;

    if (sq >= 0)   { c->squelch = sq; c->demod_squelch = sq; }
    if (an >= 0)   { c->autonotch = an; c->demod_autonotch = an; }
    if (mute >= 0) { c->mute = mute; c->demod_mute = mute; }

    if (band >= 0 && band < config.num_bands && band != c->band_idx) {
        if (c->band_idx >= 0 && c->band_idx < config.num_bands)
            bands[c->band_idx].audio_clients--;
        c->band_idx = band; c->band_idx_req = band;
        c->band_ptr = &bands[band];
        band_changed = 1;
        bands[band].audio_clients++;
        log_printf("fd=%d: /~~param switched band to %d\n", c->fd, band);
        client_reset_audio_state_on_band_change(c);
        c->uu_chseq = chat_get_pending_chseq();
    }
    {
        struct BandState *bp = c->band_ptr;
        struct DemodTable *dt = c->demod_table;
        if (bp && dt && bp->fftlen_real > 0)
            c->carrier_freq = (double)dt->half_size
                            * (2.0 * (double)bp->samplerate / (double)bp->fftlen_real);
    }
    if (!isnan(f)) {
        c->tune_khz = f;
        if (c->band_idx >= 0 && c->band_idx < config.num_bands)
            client_update_tune_bin_from_freq(c, &bands[c->band_idx]);
        c->uu_chseq = chat_get_pending_chseq();
    }
    if (!isnan(lo) && !isnan(hi)) {
        c->lo_khz = lo; c->hi_khz = hi;
        c->bw_hz = (int)((hi - lo) * 1000.0);
        if (c->bw_hz < 0) c->bw_hz = -c->bw_hz;
        if (c->js_mode == 0) {
            if (c->hi_khz > 0.0 && c->lo_khz >= 0.0) c->mode = DEMOD_USB;
            else c->mode = DEMOD_LSB;
        }
        if (c->band_ptr) {
            compute_passband_filter(c, (struct BandState *)c->band_ptr);
            c->demod_filter_dirty = 0;
        } else {
            c->demod_filter_dirty = -1;
        }
    }
    if (mode >= 0 || band_changed) {
        if (mode >= 0)
            c->js_mode = mode;
        int eff_mode = (mode >= 0) ? mode : c->js_mode;
        dsp_apply_mode_selection(c, (struct BandState *)c->band_ptr, eff_mode);
        if (mode >= 0)
            c->mode = c->demod_mode;
    }
    double gain = qs_get_double(qs, "gain");
    if (!isnan(gain)) {
        if (gain >= 9999.0) { c->agc_mode = 0; c->demod_agc_mode = 0; }
        else { c->agc_mode = 1; c->demod_agc_mode = 1;
               c->gain_linear = powf(10.0f, (float)((gain - 120.0) / 20.0)); }
    }
    double ppm_d = qs_get_double(qs, "ppm");
    if (!isnan(ppm_d)) { c->ppm = (float)ppm_d; c->demod_ppm = (float)ppm_d; }
    double ppml_d = qs_get_double(qs, "ppml");
    if (!isnan(ppml_d)) { c->ppml = (float)ppml_d; c->demod_ppml = (float)ppml_d; }
}

static void cmd_waterparam(struct Client *c, const char *qs)
{
    int band  = qs_get_int(qs, "band");
    int zoom  = qs_get_int(qs, "zoom");
    int start = qs_get_int_start(qs, "start");
    int speed = qs_get_int(qs, "speed");
    int slow  = qs_get_int(qs, "slow");
    int width = qs_get_int(qs, "width");
    int scale = qs_get_int(qs, "scale");

    if (band >= 0 && band < config.num_bands && band != c->band_idx) {
        if (c->band_idx >= 0 && c->band_idx < config.num_bands) bands[c->band_idx].audio_clients--;
        c->band_idx = band; c->band_idx_req = band; c->band_ptr = &bands[band];
        bands[band].audio_clients++;
        log_printf("fd=%d: /~~waterparam switched band to %d\n", c->fd, band);
        client_reset_audio_state_on_band_change(c);
        c->uu_chseq = chat_get_pending_chseq();
        c->audio_seqno = 0;
        c->wf_prev_valid = 0;
        memset(c->wf_prev_row, 8, sizeof(c->wf_prev_row));
        memset(c->wf_prev_n, 0, sizeof(c->wf_prev_n));
    }
    if (zoom >= 0 && zoom != c->zoom) {
        c->zoom = zoom;
        c->audio_seqno = 0; c->wf_prev_valid = 0;
        memset(c->wf_prev_row, 8, sizeof(c->wf_prev_row));
        memset(c->wf_prev_n, 0, sizeof(c->wf_prev_n));
    }
    if (start != (int)0x80000001 && start != c->wf_offset) {
        c->wf_offset = start;
        c->audio_seqno = 0; c->wf_prev_valid = 0;
        memset(c->wf_prev_row, 8, sizeof(c->wf_prev_row));
        memset(c->wf_prev_n, 0, sizeof(c->wf_prev_n));
    }
    if (width > 0 && width != c->wf_width) {
        c->wf_width = width; c->audio_seqno = 0; c->wf_prev_valid = 0;
        memset(c->wf_prev_row, 8, sizeof(c->wf_prev_row));
        memset(c->wf_prev_n, 0, sizeof(c->wf_prev_n));
    } else if (width > 0) { c->wf_width = width; }

    if (speed > 0) {
        int derived = 4 / speed;
        c->wf_slow = (derived > 0) ? derived : 1;
    }
    if (slow > 0) {
        c->wf_slow = slow;
    }

    if (scale >= 0) {
        c->wf_scale = scale;
    }
}

void client_parse_command(struct Client *c, const char *cmd, int len)
{
    (void)len;
    c->last_active = time(NULL);
    if (strncmp(cmd, "GET ", 4) == 0) cmd += 4;
    if      (strncmp(cmd, "/~~param",     8)  == 0) cmd_param(c, cmd);
    else if (strncmp(cmd, "/~~waterparam",13) == 0) cmd_waterparam(c, cmd);
}

static const uint8_t mulaw_log2[256] = {
    0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
    5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
};

static uint8_t pcm16_to_mulaw(int16_t sample)
{
    int v1 = ((~(int)sample) >> 8) & 0x80;
    int a  = (int)sample;
    if (!v1) a = -a;
    if (a > 32635) a = 32635;
    if (a <= 255) return (uint8_t)(v1 ^ 0x55 ^ ((unsigned)a >> 4));

    int exp = mulaw_log2[(unsigned)a >> 8] + 1;
    return (uint8_t)(v1 ^ 0x55 ^ (((a >> (exp + 3)) & 0xF) | (unsigned)(16 * exp)));
}

static int send_audio_compressed3(struct Client *c, struct BandState *b,
                                  float *audio_f, float scale, int smeter_raw)
{
    int cidx = (int)(c - clients);
    if (cidx < 0 || cidx >= MAX_CLIENTS) cidx = 0;

    static const int blk_sizes[4] = {40, 128, 256, 512};
    int afmt = g_config.audioformat;
    if (afmt < 0) afmt = 0;
    if (afmt > 3) afmt = 3;
    int target_bs = blk_sizes[afmt];
    if (c->block_size != target_bs) {
        c->block_size = target_bs;
        c->rate_changed = 1;
    }

    uint8_t frame[1024];
    int fpos = 0;

    c->header_counter--;
    if (c->header_counter <= 0) {
        int smeter_enc = smeter_raw / 10;
        frame[fpos++] = (uint8_t)(((smeter_enc >> 8) & 0x0F) | 0xF0);
        frame[fpos++] = (uint8_t)(smeter_enc & 0xFF);
        c->header_counter = 8;
    }

    {
        int dt_audiolen = c->demod_table ? c->demod_table->audiolen : 128;

        long long sr = (long long)b->samplerate;
        if (b->noniq) sr *= 2;
        if (b->device_type == DEVTYPE_STDIN) {
            int measured = stdin_get_measured_sps();
            if (measured > 1000000)
                sr = measured;
        }
        int audio_rate = (b->fftlen_real > 0)
            ? (int)(2LL * (long long)dt_audiolen * sr / b->fftlen_real)
            : 8000;
        if (audio_rate < 1) audio_rate = 8000;
        if (audio_rate > 65535) audio_rate = 65535;
        if (audio_rate != c->last_audio_rate) {
            c->last_audio_rate = audio_rate;
            c->carrier_freq = (double)audio_rate;
            frame[fpos++] = 0x81;
            frame[fpos++] = (uint8_t)((audio_rate >> 8) & 0xFF);
            frame[fpos++] = (uint8_t)(audio_rate & 0xFF);
        }
    }

    if (c->rate_changed) {
        c->rate_changed = 0;
        int bs = c->block_size;
        frame[fpos++] = 0x82;
        frame[fpos++] = (uint8_t)((bs >> 8) & 0xFF);
        frame[fpos++] = (uint8_t)(bs & 0xFF);
    }

    if (c->conv_changed) {
        c->conv_changed = 0;
        frame[fpos++] = 0x83;
        frame[fpos++] = (uint8_t)c->conv_type;
    }

    if (c->demod_mode == DEMOD_AMSYNC) {
        int cidx2 = (int)(c - clients);
        if (cidx2 < 0 || cidx2 >= MAX_CLIENTS) cidx2 = 0;

        int must_send = (fpos <= 2 || c->mode_changed || c->header_counter == 8);
        int status = c->amsync_state;
        long long mhz = c->amsync_truefreq_mhz;
        long long delta_mhz = llabs(mhz - g_amsync_last_sent_mhz[cidx2]);
        if (!must_send) {
            if (status != g_amsync_last_sent_state[cidx2]) {
                must_send = 1;
            } else if (delta_mhz >= 1000) {
                must_send = 1;
            } else {
                g_amsync_keepalive_frames[cidx2]++;
                if (g_amsync_keepalive_frames[cidx2] >= 64)
                    must_send = 1;
            }
        }
        if (must_send) {
            append_amsync_status_tag(frame, &fpos, c);
            g_amsync_last_sent_mhz[cidx2] = mhz;
            g_amsync_last_sent_state[cidx2] = status;
            g_amsync_keepalive_frames[cidx2] = 0;
            c->mode_changed = 0;
        }
    } else if (c->mode_changed) {
        c->mode_changed = 0;
    }

    if (c->demod_mute) {
        memset(c->pred_h, 0, sizeof(c->pred_h));
        memset(c->pred_x, 0, sizeof(c->pred_x));
        c->pred_accum = 0;
        frame[fpos++] = 0x84;
        c->compress_ema = 0.0f;
        g_audio_mute_silence_count[cidx]++;
        if (client_ws_send_binary_retry(c, frame, (size_t)fpos, 1) == 0) {
            stats_add_bytes(0, (long long)fpos);
            client_flush_outbuf(c);
            return fpos;
        }
        return 0;
    }

    int residuals[128];
    int pred_sum = 0;
    for (int j = 0; j < 20; j++)
        pred_sum += c->pred_h[j] * c->pred_x[j];

    int blk = c->block_size;
    int neg_half = -(blk / 2);
    int blk_shift = blk << 16;
    int adpcm_shift = (c->conv_type & 0x10) ? 12 : 14;
    double sum_abs = 0.0;
    double sum_pred = 0.0;
    double sum_raw = 0.0;
    int overflow = 0;

    for (int i = 0; i < 128; i++) {
        int old_accum = c->pred_accum;
        int prediction = pred_sum / 4096;

        int v27 = (int)(audio_f[i] * scale - (float)(old_accum >> 4));
        int v28 = v27 * v27;
        int error = v27 - prediction;

        sum_raw += (double)v28;
        sum_pred += (double)(error * error);

        int wrapped = (neg_half + blk_shift + error) / blk - 0x10000;
        int aw = wrapped < 0 ? -wrapped : wrapped;
        int mask = -1;
        if (aw > 16) mask = (aw <= 32) ? -2 : -4;
        wrapped = mask & wrapped;

        residuals[i] = wrapped;
        int abs_res = wrapped ^ (wrapped >> 31);
        sum_abs += (double)abs_res;

        if (abs_res > 1000) { overflow = 1; break; }

        int scaled = blk * wrapped + (blk / 2);
        int v40 = scaled >> 4;

        int partial = 0;
        for (int j = 19; j >= 1; j--) {
            int xprev  = c->pred_x[j - 1];
            int hj     = c->pred_h[j];
            int xj     = c->pred_x[j];
            int new_hj = hj + ((v40 * xj) >> adpcm_shift) - (hj >> 7);
            c->pred_h[j] = new_hj;
            c->pred_x[j] = xprev;
            partial += xprev * new_hj;
        }

        int old_x0 = c->pred_x[0];
        int v43    = old_x0 * v40;
        int v45    = prediction + scaled;
        int h0     = c->pred_h[0];
        int new_h0 = h0 + (v43 >> adpcm_shift) - (h0 >> 7);

        c->pred_x[0] = v45;
        c->pred_h[0] = new_h0;
        pred_sum = v45 * new_h0 + partial;

        if (c->conv_type & 0x10)
            c->pred_accum = 0;
        else
            c->pred_accum = old_accum + ((16 * v45) >> 3);
    }

    {
        int sq_open;
        if (c->demod_mode == DEMOD_FM)
            sq_open = c->demod_fm_sq;
        else
            sq_open = (sum_raw > 1e-10) ? ((sum_pred / sum_raw) > (double)c->squelch_threshold) : 0;

        if (!sq_open) {
            int sc = c->squelch_counter;
            if (sc <= 999) c->squelch_counter = 0;
            else c->squelch_counter = sc - 1;
        } else if (c->squelch_counter <= 30) {
            c->squelch_counter++;
        } else {
            c->squelch_counter = 1002;
        }
    }

    if (c->demod_squelch && c->squelch_counter > 899) {
        memset(c->pred_h, 0, sizeof(c->pred_h));
        memset(c->pred_x, 0, sizeof(c->pred_x));
        c->pred_accum = 0;
        frame[fpos++] = 0x84;
        c->compress_ema = 0.0f;
        g_audio_squelch_silence_count[cidx]++;
        if (client_ws_send_binary_retry(c, frame, (size_t)fpos, 1) == 0) {
            stats_add_bytes(0, (long long)fpos);
            client_flush_outbuf(c);
            return fpos;
        }
        return 0;
    }

    if (overflow) {
        memset(c->pred_h, 0, sizeof(c->pred_h));
        memset(c->pred_x, 0, sizeof(c->pred_x));
        c->pred_accum = 0;
        g_audio_overflow_count[cidx]++;
        frame[fpos++] = 0x80;
        for (int i = 0; i < 128; i++) {
            int sample = (int)(audio_f[i] * scale);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            frame[fpos++] = pcm16_to_mulaw((int16_t)sample);
        }

        c->compress_ema = (float)((8.0 - (double)c->compress_ema) * 0.005
                                  + (double)c->compress_ema);

        if (client_ws_send_binary_retry(c, frame, (size_t)fpos, 1) == 0) {
            stats_add_bytes(0, (long long)fpos);
            client_flush_outbuf(c);
            return fpos;
        }
        return 0;
    }

    float avg_residual = sum_abs * 0.0078125f;
    int v53, v54, v110, v114;

    if (avg_residual >= 3.81f) {
        if (avg_residual < 8.0f) {
            v114 = 12; v53 = 4; v110 = 2; v54 = 3;
        } else if (avg_residual < 16.3f) {
            v114 = 11; v53 = 8; v110 = 3; v54 = 4;
        } else {
            v114 = 10; v53 = 16; v110 = 4; v54 = 5;
        }
    } else {
        if (avg_residual < 1.65f) {
            v114 = 14; v53 = 1; v110 = 0; v54 = 1;
        } else {
            v114 = 13; v53 = 2; v110 = 1; v54 = 2;
        }
    }

    uint8_t *bstart = frame + fpos;
    int v122[8] = {999, 999, 8, 4, 2, 1, 99, 99};
    int bitpos;

    if (c->quant_mode == v54) {
        *bstart = 0;
        bitpos = 1;
    } else {
        *bstart = (uint8_t)((16 * (6 - v54)) ^ 0x80);
        bitpos = 4;
    }

    uint8_t *bp = bstart;
    int encode_ok = 1;

    for (int i = 0; i < 128 && encode_ok; i++) {
        int val = residuals[i];
        int sign = (unsigned int)val >> 31;
        int abs_val = val ^ (val >> 31);
        int quotient = abs_val / v53;

        if (quotient > 255) { encode_ok = 0; break; }

        int prefix_len, prefix_val;
        if (quotient >= v114) {
            prefix_len = 23 - v54;
            prefix_val = quotient;
        } else {
            prefix_len = quotient + 1;
            prefix_val = 1;
        }

        int mantissa = abs_val & (v53 - 1);
        int mant_bits = v54;

        if (quotient >= v122[v54]) { mantissa >>= 1; mant_bits = v110; }
        if (quotient >= v122[v110]) { mantissa >>= 1; mant_bits--; }
        if (mant_bits <= 0) mant_bits = 1;

        int code = (prefix_val << mant_bits) | (2 * mantissa + sign);
        int code_len = mant_bits + prefix_len;
        int shift_amt = 32 - code_len - bitpos;
        int shifted = code << shift_amt;

        *bp |= (uint8_t)(shifted >> 24);
        bitpos += code_len;
        if (bitpos > 7) {
            unsigned int extra = bitpos - 8;
            uint8_t *end = bp + 1 + (extra >> 3);
            while (bp < end) {
                shifted <<= 8;
                *(++bp) = (uint8_t)(shifted >> 24);
            }
            bitpos = extra - 8 * (extra >> 3);
        }
    }

    if (!encode_ok) {
        fpos = (int)(bstart - frame);
        memset(c->pred_h, 0, sizeof(c->pred_h));
        memset(c->pred_x, 0, sizeof(c->pred_x));
        c->pred_accum = 0;
        frame[fpos++] = 0x80;
        for (int i = 0; i < 128; i++) {
            int sample = (int)(audio_f[i] * scale);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            frame[fpos++] = pcm16_to_mulaw((int16_t)sample);
        }
    } else {
        int compressed_len = (int)(bp - bstart) + (bitpos > 0 ? 1 : 0);
        fpos = (int)(bstart - frame) + compressed_len;
    }

    c->quant_mode = v54;
    c->compress_ema = (float)(((double)fpos * 0.0625
                               - (double)c->compress_ema) * 0.005
                              + (double)c->compress_ema);

    if (client_ws_send_binary_retry(c, frame, (size_t)fpos, 1) == 0) {
        stats_add_bytes(0, (long long)fpos);
        client_flush_outbuf(c);
        return fpos;
    }
    return 0;
}

void client_dispatch_audio(struct BandState *b, int phase_flag)
{

    b->dispatch_counter++;
    int wf_slow_min = 1;
    {
        int nusers = 0;
        for (int j = 0; j < MAX_CLIENTS; j++) {
            const struct Client *cj = &clients[j];
            if (cj->state == CLIENT_WEBSOCKET && cj->audio_format > 0)
                nusers++;
        }

        unsigned int cnt = b->dispatch_counter;
        int pass1 = (nusers <= g_config.slowdownusers)  || ((cnt & 3) == 0);
        int pass2 = (nusers <= g_config.slowdownusers2) || ((cnt & 7) == 0);
        wf_slow_min = (pass1 && pass2) ? 1 : 0;
    }
    b->wf_slow_min = wf_slow_min;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct Client *c = &clients[i];
        if (c->state != CLIENT_WEBSOCKET) continue;
        if (c->band_idx < 0 || c->band_idx >= MAX_BANDS) continue;
        if (&bands[c->band_idx] != b) continue;

        int N = b->fftlen;
        int sr = b->samplerate;

        if (c->tune_khz == 0.0)
            c->tune_khz = b->centerfreq_khz;

        if (phase_flag) {
            double v9 = (c->tune_khz - b->centerfreq_khz)
                        / ((double)sr / 1000.0);
            double tn = 0.0;
            if (v9 >= -0.5) {
                tn = (v9 <= 0.5) ? (v9 + 0.5) : 1.0;
            }
            c->tune_norm = tn;
            int tb = (int)((double)N * tn + 0.5);
            if (tb >= N) tb = N - 1;
            if (c->tune_bin != tb)
                c->tune_bin = tb;
            c->last_tune_bin = tb;
        }
        int tune_bin = c->last_tune_bin;

        c->band_ptr = b;

        if (c->audio_format == 36) {

            if (b->wf_slow_min == 0)
                goto next_client;

            if (b->wf_pyramid_valid &&
                c->wf_last_pyramid_seq == b->wf_pyramid_seq)
                goto next_client;

            if (c->wf_slow > 1 &&
                (b->wf_pyramid_seq % (unsigned int)c->wf_slow) != 0)
                goto next_client;

            c->wf_last_pyramid_seq = b->wf_pyramid_seq;

            if (N <= 0) goto next_client;

            int ww = (c->wf_width > 0 && c->wf_width <= 1024) ? c->wf_width : 1024;

            if (c->audio_seqno == 0) {
                uint32_t sv = (uint32_t)c->wf_offset;
                uint8_t pf[9];
                pf[0] = 0xFF; pf[1] = 0x01;
                pf[2] = (uint8_t)(c->zoom & 0x7F);
                pf[3] = (uint8_t)(sv & 0xFF);
                pf[4] = (uint8_t)((sv >> 8) & 0xFF);
                pf[5] = (uint8_t)((sv >> 16) & 0xFF);
                pf[6] = (uint8_t)((sv >> 24) & 0xFF);
                pf[7] = 0; pf[8] = 0;
                client_ws_send_binary(c, pf, 9);
                uint8_t wf[4];
                wf[0] = 0xFF; wf[1] = 0x02;
                wf[2] = (uint8_t)(ww & 0xFF);
                wf[3] = (uint8_t)((ww >> 8) & 0xFF);
                client_ws_send_binary(c, wf, 4);
                memset(c->wf_prev_row, 8, sizeof(c->wf_prev_row));
                memset(c->wf_prev_n, 0, sizeof(c->wf_prev_n));
                c->wf_prev_valid = 0;
            }
            c->audio_seqno++;

            uint8_t wf_raw[1024];
            memset(wf_raw, 0, sizeof(wf_raw));
            if (b->wf_pyramid_valid) {
                int mz = b->maxzoom;
                int z  = c->zoom;
                if (z >= 0 && z <= mz && b->wf_zoom_power[z]) {
                    int total_px = 1024 << z;
                    int bin_offset = c->wf_offset >> (mz - z);
                    float gain_adj = b->gain_adj;
                    int go = (int)(gain_adj * (128.0f / 3.0f)) - 17106;
                    const float *pwr = b->wf_zoom_power[z];
                    for (int si = 0; si < ww; si++) {
                        int px = si + bin_offset;
                        if (px < 0 || px >= total_px) continue;
                        float pw = pwr[px];
                        if (pw <= 0.0f) continue;
                        union { float f; uint32_t u; } pu;
                        pu.f = pw;
                        int idx = ((int)(int16_t)(pu.u >> 16) + go) / 13;
                        if (idx < 0) idx = 0;
                        if (idx > 255) idx = 255;
                        wf_raw[si] = (uint8_t)idx;
                    }
                }
            }

            if (c->wf_scale == 2) {
                for (int si = 0; si < ww; si++)
                    wf_raw[si] = wf_scale2_remap[wf_raw[si]];
            } else if (c->wf_scale == 3) {
                for (int si = 0; si < ww; si++)
                    wf_raw[si] = wf_scale3_remap[wf_raw[si]];
            }

            if (b->wf_text_pending > 0) {
                const uint8_t *trow = b->wf_text_rows[0];
                int z   = c->zoom;
                int mz  = b->maxzoom;
                int bin_offset = c->wf_offset >> (mz - z);
                for (int si = 0; si < ww; si++) {

                    int zpx = si + bin_offset;
                    int z0col = zpx >> z;
                    if (z0col < 0 || z0col >= 1024) continue;
                    uint8_t tv = trow[z0col];
                    if (tv == 0) continue;
                    if (tv > 1)
                        wf_raw[si] = 0xFF;
                    else
                        wf_raw[si] >>= 1;
                }
            }

            uint8_t bitstream[3072];
            int blen;
            if (c->wf_format == 10) {
                blen = waterfall_encode_row_f10(
                    wf_raw, c->wf_prev_row, c->wf_prev_n, ww, bitstream);
            } else {
                blen = waterfall_encode_row_f9(wf_raw, c->wf_prev_row, ww, bitstream);
            }
            if (blen <= 0) goto next_client;
            if (bitstream[0] == 0xFF) {
                uint8_t *esc = (uint8_t *)malloc((size_t)(blen + 1));
                if (!esc) goto next_client;
                esc[0] = 0xFF;
                memcpy(esc + 1, bitstream, (size_t)blen);
                client_ws_send_binary(c, esc, (size_t)(blen + 1));
                free(esc);
            } else {
                client_ws_send_binary(c, bitstream, (size_t)blen);
            }
            stats_add_bytes(1, (long long)blen);
            client_flush_outbuf(c);

            if (b->wf_text_row_applied) {
                b->wf_text_row_applied = 0;
                if (b->wf_text_pending > 0) {
                    memmove(b->wf_text_rows[0], b->wf_text_rows[1],
                            31 * sizeof(b->wf_text_rows[0]));
                    memset(b->wf_text_rows[31], 0, sizeof(b->wf_text_rows[31]));
                    b->wf_text_pending--;
                }
            }
            b->wf_text_row_applied = 1;

            goto next_client;
        }

        {
            if (c->demod_filter_dirty) {
                compute_passband_filter(c, b);
                c->demod_filter_dirty = 0;
            }

            struct DemodTable *dt = c->demod_table;
            if (!dt) {
                struct DemodTable *bdt = b->band_demod_tables;
                if (!bdt) bdt = g_demod_tables;
                dt = &bdt[DT_SSB_NARROW];
                c->demod_table = dt;
            }

            if (c->demod_mode == DEMOD_AMSYNC) {
                dsp_demod_amsync(b, tune_bin, c->filter_buf, c, dt, phase_flag);
            } else if (dt->demod_fn) {
                dt->demod_fn(b, tune_bin, c->filter_buf, c, dt, phase_flag);
            }

            float *audio_out = dt->output;
            int out_samples  = dt->audiolen;
            if (!audio_out || out_samples <= 0) goto next_client;

            float peak = 0.0f;
            for (int s = 0; s < out_samples; s++) {
                float a = audio_out[s];
                if (a < 0.0f) a = -a;
                if (a > peak) peak = a;
            }
            int cidx = (int)(c - clients);
            if (cidx < 0 || cidx >= MAX_CLIENTS) cidx = 0;
            g_prev_audio_peak[cidx] = peak;

            float carrier_power = 0.0f;
            if (b->spectrum_buf) {
                int spec_bin = (tune_bin ^ (b->fftlen / 2));
                int fftlen2 = b->fftlen2;
                if (fftlen2 > 0 && b->fftlen > 0) {
                    int mapped = fftlen2 * spec_bin / b->fftlen;
                    if (mapped >= 0 && mapped < fftlen2) {
                        carrier_power = b->spectrum_buf[mapped];
                        carrier_power *= carrier_power;
                    }
                }
            }

            float smeter_power = (float)dt->power;
            if (carrier_power > 0.0f) smeter_power *= carrier_power;
            c->smeter_power = smeter_power;

            int smeter_raw = 0;
            if (smeter_power > 1e-30f) {
                double lv = log10((double)smeter_power);

                int sv = (int)(lv * 1000.0 - 5400.0
                               + 100.0 * (double)b->gain_db);
                if (sv < 0) sv = 0;
                if ((unsigned)sv >= 0x8000u) sv = 0;
                smeter_raw = sv;
            }

            float scale;
            if (c->demod_agc_mode) {
                scale = c->gain_linear * 32000.0f;
            } else {
                float new_gain = (float)(1.0 / (dt->sqrt_power + 1e-30));
                float old_gain = c->agc_gain;
                c->agc_gain = new_gain;
                if (new_gain > old_gain) {
                    new_gain = (float)((double)new_gain * 0.01 + (double)old_gain * 0.99);
                    c->agc_gain = new_gain;
                }
                scale = new_gain * 32000.0f;
            }

            {
                int src_pos = 0;
                while (src_pos < out_samples) {
                    int space = 128 - c->audio_accum_pos;
                    int avail = out_samples - src_pos;
                    int n = (avail < space) ? avail : space;
                    memcpy(c->audio_accum + c->audio_accum_pos,
                           audio_out + src_pos, n * sizeof(float));
                    c->audio_accum_pos += n;
                    src_pos += n;
                    if (c->audio_accum_pos >= 128) {
                        send_audio_compressed3(c, b, c->audio_accum, scale, smeter_raw);
                        c->audio_accum_pos = 0;
                    }
                }
            }
            c->audio_seqno++;
        }

next_client:;
    }

    if (g_config.idletimeout > 0) {
        time_t now = time(NULL);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            struct Client *c2 = &clients[i];
            if (c2->state != CLIENT_WEBSOCKET) continue;
            if (now - c2->last_active > g_config.idletimeout) {
                log_printf("fd=%i -%s-: WS client idle timeout (%i s)\n",
                           c2->fd, c2->description, g_config.idletimeout);
                client_close(c2);
            }
        }
    }
}
