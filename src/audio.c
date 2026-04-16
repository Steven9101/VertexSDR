// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#include "band.h"
#include "dsp.h"
#include "common.h"
#include "logging.h"
#include "worker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif

int rtltcp_open_device(struct BandState *b);
int rtltcp_read_samples(struct BandState *b);
int stdin_open_device(struct BandState *b);
int stdin_read_samples(struct BandState *b);
extern void network_flush_ws_clients(void);

#define MAX_ALSA_HANDLES 16
static struct {
    char       dev_name[256];
    snd_pcm_t *handle;
} g_alsa_handles[MAX_ALSA_HANDLES];
static int g_alsa_handle_count = 0;

void audio_reset_runtime_state(void)
{
    memset(g_alsa_handles, 0, sizeof(g_alsa_handles));
    g_alsa_handle_count = 0;
}

int alsa_open_device(struct BandState *b)
{
    int err;

    const char *dev = b->device_name;
    if (dev && dev[0] == '!') {
        const char *sp = strchr(dev, ' ');
        dev = sp ? sp + 1 : dev + 1;
    }
    if (!dev || dev[0] == '\0') {
        fprintf(stderr, "Band %i: empty device name\n", b->band_index);
        return -1;
    }

    if (b->device_type == DEVTYPE_RTLSDR || b->device_type == DEVTYPE_TCPSDR)
        return rtltcp_open_device(b);

    if (b->device_type == DEVTYPE_STDIN)
        return stdin_open_device(b);

    for (int i = 0; i < g_alsa_handle_count; i++) {
        if (strncmp(g_alsa_handles[i].dev_name, dev, 255) == 0) {
            b->alsa_handle = g_alsa_handles[i].handle;
            struct pollfd pfds[100];
            snd_pcm_poll_descriptors(b->alsa_handle, pfds, 100);
            b->fd = pfds[0].fd;
            fprintf(stderr, "Band %i: reusing shared ALSA handle for '%s' "
                    "(channel %i)\n", b->band_index, dev, b->device_ch);
            return 0;
        }
    }

    err = snd_pcm_open(&b->alsa_handle, dev, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (err < 0) {
        fprintf(stderr, "Can't open ALSA sound device: %s (%s).\n",
                dev, snd_strerror(err));
        exit(1);
    }

    snd_pcm_hw_params_t *hwp;
    snd_pcm_hw_params_malloc(&hwp);
    err = snd_pcm_hw_params_any(b->alsa_handle, hwp);
    if (err < 0) goto hw_err;

    err = snd_pcm_hw_params_set_access(b->alsa_handle, hwp, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) goto hw_err;

    err = snd_pcm_hw_params_set_format(b->alsa_handle, hwp, SND_PCM_FORMAT_S16_LE);
    if (err < 0) goto hw_err;

    unsigned int rate = (unsigned int)b->samplerate;
    if (b->noniq) rate *= 2;
    err = snd_pcm_hw_params_set_rate(b->alsa_handle, hwp, rate, 0);
    if (err < 0) {
        fprintf(stderr, "Can't set samplerate for ALSA sound device %s to %i: %s.\n",
                dev, rate, snd_strerror(err));

    }

    unsigned int channels = b->noniq ? 1 : 2;
    err = snd_pcm_hw_params_set_channels(b->alsa_handle, hwp, channels);
    if (err < 0) {
        fprintf(stderr, "Can't set number of channels for ALSA sound device %s to %i: %s.\n",
                dev, channels, snd_strerror(err));

    }

    snd_pcm_uframes_t buf_size = 0x40000;
    err = snd_pcm_hw_params_set_buffer_size_near(b->alsa_handle, hwp, &buf_size);

    b->alsa_buf_frames = (int)(buf_size >> 11);
    if (err < 0) goto hw_err;

    snd_pcm_uframes_t period = 1024;
    int dir = 0;
    err = snd_pcm_hw_params_set_period_size_near(b->alsa_handle, hwp, &period, &dir);
    if (err < 0) goto hw_err;

    err = snd_pcm_hw_params(b->alsa_handle, hwp);
    if (err < 0) goto hw_err;

    snd_pcm_sw_params_t *swp;
    snd_pcm_sw_params_malloc(&swp);
    err = snd_pcm_sw_params_current(b->alsa_handle, swp);
    if (err < 0) goto sw_err;
    err = snd_pcm_sw_params_set_avail_min(b->alsa_handle, swp, 1);
    if (err < 0) goto sw_err;
    err = snd_pcm_sw_params_set_start_threshold(b->alsa_handle, swp, 1);
    if (err < 0) goto sw_err;
    err = snd_pcm_sw_params(b->alsa_handle, swp);
    if (err < 0) goto sw_err;

    {
        int16_t dummy[0x2000 * 2];
        snd_pcm_readi(b->alsa_handle, dummy, 0x2000);
    }

    {
        struct pollfd pfds[100];
        snd_pcm_poll_descriptors(b->alsa_handle, pfds, 100);
        b->fd = pfds[0].fd;
    }

    if (g_alsa_handle_count < MAX_ALSA_HANDLES) {
        strncpy(g_alsa_handles[g_alsa_handle_count].dev_name, dev, 255);
        g_alsa_handles[g_alsa_handle_count].dev_name[255] = '\0';
        g_alsa_handles[g_alsa_handle_count].handle = b->alsa_handle;
        g_alsa_handle_count++;
    }

    snd_pcm_hw_params_free(hwp);
    snd_pcm_sw_params_free(swp);
    return 0;

hw_err:
    fprintf(stderr, "Can't set parameters for ALSA sound device %s: %s.\n",
            dev, snd_strerror(err));
    snd_pcm_hw_params_free(hwp);
    exit(1);

sw_err:
    fprintf(stderr, "Can't set parameters for ALSA sound device %s: %s.\n",
            dev, snd_strerror(err));
    snd_pcm_sw_params_free(swp);
    exit(1);
}

static void rtltcp_cmd(int fd, uint8_t cmd, uint32_t val)
{
    uint8_t pkt[5];
    pkt[0] = cmd;
    pkt[1] = (val >> 24) & 0xFF;
    pkt[2] = (val >> 16) & 0xFF;
    pkt[3] = (val >>  8) & 0xFF;
    pkt[4] = (val      ) & 0xFF;
    (void)write(fd, pkt, 5);
}

int rtltcp_open_device(struct BandState *b)
{

    const char *spec = b->device_name;

    const char *sp = strchr(spec, ' ');
    if (!sp) { fprintf(stderr, "Band %i: bad rtltcp spec '%s'\n", b->band_index, spec); return -1; }
    sp++;

    char host[256];
    int  port = 1234;
    const char *colon = strrchr(sp, ':');
    if (colon) {
        size_t hlen = (size_t)(colon - sp);
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, sp, hlen);
        host[hlen] = '\0';
        port = atoi(colon + 1);
    } else {
        snprintf(host, sizeof(host), "%s", sp);
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        fprintf(stderr, "Band %i: can't resolve '%s'\n", b->band_index, host);
        return -1;
    }

    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); perror("rtltcp socket"); return -1; }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        fprintf(stderr, "Band %i: cannot connect to rtl_tcp at %s:%d: %s\n"
                        "         Make sure rtl_tcp is running: rtl_tcp -a 127.0.0.1\n",
                b->band_index, host, port, strerror(errno));
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);

    uint8_t magic[12];
    int got = 0;
    while (got < 12) {
        int n = (int)read(fd, magic + got, (size_t)(12 - got));
        if (n <= 0) { fprintf(stderr, "Band %i: rtl_tcp magic read failed\n", b->band_index); close(fd); return -1; }
        got += n;
    }
    if (memcmp(magic, "RTL0", 4) != 0)
        fprintf(stderr, "Band %i: warning: no RTL0 magic (got %02x%02x%02x%02x)\n",
                b->band_index, magic[0], magic[1], magic[2], magic[3]);

    fprintf(stderr, "Band %i: connected to rtl_tcp at %s:%d\n", b->band_index, host, port);

    rtltcp_cmd(fd, 0x02, (uint32_t)b->samplerate);

    rtltcp_cmd(fd, 0x01, (uint32_t)(b->centerfreq_khz * 1000.0));

    if (b->gain_db != 0) {
        rtltcp_cmd(fd, 0x04, 1);
        rtltcp_cmd(fd, 0x05, (uint32_t)(b->gain_db * 10));
    } else {
        rtltcp_cmd(fd, 0x04, 0);
    }

    rtltcp_cmd(fd, 0x0d, 0);

    fcntl(fd, F_SETFL, O_NONBLOCK);

    b->fd = fd;
    b->rtltcp_fd = fd;
    fprintf(stderr, "Band %i: rtl_tcp ready\n", b->band_index);
    return 0;
}

int rtltcp_read_samples(struct BandState *b)
{
    uint8_t ibuf[4096];
    ssize_t n = read(b->rtltcp_fd, ibuf, sizeof(ibuf));
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        fprintf(stderr, "Band %i: rtl_tcp read error: %s\n", b->band_index, strerror(errno));
        return -1;
    }

    int pairs = (int)(n / 2);

    for (int i = 0; i < pairs; i++) {
        float fi = (float)ibuf[2*i + 0];
        float fq = (float)ibuf[2*i + 1];
        if (b->swapiq) { float t = fi; fi = fq; fq = t; }

        if (b->delay_buf && b->delay_samples > 0) {
            float *ptr = b->delay_buf_ptr;
            float oi = ptr[0], oq = ptr[1];
            ptr[0] = fi; ptr[1] = fq;
            ptr += 2;
            if (ptr >= b->delay_buf + b->delay_samples * 2) ptr = b->delay_buf;
            b->delay_buf_ptr = ptr;
            fi = oi; fq = oq;
        }

        int sc = b->sample_count;

        if (!b->noniq && b->fft_in_cx) {
            b->fft_in_cx[sc][0] = fi;
            b->fft_in_cx[sc][1] = fq;

            if (b->fft_filter_buf) {
                int cap = 4 * b->fftlen2;
                fftwf_complex *ring = (fftwf_complex *)b->fft_filter_buf;
                ring[b->wf_ring_pos][0] = fi;
                ring[b->wf_ring_pos][1] = fq;
                b->wf_ring_pos++;
                if (b->wf_ring_pos >= cap) b->wf_ring_pos = 0;
                if (b->wf_ring_fill < cap) b->wf_ring_fill++;
            }
        }

        b->sample_count = sc + 1;

        if (b->sample_count == b->half_fftlen)
            dsp_process_fft_block(b);
    }

    return pairs;
}

#include <sys/eventfd.h>
#include <pthread.h>

enum StdinFormatKind {
    STDINFMT_REAL_S8,
    STDINFMT_REAL_U8,
    STDINFMT_REAL_S16LE,
    STDINFMT_REAL_U16LE,
    STDINFMT_REAL_F32LE,
    STDINFMT_IQ_CS8,
    STDINFMT_IQ_CU8,
    STDINFMT_IQ_CS16LE,
    STDINFMT_IQ_CU16LE,
    STDINFMT_IQ_CF32LE
};

#define STDIN_RING_FRAMES (1 << 25)
#define STDIN_RING_MASK   (STDIN_RING_FRAMES - 1)

static float    stdin_ring_i[STDIN_RING_FRAMES];
static float    stdin_ring_q[STDIN_RING_FRAMES];
static volatile uint64_t stdin_ring_wr;
static volatile uint64_t stdin_ring_rd;
static volatile uint64_t stdin_ring_dropped;
static volatile uint64_t stdin_ring_overruns;
static int          stdin_event_fd = -1;
static volatile int stdin_eof;
static volatile int stdin_stop_requested;
static size_t       stdin_partial_len;
static struct BandState *stdin_band;
static pthread_t    stdin_reader_tid;
static pthread_t    stdin_dsp_tid;
static int          stdin_reader_started;
static int          stdin_dsp_started;
static enum StdinFormatKind stdin_format_kind;
static int          stdin_frame_bytes;
static int          stdin_is_iq;
static char         stdin_format_name[32];

static struct timespec stdin_dbg_last_dispatch;
static int             stdin_dbg_init;
static long long       stdin_dbg_total_produced;
static long long       stdin_dbg_last_prod_report;
static uint64_t        stdin_dbg_last_drop_report;
static int             stdin_measured_sps;
static struct timespec stdin_dbg_last_report;

static long long ts_us(struct timespec *t)
{
    return (long long)t->tv_sec * 1000000LL + t->tv_nsec / 1000;
}

static int stdin_parse_format(const char *name, int noniq,
                              enum StdinFormatKind *kind_out,
                              int *frame_bytes_out,
                              int *is_iq_out,
                              char *normalized,
                              size_t normalized_sz)
{
    const char *fmt = name && name[0] ? name : (noniq ? "s16le" : "cs16le");

    if (strcasecmp(fmt, "s8") == 0) {
        *kind_out = STDINFMT_REAL_S8;
        *frame_bytes_out = 1;
        *is_iq_out = 0;
        snprintf(normalized, normalized_sz, "s8");
    } else if (strcasecmp(fmt, "u8") == 0) {
        *kind_out = STDINFMT_REAL_U8;
        *frame_bytes_out = 1;
        *is_iq_out = 0;
        snprintf(normalized, normalized_sz, "u8");
    } else if (strcasecmp(fmt, "s16") == 0 || strcasecmp(fmt, "s16le") == 0) {
        *kind_out = STDINFMT_REAL_S16LE;
        *frame_bytes_out = 2;
        *is_iq_out = 0;
        snprintf(normalized, normalized_sz, "s16le");
    } else if (strcasecmp(fmt, "u16") == 0 || strcasecmp(fmt, "u16le") == 0) {
        *kind_out = STDINFMT_REAL_U16LE;
        *frame_bytes_out = 2;
        *is_iq_out = 0;
        snprintf(normalized, normalized_sz, "u16le");
    } else if (strcasecmp(fmt, "f32") == 0 || strcasecmp(fmt, "f32le") == 0) {
        *kind_out = STDINFMT_REAL_F32LE;
        *frame_bytes_out = 4;
        *is_iq_out = 0;
        snprintf(normalized, normalized_sz, "f32le");
    } else if (strcasecmp(fmt, "cs8") == 0) {
        *kind_out = STDINFMT_IQ_CS8;
        *frame_bytes_out = 2;
        *is_iq_out = 1;
        snprintf(normalized, normalized_sz, "cs8");
    } else if (strcasecmp(fmt, "cu8") == 0) {
        *kind_out = STDINFMT_IQ_CU8;
        *frame_bytes_out = 2;
        *is_iq_out = 1;
        snprintf(normalized, normalized_sz, "cu8");
    } else if (strcasecmp(fmt, "cs16") == 0 || strcasecmp(fmt, "cs16le") == 0) {
        *kind_out = STDINFMT_IQ_CS16LE;
        *frame_bytes_out = 4;
        *is_iq_out = 1;
        snprintf(normalized, normalized_sz, "cs16le");
    } else if (strcasecmp(fmt, "cu16") == 0 || strcasecmp(fmt, "cu16le") == 0) {
        *kind_out = STDINFMT_IQ_CU16LE;
        *frame_bytes_out = 4;
        *is_iq_out = 1;
        snprintf(normalized, normalized_sz, "cu16le");
    } else if (strcasecmp(fmt, "cf32") == 0 || strcasecmp(fmt, "cf32le") == 0) {
        *kind_out = STDINFMT_IQ_CF32LE;
        *frame_bytes_out = 8;
        *is_iq_out = 1;
        snprintf(normalized, normalized_sz, "cf32le");
    } else {
        return -1;
    }

    if (noniq && *is_iq_out)
        return -1;
    if (!noniq && !*is_iq_out)
        return -1;

    return 0;
}

static void stdin_decode_frame(const uint8_t *src, float *out_i, float *out_q)
{
    switch (stdin_format_kind) {
        case STDINFMT_REAL_S8:
            *out_i = (float)(*(const int8_t *)src);
            *out_q = 0.0f;
            break;
        case STDINFMT_REAL_U8:
            *out_i = (float)((int)src[0] - 128);
            *out_q = 0.0f;
            break;
        case STDINFMT_REAL_S16LE:
            *out_i = (float)(*(const int16_t *)src);
            *out_q = 0.0f;
            break;
        case STDINFMT_REAL_U16LE:
            *out_i = (float)((int)(*(const uint16_t *)src) - 32768);
            *out_q = 0.0f;
            break;
        case STDINFMT_REAL_F32LE:
            *out_i = *(const float *)src;
            *out_q = 0.0f;
            break;
        case STDINFMT_IQ_CS8:
            *out_i = (float)(*(const int8_t *)(src + 0));
            *out_q = (float)(*(const int8_t *)(src + 1));
            break;
        case STDINFMT_IQ_CU8:
            *out_i = (float)((int)src[0] - 128);
            *out_q = (float)((int)src[1] - 128);
            break;
        case STDINFMT_IQ_CS16LE:
            *out_i = (float)(*(const int16_t *)(src + 0));
            *out_q = (float)(*(const int16_t *)(src + 2));
            break;
        case STDINFMT_IQ_CU16LE:
            *out_i = (float)((int)(*(const uint16_t *)(src + 0)) - 32768);
            *out_q = (float)((int)(*(const uint16_t *)(src + 2)) - 32768);
            break;
        case STDINFMT_IQ_CF32LE:
            *out_i = *(const float *)(src + 0);
            *out_q = *(const float *)(src + 4);
            break;
    }
}

int stdin_get_measured_sps(void)
{
    return stdin_measured_sps;
}

static void *stdin_reader_thread(void *arg)
{
    (void)arg;
    uint8_t buf[1048576 + 8];

    for (;;) {
        if (stdin_stop_requested)
            break;

        size_t prefix = stdin_partial_len;

        ssize_t n = read(STDIN_FILENO, buf + prefix, sizeof(buf) - (size_t)prefix);
        if (n <= 0) {
            stdin_eof = 1;
            uint64_t v = 1;
            (void)write(stdin_event_fd, &v, sizeof(v));
            break;
        }

        size_t total_bytes = (size_t)n + (size_t)prefix;
        size_t whole_bytes = total_bytes - (total_bytes % (size_t)stdin_frame_bytes);
        stdin_partial_len = total_bytes - whole_bytes;
        if (stdin_partial_len > 0)
            memmove(buf, buf + whole_bytes, stdin_partial_len);

        int frames = (int)(whole_bytes / (size_t)stdin_frame_bytes);
        if (frames <= 0)
            continue;

        uint64_t wr = stdin_ring_wr;
        uint64_t rd_snap = stdin_ring_rd;
        uint64_t used = wr - rd_snap;
        uint64_t free_space = (used < STDIN_RING_FRAMES) ? (STDIN_RING_FRAMES - used) : 0;

        const uint8_t *src = buf;
        int to_write = frames;
        if ((uint64_t)to_write > free_space) {
            int drop = to_write - (int)free_space;
            to_write = (int)free_space;
            src += (size_t)drop * (size_t)stdin_frame_bytes;
            stdin_ring_dropped += (uint64_t)drop;
            stdin_ring_overruns++;
        }

        if (to_write > 0) {
            size_t ring_off = (size_t)(wr & STDIN_RING_MASK);
            size_t first = STDIN_RING_FRAMES - ring_off;
            if (first > (size_t)to_write)
                first = (size_t)to_write;

            for (size_t i = 0; i < first; i++)
                stdin_decode_frame(src + i * (size_t)stdin_frame_bytes,
                                   &stdin_ring_i[ring_off + i],
                                   &stdin_ring_q[ring_off + i]);

            for (size_t i = first; i < (size_t)to_write; i++)
                stdin_decode_frame(src + i * (size_t)stdin_frame_bytes,
                                   &stdin_ring_i[i - first],
                                   &stdin_ring_q[i - first]);

            wr += (uint64_t)to_write;
        }
        __sync_synchronize();
        stdin_ring_wr = wr;
        stdin_dbg_total_produced += to_write;

        if (to_write > 0) {
            uint64_t v = 1;
            (void)write(stdin_event_fd, &v, sizeof(v));
        }
    }
    return NULL;
}

static int stdin_process_samples(struct BandState *b)
{
    if (stdin_eof) {
        fprintf(stderr, "Band %i: stdin EOF — rx888_stream has exited\n", b->band_index);
        return -1;
    }

    struct timespec t_enter;
    clock_gettime(CLOCK_MONOTONIC, &t_enter);

    if (!stdin_dbg_init) {
        stdin_dbg_last_dispatch = t_enter;
        stdin_dbg_last_report = t_enter;
        stdin_dbg_last_prod_report = stdin_dbg_total_produced;
        stdin_dbg_init = 1;
    }

    __sync_synchronize();
    uint64_t wr = stdin_ring_wr;
    uint64_t rd = stdin_ring_rd;
    int avail = (int)(wr - rd);

    if (avail <= 0)
        return 0;

    int total_consumed = 0;
    int did_fft = 0;
    const int fft_budget = 16;

    int has_delay = (b->delay_buf && b->delay_samples > 0);

    while (total_consumed < avail) {
        int remaining = avail - total_consumed;
        int to_next = b->half_fftlen - b->sample_count;
        if (to_next <= 0) to_next = b->half_fftlen;
        int chunk = (remaining < to_next) ? remaining : to_next;

        if (b->noniq && !has_delay && b->fft_in_r2c) {
            int ring_off = (int)((rd + (uint64_t)total_consumed) & STDIN_RING_MASK);
            int sc = b->sample_count;
            int contig = STDIN_RING_FRAMES - ring_off;
            if (contig > chunk) contig = chunk;

            float *dst = b->fft_in_r2c + sc;
            const float *src = stdin_ring_i + ring_off;
            memcpy(dst, src, (size_t)contig * sizeof(*dst));

            int rest = chunk - contig;
            if (rest > 0) {
                dst  += contig;
                src   = stdin_ring_i;
                memcpy(dst, src, (size_t)rest * sizeof(*dst));
            }

            b->sample_count = sc + chunk;
        } else {
            for (int i = 0; i < chunk; i++) {
                int sc = b->sample_count;
                int ring_idx = (int)((rd + total_consumed + i) & STDIN_RING_MASK);

                if (b->noniq) {
                    float fi = stdin_ring_i[ring_idx];

                    if (has_delay) {
                        float *ptr = b->delay_buf_ptr;
                        float old  = *ptr;
                        *ptr = fi;
                        ptr++;
                        if (ptr >= b->delay_buf + b->delay_samples * 2) ptr = b->delay_buf;
                        b->delay_buf_ptr = ptr;
                        fi = old;
                    }

                    if (b->fft_in_r2c)
                        b->fft_in_r2c[sc] = fi;
                } else {
                    float fi = stdin_ring_i[ring_idx];
                    float fq = stdin_ring_q[ring_idx];
                    if (b->swapiq) { float t = fi; fi = fq; fq = t; }

                    if (has_delay) {
                        float *ptr = b->delay_buf_ptr;
                        float oi = ptr[0], oq = ptr[1];
                        ptr[0] = fi;
                        ptr[1] = fq;
                        ptr += 2;
                        if (ptr >= b->delay_buf + b->delay_samples * 2) ptr = b->delay_buf;
                        b->delay_buf_ptr = ptr;
                        fi = oi;
                        fq = oq;
                    }

                    if (b->fft_in_cx) {
                        b->fft_in_cx[sc][0] = fi;
                        b->fft_in_cx[sc][1] = fq;

                        if (b->fft_filter_buf) {
                            int cap = 4 * b->fftlen2;
                            fftwf_complex *ring = (fftwf_complex *)b->fft_filter_buf;
                            ring[b->wf_ring_pos][0] = fi;
                            ring[b->wf_ring_pos][1] = fq;
                            b->wf_ring_pos++;
                            if (b->wf_ring_pos >= cap) b->wf_ring_pos = 0;
                            if (b->wf_ring_fill < cap) b->wf_ring_fill++;
                        }
                    }
                }
                b->sample_count = sc + 1;
            }
        }

        total_consumed += chunk;

        if (b->sample_count == b->half_fftlen) {
            dsp_process_fft_block(b);
            stdin_dbg_last_dispatch = t_enter;
            did_fft++;
            if (did_fft >= fft_budget)
                break;
        }
    }

    __sync_synchronize();
    stdin_ring_rd = rd + (uint64_t)total_consumed;

    if (total_consumed < avail) {
        uint64_t kick = 1;
        (void)write(stdin_event_fd, &kick, sizeof(kick));
    }

    {
        struct timespec t_exit;
        clock_gettime(CLOCK_MONOTONIC, &t_exit);
        long long dt = (t_exit.tv_sec - t_enter.tv_sec) * 1000000LL
                     + (t_exit.tv_nsec - t_enter.tv_nsec) / 1000;
        dsp_add_stdin_us(dt);
    }

    long long since_report = ts_us(&t_enter) - ts_us(&stdin_dbg_last_report);
    if (since_report > 2000000) {
        long long produced_delta = stdin_dbg_total_produced - stdin_dbg_last_prod_report;
        if (since_report > 0 && produced_delta > 0) {
            long long sps = (produced_delta * 1000000LL) / since_report;
            if (sps > 1000000 && sps < 50000000)
                stdin_measured_sps = (int)sps;
        }
        stdin_dbg_last_report = t_enter;
        stdin_dbg_last_prod_report = stdin_dbg_total_produced;
        if (stdin_ring_dropped != stdin_dbg_last_drop_report) {
            uint64_t d = stdin_ring_dropped - stdin_dbg_last_drop_report;
            stdin_dbg_last_drop_report = stdin_ring_dropped;
            log_printf("stdin: dropped %llu samples in last interval (total=%llu overruns=%llu)\n",
                       (unsigned long long)d,
                       (unsigned long long)stdin_ring_dropped,
                       (unsigned long long)stdin_ring_overruns);
        }
    }

    return total_consumed;
}

static void *stdin_dsp_thread(void *arg)
{
    struct BandState *b = (struct BandState *)arg;

    for (;;) {
        uint64_t v;
        ssize_t n = read(stdin_event_fd, &v, sizeof(v));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec ts = {0, 1000000};
                nanosleep(&ts, NULL);
                continue;
            }
            break;
        }

        if (stdin_stop_requested)
            break;

        pthread_mutex_lock(&worker_dsp_mutex);
        int got = stdin_process_samples(b);
        if (got > 0)
            network_flush_ws_clients();
        pthread_mutex_unlock(&worker_dsp_mutex);

        if (got < 0)
            break;
    }

    return NULL;
}

int stdin_open_device(struct BandState *b)
{
    if (stdin_parse_format(b->stdinformat_str, b->noniq,
                           &stdin_format_kind, &stdin_frame_bytes, &stdin_is_iq,
                           stdin_format_name, sizeof(stdin_format_name)) != 0) {
        fprintf(stderr, "Band %i: invalid stdinformat '%s'\n",
                b->band_index,
                b->stdinformat_str && b->stdinformat_str[0] ? b->stdinformat_str : "(default)");
        return -1;
    }

    fcntl(STDIN_FILENO, F_SETPIPE_SZ, 16777216);

    stdin_event_fd = eventfd(0, EFD_NONBLOCK);
    if (stdin_event_fd < 0) {
        perror("eventfd");
        return -1;
    }

    stdin_ring_wr = 0;
    stdin_ring_rd = 0;
    stdin_ring_dropped = 0;
    stdin_ring_overruns = 0;
    stdin_eof = 0;
    stdin_stop_requested = 0;
    stdin_partial_len = 0;
    stdin_measured_sps = 0;
    stdin_dbg_init = 0;
    stdin_dbg_total_produced = 0;
    stdin_dbg_last_prod_report = 0;
    stdin_dbg_last_drop_report = 0;
    stdin_band = b;

    b->fd = stdin_event_fd;

    if (pthread_create(&stdin_reader_tid, NULL, stdin_reader_thread, NULL) != 0) {
        perror("pthread_create stdin reader");
        close(stdin_event_fd);
        stdin_event_fd = -1;
        return -1;
    }
    stdin_reader_started = 1;

    if (pthread_create(&stdin_dsp_tid, NULL, stdin_dsp_thread, b) != 0) {
        perror("pthread_create stdin dsp");
        stdin_stop_requested = 1;
        close(stdin_event_fd);
        stdin_event_fd = -1;
        pthread_cancel(stdin_reader_tid);
        pthread_join(stdin_reader_tid, NULL);
        stdin_reader_started = 0;
        return -1;
    }
    stdin_dsp_started = 1;

    fprintf(stderr, "Band %i: reading %s %s samples from stdin (threaded)\n",
            b->band_index, stdin_format_name, stdin_is_iq ? "IQ" : "real");
    fprintf(stderr, "Band %i: stdin DSP path moved off the network thread\n",
            b->band_index);
    return 0;
}

int stdin_read_samples(struct BandState *b)
{
    uint64_t v;
    (void)read(stdin_event_fd, &v, sizeof(v));
    return stdin_process_samples(b);
}

int stdin_stop_device(struct BandState *b)
{
    if (stdin_band && b != stdin_band)
        return 0;

    stdin_stop_requested = 1;
    if (stdin_event_fd >= 0) {
        uint64_t v = 1;
        (void)write(stdin_event_fd, &v, sizeof(v));
    }

    if (stdin_dsp_started) {
        pthread_join(stdin_dsp_tid, NULL);
        stdin_dsp_started = 0;
    }
    if (stdin_reader_started) {
        pthread_cancel(stdin_reader_tid);
        pthread_join(stdin_reader_tid, NULL);
        stdin_reader_started = 0;
    }

    stdin_band = NULL;
    return 0;
}

int alsa_read_samples(struct BandState *b)
{

    int channel_count = b->noniq ? 1 : 2;
    if (b->device_ch > 0) channel_count = (b->device_ch + 1) * 2;
    int16_t ibuf[1024 * 8];
    snd_pcm_sframes_t n;

    n = snd_pcm_readi(b->alsa_handle, ibuf, 1024);
    if (n < 0) {
        n = snd_pcm_recover(b->alsa_handle, (int)n, 0);
        if (n < 0) {
            fprintf(stderr, "ALSA read error on band %i: %s\n",
                    b->band_index, snd_strerror((int)n));
            return -1;
        }
        n = snd_pcm_readi(b->alsa_handle, ibuf, 1024);
        if (n < 0) return -1;
    }

    int frames = (int)n;

    int ch_base = b->device_ch * 2;

    if (b->noniq) {
        for (int i = 0; i < frames; i++) {
            float fi = (float)ibuf[i * channel_count + ch_base];

            if (b->delay_buf && b->delay_samples > 0) {
                float *ptr = b->delay_buf_ptr;
                float old  = *ptr;
                *ptr = fi;
                ptr++;
                if (ptr >= b->delay_buf + b->delay_samples * 2) ptr = b->delay_buf;
                b->delay_buf_ptr = ptr;
                fi = old;
            }

            int sc = b->sample_count;

            if (b->fft_in_r2c)
                b->fft_in_r2c[sc] = fi;

            b->sample_count = sc + 1;

            if (b->sample_count == b->half_fftlen)
                dsp_process_fft_block(b);
        }
    } else {
        for (int i = 0; i < frames; i++) {
            float fi = (float)ibuf[i * channel_count + ch_base];
            float fq = (float)ibuf[i * channel_count + ch_base + 1];
            if (b->swapiq) { float t = fi; fi = fq; fq = t; }

            if (b->delay_buf && b->delay_samples > 0) {
                float *ptr = b->delay_buf_ptr;
                float oi = ptr[0], oq = ptr[1];
                ptr[0] = fi; ptr[1] = fq;
                ptr += 2;
                if (ptr >= b->delay_buf + b->delay_samples * 2) ptr = b->delay_buf;
                b->delay_buf_ptr = ptr;
                fi = oi; fq = oq;
            }

            int sc = b->sample_count;

            if (b->fft_in_cx) {
                b->fft_in_cx[sc][0] = fi;
                b->fft_in_cx[sc][1] = fq;
            }

            b->sample_count = sc + 1;

            if (b->sample_count == b->half_fftlen)
                dsp_process_fft_block(b);
        }
    }

    return frames;
}
