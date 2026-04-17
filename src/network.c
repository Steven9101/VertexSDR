// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#include "common.h"
#include "config.h"
#include "logging.h"
#include "client.h"
#include "band.h"
#include "dsp.h"
#include "audio.h"
#include "worker.h"
#include "chat.h"
#include "logbook.h"
#include "waterfall.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <poll.h>
#include <unistd.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <math.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <limits.h>
#include <ctype.h>

static const char WS_MAGIC[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

#define HP_MAX        2048

#define MAX_EVENTS    1024

#define HP_BUF_SIZE   1024

#define EPOLL_TAG_HTTP    UINT64_C(0x200000000)
#define EPOLL_TAG_LISTEN  UINT64_C(0x300000000)
#define EPOLL_TAG_ALSA    UINT64_C(0x400000000)
#define EPOLL_TAG_MASK    UINT64_C(0xFFFFFFFF00000000)

typedef struct {
    int      state;
    int      fd;
    void    *ws_ptr;
    void    *raw_ptr;
    int      file_fd;
    char     buf[HP_BUF_SIZE];
    int      buf_len;
    char     ip_str[72];
    time_t   last_ts;
    time_t   created_ts;
    char     cookie[32];
    int      ws_mode;
    int      ws_audio_user;
    int      async_state;
    int      async_close_after;
    char    *async_resp;
    size_t   async_resp_len;
    size_t   async_resp_off;
} HttpConn;

static HttpConn hp[HP_MAX];
static uint32_t hp_gen[HP_MAX];

#define HTTP_IDLE_TIMEOUT_SEC     90
#define HTTP_HEADER_TIMEOUT_SEC    0
#define HTTP_MAX_CONN_PER_IP       0
#define NET_ACCEPT_BUDGET         32
#define NET_HTTP_IO_BUDGET       256
#define NET_WS_IO_BUDGET         256
#define NET_HTTP_TIMESLICE_US   3000

int epoll_fd;

static int listen_fd;

volatile int g_configreload = 0;

static struct {
    float cpu_pct;
    float gpu_pct;
    float avg_users;
    float audio_kbps;
    float wf_kbps;
    float http_kbps;
    int   chseq;
    int   stats_chseq;

    long long audio_bytes;
    long long wf_bytes;
    long long http_bytes;

    double    user_integral;
    long long user_integral_ts_us;
    int       num_users;

    long long last_ru_utime_us;
    long long last_ru_stime_us;
    long long last_wall_us;

    long long next_update_us;

    long long last_rc6_ms;
    int       rc6_available;
} g_stats;

static inline long long mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

#define HTTP_ASYNC_QMAX 256
#define HTTP_ASYNC_THREADS 2

typedef struct {
    int      idx;
    uint32_t gen;
    int      use_cache;
    int      is_html;
    char     req[HP_BUF_SIZE];
    char     relpath[512];
    char     mime[64];
} HttpAsyncJob;

typedef struct {
    int      idx;
    uint32_t gen;
    char    *resp;
    size_t   resp_len;
    int      close_after;
} HttpAsyncResult;

static pthread_t http_async_tid[HTTP_ASYNC_THREADS];
static pthread_mutex_t http_async_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  http_async_cv = PTHREAD_COND_INITIALIZER;
static HttpAsyncJob    http_async_q[HTTP_ASYNC_QMAX];
static int             http_async_q_head;
static int             http_async_q_tail;
static int             http_async_q_len;
static HttpAsyncResult http_async_r[HTTP_ASYNC_QMAX];
static int             http_async_r_head;
static int             http_async_r_tail;
static int             http_async_r_len;

typedef struct {
    float freq_khz;
    char mode[8];
    char text[192];
    int lines;
} StationInfoEntry;

static StationInfoEntry *g_stationinfo = NULL;
static int g_stationinfo_count = 0;
static int g_stationinfo_cap = 0;
static time_t g_stationinfo_mtime = 0;
static char g_stationinfo_path[512] = "";

static void stationinfo_clear(void)
{
    free(g_stationinfo);
    g_stationinfo = NULL;
    g_stationinfo_count = 0;
    g_stationinfo_cap = 0;
}

static int stationinfo_cmp(const void *a, const void *b)
{
    const StationInfoEntry *ea = (const StationInfoEntry *)a;
    const StationInfoEntry *eb = (const StationInfoEntry *)b;
    if (ea->freq_khz < eb->freq_khz) return -1;
    if (ea->freq_khz > eb->freq_khz) return 1;
    return 0;
}

static int stationinfo_ensure_cap(int want)
{
    if (g_stationinfo_cap >= want) return 1;
    int new_cap = g_stationinfo_cap ? g_stationinfo_cap * 2 : 64;
    while (new_cap < want) new_cap *= 2;
    StationInfoEntry *next = (StationInfoEntry *)realloc(g_stationinfo, (size_t)new_cap * sizeof(*next));
    if (!next) return 0;
    g_stationinfo = next;
    g_stationinfo_cap = new_cap;
    return 1;
}

static void stationinfo_append(float freq_khz, const char *mode, const char *text)
{
    for (int i = 0; i < g_stationinfo_count; i++) {
        StationInfoEntry *e = &g_stationinfo[i];
        if (fabsf(e->freq_khz - freq_khz) < 0.0005f) {
            if (e->lines < 3 && (int)strlen(e->text) < 170) {
                size_t cur = strlen(e->text);
                snprintf(e->text + cur, sizeof(e->text) - cur, "<br>%s", text);
                e->lines++;
            }
            return;
        }
    }
    if (!stationinfo_ensure_cap(g_stationinfo_count + 1)) return;
    StationInfoEntry *e = &g_stationinfo[g_stationinfo_count++];
    memset(e, 0, sizeof(*e));
    e->freq_khz = freq_khz;
    snprintf(e->mode, sizeof(e->mode), "%s", mode ? mode : "");
    snprintf(e->text, sizeof(e->text), "%s", text ? text : "");
    e->lines = 1;
}

static int stationinfo_parse_file(FILE *f)
{
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;

        char *end = NULL;
        double freq = strtod(p, &end);
        if (end == p || freq <= 0.0) continue;
        p = end;
        while (*p == ' ' || *p == '\t') p++;

        char mode[8] = "";
        char text[192] = "";
        char *text_start = p;
        if (*p && *p != '\n') {
            char *tok_end = p;
            while (*tok_end && *tok_end != ' ' && *tok_end != '\t' && *tok_end != '\n' && *tok_end != '\r')
                tok_end++;
            int tok_len = (int)(tok_end - p);
            int alpha = 0;
            for (int i = 0; i < tok_len; i++) {
                char ch = p[i];
                if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
                    alpha = 1;
                    break;
                }
            }
            if (tok_len > 0 && tok_len <= 4 && alpha) {
                int copy = tok_len < (int)sizeof(mode) - 1 ? tok_len : (int)sizeof(mode) - 1;
                memcpy(mode, p, (size_t)copy);
                mode[copy] = '\0';
                p = tok_end;
                while (*p == ' ' || *p == '\t') p++;
                text_start = p;
            }
        }

        int ti = 0;
        while (*text_start && *text_start != '\n' && *text_start != '\r' && ti < (int)sizeof(text) - 1) {
            text[ti++] = *text_start++;
        }
        while (ti > 0 && (text[ti - 1] == ' ' || text[ti - 1] == '\t')) ti--;
        text[ti] = '\0';
        if (!text[0]) continue;
        stationinfo_append((float)freq, mode, text);
    }
    return 1;
}

static void html_escape_into(const char *src, char *dst, size_t dst_size)
{
    size_t n = 0;
    if (!dst_size) return;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p && n + 1 < dst_size; p++) {
        const char *rep = NULL;
        switch (*p) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default: break;
        }
        if (rep) {
            size_t rep_len = strlen(rep);
            if (n + rep_len >= dst_size) break;
            memcpy(dst + n, rep, rep_len);
            n += rep_len;
        } else if (*p >= 32 && *p <= 126) {
            dst[n++] = (char)*p;
        } else {
            dst[n++] = ' ';
        }
    }
    dst[n] = '\0';
}

static void mask_ip_string(const char *src, char *dst, size_t dst_size)
{
    if (!dst_size) return;
    if (!src || !src[0]) {
        snprintf(dst, dst_size, "-");
        return;
    }

    const char *ip = src;
    if (strncmp(ip, "::ffff:", 7) == 0)
        ip += 7;

    if (strchr(ip, '.')) {
        unsigned a, b, c, d;
        if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            snprintf(dst, dst_size, "%u.%u.x.x", a, b);
            return;
        }
    }

    if (strchr(ip, ':')) {
        char copy[72];
        snprintf(copy, sizeof(copy), "%s", ip);
        char *last = strrchr(copy, ':');
        if (last) {
            *last = '\0';
            last = strrchr(copy, ':');
            if (last)
                snprintf(dst, dst_size, "%s:x:x", copy);
            else
                snprintf(dst, dst_size, "%s:x", copy);
            return;
        }
    }

    snprintf(dst, dst_size, "%s", ip);
}

static void mask_ips_in_text(const char *src, char *dst, size_t dst_size)
{
    size_t n = 0;
    if (!dst_size) return;
    dst[0] = '\0';
    if (!src) return;

    for (size_t i = 0; src[i] && n + 1 < dst_size; ) {
        if ((src[i] >= '0' && src[i] <= '9') || src[i] == ':') {
            size_t j = i;
            while (src[j] && (isalnum((unsigned char)src[j]) || src[j] == '.' ||
                              src[j] == ':' || src[j] == '[' || src[j] == ']' ||
                              src[j] == '-'))
                j++;

            if (j > i) {
                char token[128];
                size_t tok_len = j - i;
                if (tok_len >= sizeof(token))
                    tok_len = sizeof(token) - 1;
                memcpy(token, src + i, tok_len);
                token[tok_len] = '\0';

                char host[128];
                snprintf(host, sizeof(host), "%s", token);
                size_t host_len = strlen(host);
                if (host[0] == '[' && host_len > 2 && host[host_len - 1] == ']') {
                    memmove(host, host + 1, host_len - 2);
                    host[host_len - 2] = '\0';
                }
                char *port = strrchr(host, ':');
                if (port && strchr(host, '.') && strchr(port + 1, ':') == NULL) {
                    *port = '\0';
                }

                unsigned char ipv4[4];
                unsigned char ipv6[16];
                if (inet_pton(AF_INET, host, ipv4) == 1 || inet_pton(AF_INET6, host, ipv6) == 1) {
                    char masked[96];
                    mask_ip_string(host, masked, sizeof(masked));
                    size_t mlen = strlen(masked);
                    if (n + mlen >= dst_size)
                        break;
                    memcpy(dst + n, masked, mlen);
                    n += mlen;
                    i = j;
                    continue;
                }
            }
        }
        dst[n++] = src[i++];
    }
    dst[n] = '\0';
}

static int sanitize_relative_path(const char *src, char *dst, size_t dst_size)
{
    size_t n = 0;
    const char *p = src;
    if (!src || !src[0] || !dst_size) return 0;
    if (*p == '/' || *p == '\\') return 0;

    while (*p) {
        while (*p == '/')
            p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/')
            p++;
        size_t seg_len = (size_t)(p - seg);
        if (seg_len == 0) continue;
        if (seg_len == 1 && seg[0] == '.') continue;
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') return 0;
        for (size_t i = 0; i < seg_len; i++) {
            unsigned char ch = (unsigned char)seg[i];
            if (ch == '\\' || ch < 32) return 0;
        }
        if (n && n + 1 >= dst_size) return 0;
        if (n) dst[n++] = '/';
        if (n + seg_len >= dst_size) return 0;
        memcpy(dst + n, seg, seg_len);
        n += seg_len;
    }

    if (n == 0) return 0;
    dst[n] = '\0';
    return 1;
}

static int resolve_public_path(const char *relpath, char *out, size_t out_size, struct stat *st_out)
{
    char clean[512];
    struct stat st;
    if (!sanitize_relative_path(relpath, clean, sizeof(clean)))
        return 0;

    if (config.public2_dir[0]) {
        snprintf(out, out_size, "%s/%s", config.public2_dir, clean);
        if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
            if (st_out) *st_out = st;
            return 1;
        }
    }
    if (config.public_dir[0]) {
        snprintf(out, out_size, "%s/%s", config.public_dir, clean);
        if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
            if (st_out) *st_out = st;
            return 1;
        }
    }
    return 0;
}

static const char *client_display_name(const struct Client *cl)
{
    if (cl->username[0]) return cl->username;
    if (cl->description[0]) {
        static __thread char masked[64];
        mask_ip_string(cl->description, masked, sizeof(masked));
        return masked;
    }
    return "?";
}

static int stationinfo_resolve(char *path, size_t path_len, struct stat *st)
{
    if (config.public2_dir[0]) {
        snprintf(path, path_len, "%s/stationinfo.txt", config.public2_dir);
        if (stat(path, st) == 0) return 1;
    }
    if (config.public_dir[0]) {
        snprintf(path, path_len, "%s/stationinfo.txt", config.public_dir);
        if (stat(path, st) == 0) return 1;
    }
    snprintf(path, path_len, "stationinfo.txt");
    if (stat(path, st) == 0) return 1;
    return 0;
}

static void stationinfo_load_if_changed(void)
{
    char path[512];
    struct stat st;
    if (!stationinfo_resolve(path, sizeof(path), &st)) {
        stationinfo_clear();
        g_stationinfo_mtime = 0;
        g_stationinfo_path[0] = '\0';
        return;
    }

    if (g_stationinfo_count > 0 &&
        g_stationinfo_mtime == st.st_mtime &&
        strcmp(g_stationinfo_path, path) == 0) {
        return;
    }

    FILE *f = fopen(path, "r");
    if (!f) return;
    stationinfo_clear();
    stationinfo_parse_file(f);
    fclose(f);
    if (g_stationinfo_count > 1)
        qsort(g_stationinfo, (size_t)g_stationinfo_count, sizeof(*g_stationinfo), stationinfo_cmp);
    g_stationinfo_mtime = st.st_mtime;
    snprintf(g_stationinfo_path, sizeof(g_stationinfo_path), "%s", path);
}

static void js_escape_dq(const char *src, char *dst, int dstlen)
{
    int j = 0;
    for (int i = 0; src[i] && j < dstlen - 2; i++) {
        char ch = src[i];
        if (ch == '\\' || ch == '"') {
            if (j >= dstlen - 3) break;
            dst[j++] = '\\';
            dst[j++] = ch;
        } else if ((unsigned char)ch >= 32) {
            dst[j++] = ch;
        }
    }
    dst[j] = '\0';
}

static void qs_decode(const char *src, char *dst, int dstlen);
static int  qs_get_param(const char *req, const char *key, char *dst, int dstlen);

static int g_maxusers  = 0;
static int g_skipmute  = 0;

static char g_wf_text[80];

static int client_is_audio_user(const struct Client *cl)
{
    return cl->state == CLIENT_WEBSOCKET
        && cl->audio_format > 0
        && cl->audio_format != 36;
}

static int count_http_slots_for_ip(const char *ip)
{
    int n = 0;
    for (int i = 0; i < HP_MAX; i++) {
        if (!hp[i].state) continue;
        if (strcmp(hp[i].ip_str, ip) == 0)
            n++;
    }
    return n;
}

static int count_audio_users(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_is_audio_user(&clients[i]))
            n++;
    }
    return n;
}

static int orgstatus_user_count(void)
{
    return count_audio_users();
}

static int client_mobile_platform(const struct Client *cl)
{
    if (!cl)
        return 0;
    if (strncmp(cl->username, "mobile", 6) != 0)
        return 0;
    if (strcmp(cl->username, "mobile/Android") == 0)
        return 2;
    if (strcmp(cl->username, "mobile/iOS") == 0)
        return 3;
    return 1;
}

static const char *orgstatus_mobile_path(void)
{
    static const char *mobile_candidates[] = {
        "m.html",
        "mobile.html",
        "mobile/index.html",
    };

    char path[512];
    for (size_t i = 0; i < sizeof(mobile_candidates) / sizeof(mobile_candidates[0]); i++) {
        if (resolve_public_path(mobile_candidates[i], path, sizeof(path), NULL))
            return mobile_candidates[i];
    }
    return NULL;
}

static void stats_sync_user_count(long long now_us)
{
    int actual = count_audio_users();
    if (actual == g_stats.num_users)
        return;

    long long elapsed = now_us - g_stats.user_integral_ts_us;
    if (elapsed > 0)
        g_stats.user_integral += (double)actual * (double)elapsed;
    g_stats.user_integral_ts_us = now_us;
    g_stats.num_users = actual;
}

static void orgstatus_obfuscate_email(char *s)
{
    char *p = strstr(s, "Email: ");
    if (!p) return;
    p += 7;
    while (*p > 31) {
        *p = (char)(*p ^ 1);
        p++;
    }
}

static int sysop_is_trusted(const char *ip)
{
    if (config.donttrustlocalhost) {
        log_printf("sysop_is_trusted: denied %s (donttrustlocalhost)\n", ip);
        return 0;
    }

    const char *addr = ip;
    if (strncmp(addr, "::ffff:", 7) == 0)
        addr += 7;

    if (strcmp(addr, "::1") == 0)
        return 1;

    if (strcmp(addr, "127.0.0.1") == 0)
        return 1;

    if (config.dotrust[0]) {
        const char *p = strstr(config.dotrust, addr);
        if (p) {
            int ok_before = (p == config.dotrust || (unsigned char)p[-1] <= ' ');
            int ok_after  = ((unsigned char)p[strlen(addr)] <= ' ');
            if (ok_before && ok_after)
                return 1;
        }
    }

    if (config.dotrustlocalnet) {
        if (strncmp(addr, "10.", 3) == 0)
            return 1;
        if (strncmp(addr, "192.168.", 8) == 0)
            return 1;
    }

    log_printf("sysop_is_trusted: denied %s (addr=%s dotrustlocalnet=%d dotrust='%s')\n",
               ip, addr, (int)config.dotrustlocalnet, config.dotrust);
    return 0;
}

static void stats_accumulate_users(long long now_us)
{
    long long elapsed = now_us - g_stats.user_integral_ts_us;
    if (elapsed > 0)
        g_stats.user_integral += (double)g_stats.num_users * (double)elapsed;
    g_stats.user_integral_ts_us = now_us;
}

static void stats_change_users(int delta)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long now_us = (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
    stats_accumulate_users(now_us);
    g_stats.num_users += delta;
    if (g_stats.num_users < 0) g_stats.num_users = 0;
}

void stats_add_bytes(int type, long long n)
{
    if      (type == 0) g_stats.audio_bytes += n;
    else if (type == 1) g_stats.wf_bytes    += n;
    else                g_stats.http_bytes  += n;
}

static long long read_rc6_residency_ms(void)
{
    FILE *f = fopen("/sys/class/drm/card0/gt/gt0/rc6_residency_ms", "r");
    if (!f) return -1;
    long long val = -1;
    if (fscanf(f, "%lld", &val) != 1) val = -1;
    fclose(f);
    return val;
}

static void stats_update(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long now_us = (long long)tv.tv_sec * 1000000LL + tv.tv_usec;

    stats_sync_user_count(now_us);
    stats_accumulate_users(now_us);

    long long wall_elapsed = now_us - g_stats.last_wall_us;
    if (wall_elapsed < 1) wall_elapsed = 1;

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    long long utime_us = (long long)ru.ru_utime.tv_sec * 1000000LL + ru.ru_utime.tv_usec;
    long long stime_us = (long long)ru.ru_stime.tv_sec * 1000000LL + ru.ru_stime.tv_usec;
    long long cpu_us   = (utime_us - g_stats.last_ru_utime_us)
                       + (stime_us - g_stats.last_ru_stime_us);
    g_stats.cpu_pct    = (float)((double)cpu_us / (double)wall_elapsed * 100.0);
    g_stats.last_ru_utime_us = utime_us;
    g_stats.last_ru_stime_us = stime_us;

    struct DspTimingStats dt;
    dsp_get_and_reset_timing(&dt);

    long long rc6_now = read_rc6_residency_ms();
    if (rc6_now >= 0 && g_stats.last_rc6_ms >= 0) {
        long long rc6_delta = rc6_now - g_stats.last_rc6_ms;
        double wall_ms = (double)wall_elapsed / 1000.0;
        if (wall_ms > 0.0) {
            double idle_pct = (double)rc6_delta / wall_ms * 100.0;
            g_stats.gpu_pct = (float)(100.0 - idle_pct);
            if (g_stats.gpu_pct < 0.0f) g_stats.gpu_pct = 0.0f;
            g_stats.rc6_available = 1;
        }
    }
    g_stats.last_rc6_ms = rc6_now;

    g_stats.avg_users = (float)(g_stats.user_integral / (double)wall_elapsed);
    g_stats.user_integral = 0.0;

    double secs = (double)wall_elapsed / 1e6;
    if (secs < 0.1) secs = 0.1;
    g_stats.audio_kbps = (float)((double)g_stats.audio_bytes * 8.0 / 1000.0 / secs);
    g_stats.wf_kbps    = (float)((double)g_stats.wf_bytes    * 8.0 / 1000.0 / secs);
    g_stats.http_kbps  = (float)((double)g_stats.http_bytes  * 8.0 / 1000.0 / secs);

    g_stats.audio_bytes = 0;
    g_stats.wf_bytes    = 0;
    g_stats.http_bytes  = 0;
    g_stats.last_wall_us = now_us;
    g_stats.next_update_us = now_us + 10000000LL;
    g_stats.stats_chseq = chat_get_pending_chseq();

}

static void set_tcp_options(int fd)
{
    int v;
    v = 1;  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,  &v, sizeof(v));
    v = 9;  setsockopt(fd, SOL_TCP,    TCP_KEEPCNT,    &v, sizeof(v));
    v = 60; setsockopt(fd, SOL_TCP,    TCP_KEEPIDLE,   &v, sizeof(v));
    v = 1;  setsockopt(fd, SOL_TCP,    TCP_NODELAY,    &v, sizeof(v));
    v = 30; setsockopt(fd, SOL_TCP,    TCP_KEEPINTVL,  &v, sizeof(v));
}

static inline uint64_t hp_encode(int idx) { return (uint64_t)idx + EPOLL_TAG_HTTP; }
static inline int      hp_decode(uint64_t v) { return (int)(v & 0xFFFFFFFFu); }

static void hp_close(int idx)
{
    HttpConn *c = &hp[idx];
    if (!c->state) return;

    if (c->ws_ptr) {
        pthread_mutex_lock(&worker_dsp_mutex);
        struct Client *cl = (struct Client *)c->ws_ptr;
        if (c->ws_audio_user)
            stats_change_users(-1);
        client_close(cl);
        c->ws_ptr = NULL;
        c->ws_audio_user = 0;
        pthread_mutex_unlock(&worker_dsp_mutex);
    }

    c->state = 0;
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    c->fd = 0;

    if (c->file_fd > 0) {
        close(c->file_fd);
        c->file_fd = 0;
    }
    if (c->async_resp) {
        free(c->async_resp);
        c->async_resp = NULL;
    }
    c->async_resp_len = 0;
    c->async_resp_off = 0;
    c->async_state = 0;
    c->async_close_after = 0;
    hp_gen[idx]++;
}

static void hp_close_write_fail(int idx)
{
    hp_close(idx);
}

static void hp_try_flush_async(int idx)
{
    if (idx < 0 || idx >= HP_MAX) return;
    HttpConn *c = &hp[idx];
    if (!c->state || c->ws_mode || c->async_state != 2 || !c->async_resp)
        return;

    while (c->async_resp_off < c->async_resp_len) {
        ssize_t wr = write(c->fd,
                           c->async_resp + c->async_resp_off,
                           c->async_resp_len - c->async_resp_off);
        if (wr > 0) {
            c->async_resp_off += (size_t)wr;
            continue;
        }
        if (wr < 0 && errno == EINTR)
            continue;
        if (wr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        hp_close(idx);
        return;
    }

    if (!c->state || !c->async_resp || c->async_resp_off < c->async_resp_len)
        return;

    g_stats.http_bytes += (long long)c->async_resp_len;
    free(c->async_resp);
    c->async_resp = NULL;
    c->async_resp_len = 0;
    c->async_resp_off = 0;
    c->async_state = 0;
    if (c->async_close_after) {
        hp_close(idx);
    } else {
        c->async_close_after = 0;
        c->buf_len = 0;
        c->buf[0] = '\0';
    }
}

static int socket_write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    int eagain_tries = 0;
    const int eagain_try_max = 80;
    const int eagain_poll_ms = 5;
    while (off < len) {
        ssize_t wr = write(fd, p + off, len - off);
        if (wr > 0) {
            off += (size_t)wr;
            eagain_tries = 0;
            continue;
        }
        if (wr < 0 && errno == EINTR)
            continue;
        if (wr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (eagain_tries >= eagain_try_max)
                return -1;
            struct pollfd pfd;
            pfd.fd = fd;
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

static void http_send_response_typed(int idx, const char *status, const char *content_type, const char *body)
{
    HttpConn *c = &hp[idx];
    int client_wants_close = (strstr(c->buf, "Connection: close") != NULL
                              || strstr(c->buf, "connection: close") != NULL);
    char hdr[1024];
    int blen = (int)strlen(body);
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Server: WebSDR/%s\r\n"
        "Content-Length: %d\r\n"
        "Content-Type: %s\r\n"
        "Cache-control: no-cache\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status, VERSION_STRING, blen, content_type ? content_type : "text/html",
        client_wants_close ? "close" : "keep-alive");
    if (socket_write_all(c->fd, hdr, (size_t)hlen) != 0) { hp_close_write_fail(idx); return; }
    if (blen > 0 && socket_write_all(c->fd, body, (size_t)blen) != 0) { hp_close_write_fail(idx); return; }
    g_stats.http_bytes += (long long)(hlen + blen);
}

static void http_send_response(int idx, const char *status, const char *body)
{
    http_send_response_typed(idx, status, "text/html", body);
}

static const char *mime_for_ext(const char *ext)
{
    if (!ext) return NULL;
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".js")   == 0) return "application/x-javascript";
    if (strcmp(ext, ".css")  == 0) return "text/css";
    if (strcmp(ext, ".png")  == 0) return "image/png";
    if (strcmp(ext, ".gif")  == 0) return "image/gif";
    if (strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".jpg") == 0) return "image/jpeg";
    if (strcmp(ext, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(ext, ".txt")  == 0) return "text/plain";
    if (strcmp(ext, ".jar")  == 0) return "applet/java-archive";
    if (strcmp(ext, ".class")== 0) return "applet/java";
    if (strcmp(ext, ".swf")  == 0) return "application/x-shockwave-flash";
    if (strcmp(ext, ".pdf")  == 0) return "application/pdf";
    if (strcmp(ext, ".zip")  == 0) return "application/zip";
    if (strcmp(ext, ".tgz")  == 0) return "application/octet-stream";
    if (strcmp(ext, ".gz")   == 0) return "application/x-gzip";
    return NULL;
}

static int net_qs_int(const char *req, const char *key)
{

    const char *q = strchr(req, '?');
    if (!q) return -1;
    size_t klen = strlen(key);
    for (const char *p = q + 1; p && *p > 32; ) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
            return (int)strtol(p + klen + 1, NULL, 10);
        p = strchr(p, '&');
        if (p) p++;
    }
    return -1;
}

static void http_send_ok_text(int idx, const char *body)
{
    HttpConn *c = &hp[idx];
    int close = (strstr(c->buf, "Connection: close") != NULL
              || strstr(c->buf, "connection: close") != NULL);
    http_send_response(idx, "200 OK", body);
    if (close) {
        hp_close(idx);
    } else {
        c->buf_len = 0;
        c->buf[0]  = '\0';
    }
}

static void http_send_ok_plain(int idx, const char *body)
{
    HttpConn *c = &hp[idx];
    int close = (strstr(c->buf, "Connection: close") != NULL
              || strstr(c->buf, "connection: close") != NULL);
    http_send_response_typed(idx, "200 OK", "text/plain", body);
    if (close) {
        hp_close(idx);
    } else {
        c->buf_len = 0;
        c->buf[0]  = '\0';
    }
}

static void http_send_ok_text_noclose(int idx, const char *body)
{
    http_send_response(idx, "200 OK", body);

}

static void handle_tilde_stream(int idx)
{
    HttpConn *c = &hp[idx];
    int fmt  = net_qs_int(c->buf, "format");
    int band = net_qs_int(c->buf, "band");
    int mute = net_qs_int(c->buf, "mute");

    if (fmt <= 0) {
        fmt = 23;
    } else if (fmt == 36) {
        fmt = 0;
    } else if (fmt != 23) {
        hp_close(idx);
        return;
    }

    struct Client *cl = (struct Client *)c->ws_ptr;
    if (!cl) {

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].state == CLIENT_WEBSOCKET && clients[i].fd == c->fd) {
                cl = &clients[i];
                c->ws_ptr = cl;
                break;
            }
        }
    }
    if (!cl) {

        cl = client_assign_fd(c->fd, c->ip_str);
        if (cl) c->ws_ptr = cl;
    }

    if (cl) {
        pthread_mutex_lock(&worker_dsp_mutex);
        int was_audio_user = client_is_audio_user(cl);
        cl->audio_format = fmt;
        int new_band = (band >= 0 && band < config.num_bands) ? band
                     : (cl->band_idx < 0 && config.num_bands > 0) ? 0 : cl->band_idx;
        if (new_band != cl->band_idx) {
            if (cl->band_idx >= 0 && cl->band_idx < config.num_bands)
                bands[cl->band_idx].audio_clients--;
            cl->band_idx = new_band;
            cl->band_idx_req = new_band;
            cl->band_ptr = (new_band >= 0 && new_band < config.num_bands) ? &bands[new_band] : NULL;
            if (new_band >= 0 && new_band < config.num_bands)
                bands[new_band].audio_clients++;
            cl->uu_chseq = chat_get_pending_chseq();
        } else if (cl->band_idx < 0 && config.num_bands > 0) {
            cl->band_idx = 0;
            cl->band_idx_req = 0;
            cl->band_ptr = &bands[0];
            bands[0].audio_clients++;
            cl->uu_chseq = chat_get_pending_chseq();
        }
        if (mute >= 0)
            cl->mute = mute;
        int is_audio_user = client_is_audio_user(cl);
        if (!was_audio_user && is_audio_user)
            stats_change_users(+1);
        else if (was_audio_user && !is_audio_user)
            stats_change_users(-1);
        c->ws_audio_user = is_audio_user ? 1 : 0;
        pthread_mutex_unlock(&worker_dsp_mutex);
        log_printf("fd=%i -%s-: assigned audio stream band=%i format=%i\n",
                   c->fd, c->ip_str, cl->band_idx, fmt);
    }

    if (c->ws_mode) return;
    http_send_ok_text_noclose(idx, "0\r\n");
}

static void handle_tilde_waterstream(int idx)
{
    HttpConn *c = &hp[idx];

    int band = -1;
    const char *ws_ptr = strstr(c->buf, "/~~waterstream");
    if (ws_ptr) {
        const char *after = ws_ptr + 14;
        if (*after >= '0' && *after <= '9')
            band = (int)strtol(after, NULL, 10);
    }
    if (band < 0) band = net_qs_int(c->buf, "band");

    int fmt = net_qs_int(c->buf, "format");
    if (fmt < 0) fmt = 0;

    int width = net_qs_int(c->buf, "width");
    int zoom  = net_qs_int(c->buf, "zoom");
    int start = net_qs_int(c->buf, "start");

    struct Client *cl = (struct Client *)c->ws_ptr;
    if (!cl) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].state == CLIENT_WEBSOCKET && clients[i].fd == c->fd) {
                cl = &clients[i]; c->ws_ptr = cl; break;
            }
        }
    }
    if (!cl) {
        cl = client_assign_fd(c->fd, c->ip_str);
        if (cl) c->ws_ptr = cl;
    }

    if (cl) {
        pthread_mutex_lock(&worker_dsp_mutex);
        int was_audio_user = client_is_audio_user(cl);
        cl->audio_format = 36;
        cl->wf_format    = (fmt == 8 || fmt == 9 || fmt == 10) ? fmt : 0;
        int new_band = (band >= 0 && band < config.num_bands) ? band
                     : (cl->band_idx < 0 && config.num_bands > 0) ? 0 : cl->band_idx;
        if (new_band != cl->band_idx) {
            if (cl->band_idx >= 0 && cl->band_idx < config.num_bands)
                bands[cl->band_idx].audio_clients--;
            cl->band_idx = new_band;
            cl->band_idx_req = new_band;
            cl->band_ptr = (new_band >= 0 && new_band < config.num_bands) ? &bands[new_band] : NULL;
            if (new_band >= 0 && new_band < config.num_bands)
                bands[new_band].audio_clients++;
            cl->uu_chseq = chat_get_pending_chseq();
        } else if (cl->band_idx < 0 && config.num_bands > 0) {
            cl->band_idx = 0;
            cl->band_idx_req = 0;
            cl->band_ptr = &bands[0];
            bands[0].audio_clients++;
            cl->uu_chseq = chat_get_pending_chseq();
        }
        if (width > 0)  cl->wf_width  = width;
        if (zoom  >= 0) cl->zoom      = zoom;
        if (start >= 0) cl->wf_offset = start;
        if (was_audio_user)
            stats_change_users(-1);
        c->ws_audio_user = 0;
        pthread_mutex_unlock(&worker_dsp_mutex);
        log_printf("fd=%i -%s-: assigned waterfall stream band=%i format=%i\n",
                   c->fd, c->ip_str, cl->band_idx, fmt);
    }

    if (c->ws_mode) return;
    http_send_ok_text_noclose(idx, "0\r\n");
}

static void handle_tilde_iqbalance(int idx)
{
    HttpConn *c = &hp[idx];
    int band = net_qs_int(c->buf, "band");

    struct Client *cl = (struct Client *)c->ws_ptr;
    if (!cl) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].state == CLIENT_WEBSOCKET && clients[i].fd == c->fd) {
                cl = &clients[i]; c->ws_ptr = cl; break;
            }
        }
    }
    if (!cl) {
        cl = client_assign_fd(c->fd, c->ip_str);
        if (cl) c->ws_ptr = cl;
    }

    if (cl) {
        pthread_mutex_lock(&worker_dsp_mutex);
        int was_audio_user = client_is_audio_user(cl);
        cl->audio_format = 36;
        if (band >= 0 && band < config.num_bands)
            cl->band_idx = band;
        cl->band_idx_req = cl->band_idx;
        if (cl->band_idx >= 0 && cl->band_idx < config.num_bands)
            cl->band_ptr = &bands[cl->band_idx];
        if (was_audio_user)
            stats_change_users(-1);
        c->ws_audio_user = 0;
        pthread_mutex_unlock(&worker_dsp_mutex);
        log_printf("fd=%i -%s-: assigned iqbalance band=%i\n",
                   c->fd, c->ip_str, cl->band_idx);
    }

    http_send_ok_text(idx, "0\r\n");
}

static void handle_tilde_raw(int idx)
{
    HttpConn *c = &hp[idx];

    if (config.rawpassword[0]) {
        char given[256] = "";
        qs_get_param(c->buf, "pwd", given, sizeof(given));
        if (strcmp(given, config.rawpassword) != 0) {
            http_send_response(idx, "403 Forbidden", "");
            hp_close(idx);
            return;
        }
    }

    struct Client *cl = (struct Client *)c->ws_ptr;
    if (!cl) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].state == CLIENT_WEBSOCKET && clients[i].fd == c->fd) {
                cl = &clients[i]; c->ws_ptr = cl; break;
            }
        }
    }
    if (!cl) {
        cl = client_assign_fd(c->fd, c->ip_str);
        if (cl) c->ws_ptr = cl;
    }

    if (cl) {
        pthread_mutex_lock(&worker_dsp_mutex);
        int was_audio_user = client_is_audio_user(cl);
        int band = net_qs_int(c->buf, "band");
        if (band >= 0 && band < config.num_bands)
            cl->band_idx = band;
        cl->band_idx_req = cl->band_idx;
        if (cl->band_idx >= 0 && cl->band_idx < config.num_bands)
            cl->band_ptr = &bands[cl->band_idx];
        cl->audio_format = 36;
        if (was_audio_user)
            stats_change_users(-1);
        c->ws_audio_user = 0;
        pthread_mutex_unlock(&worker_dsp_mutex);
        log_printf("fd=%i -%s-: assigned raw stream band=%i\n",
                   c->fd, c->ip_str, cl->band_idx);
    }

    http_send_ok_text(idx, "0\r\n");
}

static void handle_tilde_status(int idx)
{
    char body[8192];
    int n = 0;
    n += snprintf(body + n, sizeof(body) - (size_t)n,
                  "maxusers=%i  skipmute=%i  \n\n",
                  g_maxusers > 0 ? g_maxusers : config.max_clients,
                  g_skipmute);
    n += snprintf(body + n, sizeof(body) - (size_t)n,
                  "name     overruns readb.  blocks          timestamp  users  10sec_ADmin/max  ever_ADmin/max  source\n\n");
    for (int i = 0; i < config.num_bands && n < (int)sizeof(body) - 128; i++) {
        const struct BandState *b = &bands[i];
        char masked_source[256];
        mask_ips_in_text(config.bands[i].device[0] ? config.bands[i].device : "(none)",
                         masked_source, sizeof(masked_source));
        n += snprintf(body + n, sizeof(body) - (size_t)n,
                      "%-8s %8i %3i %10i  %10i.%06i  %-5i  %7i %-7i %7i %-7i  %i,%s\n",
                      b->name[0] ? b->name : "(band)",
                      0,
                      b->read_pos,
                      b->sample_count,
                      (int)(b->last_active_ts / 1000000ULL),
                      (int)(b->last_active_ts % 1000000ULL),
                      b->num_users,
                      b->peak_min,
                      b->peak_max,
                      b->peak_min,
                      b->peak_max,
                      i,
                      masked_source);
    }
    n += snprintf(body + n, sizeof(body) - (size_t)n,
                  "\n\n%g(%lli) %g(%lli) %g(%lli) %g %g(%i) %g\n",
                  (double)g_stats.audio_kbps,
                  g_stats.audio_bytes,
                  (double)g_stats.wf_kbps,
                  g_stats.wf_bytes,
                  (double)g_stats.http_kbps,
                  g_stats.http_bytes,
                  (double)g_stats.cpu_pct,
                  (double)g_stats.avg_users,
                  g_stats.num_users,
                  g_stats.rc6_available ? (double)g_stats.gpu_pct : 0.0);
    http_send_response_typed(idx, "200 OK", "text/plain", body);
    hp_close(idx);
}

static void handle_tilde_waterfalltext(int idx)
{
    HttpConn *c = &hp[idx];
    char text[80];
    if (qs_get_param(c->buf, "text", text, sizeof(text)) && sysop_is_trusted(c->ip_str)) {
        strncpy(g_wf_text, text, sizeof(g_wf_text) - 1);
        g_wf_text[sizeof(g_wf_text) - 1] = '\0';

        for (int b = 0; b < config.num_bands; b++) {
            waterfall_render_text_rows(g_wf_text, bands[b].wf_text_rows);
            bands[b].wf_text_pending = 32;
        }
    }
    http_send_ok_text(idx, "ok");
}

static void handle_tilde_orgstatus(int idx)
{
    int users = orgstatus_user_count();
    const char *mobile_path = orgstatus_mobile_path();
    int req_cfg = net_qs_int(hp[idx].buf, "config");
    static int cfg_serial = 0;
    if (cfg_serial == 0)
        cfg_serial = (int)(time(NULL) & 0x7fffffff);

    if (req_cfg == cfg_serial) {
        char body[64];
        snprintf(body, sizeof(body), "Users: %d\n", users);
        http_send_ok_plain(idx, body);
        return;
    }

    char body[8192];
    int off = 0;
    off += snprintf(body + off, sizeof(body) - (size_t)off, "Config: %d\n", cfg_serial);

    if (config.org_info[0]) {
        char orgbuf[sizeof(config.org_info)];
        snprintf(orgbuf, sizeof(orgbuf), "%s", config.org_info);
        orgstatus_obfuscate_email(orgbuf);
        off += snprintf(body + off, sizeof(body) - (size_t)off, "%s", orgbuf);
    }

    if (mobile_path) {
        off += snprintf(body + off, sizeof(body) - (size_t)off,
                        "Mobile: %s\n", mobile_path);
    }
    off += snprintf(body + off, sizeof(body) - (size_t)off, "Bands: %d\n", config.num_bands);
    for (int i = 0; i < config.num_bands && off < (int)sizeof(body) - 128; i++) {
        off += snprintf(body + off, sizeof(body) - (size_t)off,
                        "Band: %d %f %f %s\n",
                        i,
                        bands[i].centerfreq_khz,
                        (double)bands[i].samplerate * 0.001,
                        bands[i].name);
    }

    snprintf(body + off, sizeof(body) - (size_t)off, "Users: %d\n", users);
    http_send_ok_plain(idx, body);
}

char g_config_file[256] = "websdr.cfg";

void ws_broadcast_text(const char *msg)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].state == CLIENT_WEBSOCKET)
            client_ws_send_text(&clients[i], msg);
    }
}

static void handle_tilde_configreload(int idx)
{
    HttpConn *c = &hp[idx];
    int how = net_qs_int(c->buf, "how");
    if (how <= 0) {
        http_send_response(idx, "403 Forbidden", "");
        hp_close(idx);
        return;
    }
    if (!sysop_is_trusted(c->ip_str)) {
        http_send_response(idx, "403 Forbidden", "");
        hp_close(idx);
        return;
    }

    char cfgname[128];
    if (qs_get_param(c->buf, "cfg", cfgname, sizeof(cfgname)) && cfgname[0])
        strncpy(g_config_file, cfgname, sizeof(g_config_file) - 1);

    if (how == 3) {

        g_configreload = 3;
    } else {

        g_configreload = how;
    }

    http_send_ok_text(idx, "ok\r\n");
}

static void qs_decode(const char *src, char *dst, int dstlen)
{
    int i = 0;
    while (*src && *src != '&' && *src != ' ' && *src != '\r' && *src != '\n'
           && i < dstlen - 1) {
        if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static int qs_get_param(const char *req, const char *key,
                        char *dst, int dstlen)
{
    size_t klen = strlen(key);

    const char *search = strchr(req, '?');
    if (!search) search = strstr(req, "\r\n\r\n");
    if (!search) search = req;

    const char *p = search;
    if (*p == '?' || *p == '&') p++;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            qs_decode(p + klen + 1, dst, dstlen);
            return 1;
        }

        while (*p && *p != '&' && *p != '\r' && *p != '\n') p++;
        if (*p == '&') p++;
        else break;
    }
    dst[0] = '\0';
    return 0;
}

static void handle_tilde_fetchdx(int idx)
{
    HttpConn *c = &hp[idx];
    stationinfo_load_if_changed();

    char minbuf[64], maxbuf[64];
    double min_khz = -1e30, max_khz = 1e30;
    if (qs_get_param(c->buf, "min", minbuf, sizeof(minbuf)) && minbuf[0])
        min_khz = strtod(minbuf, NULL);
    if (qs_get_param(c->buf, "max", maxbuf, sizeof(maxbuf)) && maxbuf[0])
        max_khz = strtod(maxbuf, NULL);

    char body[65536];
    int n = 0;
    for (int i = 0; i < g_stationinfo_count && n < (int)sizeof(body) - 256; i++) {
        const StationInfoEntry *e = &g_stationinfo[i];
        if (e->freq_khz < (float)min_khz || e->freq_khz > (float)max_khz)
            continue;
        char mode_esc[32], text_esc[384];
        js_escape_dq(e->mode, mode_esc, sizeof(mode_esc));
        js_escape_dq(e->text, text_esc, sizeof(text_esc));
        n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
                      "dx(%f,\"%s\",\"%s\");\n",
                      e->freq_khz, mode_esc, text_esc);
    }
    body[n] = '\0';
    http_send_response(idx, "200 OK", body);
    hp_close(idx);
}

static void handle_tilde_chat(int idx, int is_post)
{
    HttpConn *c = &hp[idx];
    (void)is_post;

    char msg[512] = "", namebuf[64] = "";
    qs_get_param(c->buf, "msg",  msg,     sizeof(msg));
    qs_get_param(c->buf, "name", namebuf, sizeof(namebuf));

    if (msg[0]) {

        chat_handle_post(msg, namebuf[0] ? namebuf : NULL, c->ip_str);
        http_send_ok_text(idx, "ok\r\n");
    } else {

        char body[32768];
        int n = chat_handle_get(body, sizeof(body));
        body[n] = '\0';
        http_send_response(idx, "200 OK", body);
        hp_close(idx);
    }
}

static void handle_tilde_chatidentities(int idx)
{
    char body[32768];
    int n = 0;
    time_t now = time(NULL);
    for (int i = 0; i < MAX_CLIENTS && n < (int)sizeof(body) - 96; i++) {
        const struct Client *cl = &clients[i];
        int age = 0;
        if (cl->state != CLIENT_FREE && cl->last_active > 0 && now >= cl->last_active)
            age = (int)(now - cl->last_active);
        char masked[64];
        char display[64];
        mask_ip_string(cl->description, masked, sizeof(masked));
        snprintf(display, sizeof(display), "%s", client_display_name(cl));
        n += snprintf(body + n, sizeof(body) - (size_t)n,
                      "%4i %10i -%s- -%s-\n",
                      i, age, masked, display);
    }
    body[n] = '\0';
    http_send_response_typed(idx, "200 OK", "text/plain", body);
    hp_close(idx);
}

static void handle_tilde_chatcensor(int idx)
{
    HttpConn *c = &hp[idx];
    if (!sysop_is_trusted(c->ip_str)) {
        http_send_ok_text(idx, "ok\r\n");
        return;
    }

    char val[256];

    if (qs_get_param(c->buf, "disable", val, sizeof(val)))
        chat_set_disabled(atoi(val));

    if (qs_get_param(c->buf, "line", val, sizeof(val)) && val[0])
        chat_handle_censor_line(val);

    http_send_ok_text(idx, "ok\r\n");
}

static void handle_tilde_setconfig(int idx)
{
    HttpConn *c = &hp[idx];
    if (sysop_is_trusted(c->ip_str)) {
        char val[32];
        if (qs_get_param(c->buf, "maxusers", val, sizeof(val))) {
            int n = atoi(val);
            if (n < 0) n = 0;
            if (n > 512) n = 512;
            g_maxusers = n;
        }
        if (qs_get_param(c->buf, "skipmute", val, sizeof(val)))
            g_skipmute = atoi(val) ? 1 : 0;
    }
    http_send_ok_text(idx, "ok\r\n");
}

static float g_wf_dir = 0.0f;

static void handle_tilde_setdir(int idx)
{
    HttpConn *c = &hp[idx];
    char val[32];
    if (sysop_is_trusted(c->ip_str) && qs_get_param(c->buf, "dir", val, sizeof(val)))
        g_wf_dir = (float)atof(val);
    http_send_ok_text(idx, "ok\r\n");
}

static void handle_tilde_blockmee(int idx)
{
    HttpConn *c = &hp[idx];
    FILE *f = fopen("reject.txt", "a");
    if (f) {

        char safe_ip[72];
        strncpy(safe_ip, c->ip_str, sizeof(safe_ip) - 1);
        safe_ip[sizeof(safe_ip) - 1] = '\0';
        for (char *p = safe_ip; *p; p++) {
            if ((unsigned char)(*p - 32) > 0x5E)
                *p = ' ';
        }
        fprintf(f, "\n# added by ~~blockmee\n%s\n", safe_ip);
        fclose(f);
    }
    http_send_ok_text(idx, "ok\r\n");
}

static void handle_tilde_logbook(int idx)
{
    HttpConn *c = &hp[idx];
    int nr = net_qs_int(c->buf, "nr");
    char body[65536];
    int n = logbook_handle_get(body, sizeof(body), nr);

    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Server: WebSDR/%s\r\n"
        "Content-Length: %d\r\n"
        "Content-Type: text/plain\r\n"
        "Cache-control: no-cache\r\n"
        "\r\n",
        VERSION_STRING, n);
    if (write(c->fd, hdr, (size_t)hlen) != hlen) { hp_close_write_fail(idx); return; }
    if (n > 0 && write(c->fd, body, (size_t)n) != n) { hp_close_write_fail(idx); return; }
    g_stats.http_bytes += (long long)(hlen + n);
    hp_close(idx);
}

static void handle_tilde_loginsert(int idx)
{
    HttpConn *c = &hp[idx];

    char call[64] = "", comment[256] = "", freq[32] = "", namebuf[64] = "";
    const char *q = strchr(c->buf, '?');
    if (q) {

        char tmp[512];

        const char *p = strstr(q, "call=");
        if (p) { p += 5; int i = 0; while (*p && *p != '&' && *p != ' ' && i < 63) call[i++] = *p++; call[i] = '\0'; }

        p = strstr(q, "comment=");
        if (p) {
            p += 8; int i = 0;
            while (*p && *p != '&' && *p != ' ' && i < 255) {
                if (*p == '+') { comment[i++] = ' '; p++; }
                else if (*p == '%' && p[1] && p[2]) {
                    char hex[3] = {p[1], p[2], 0};
                    comment[i++] = (char)strtol(hex, NULL, 16); p += 3;
                } else comment[i++] = *p++;
            }
            comment[i] = '\0';
        }

        p = strstr(q, "freq=");
        if (p) { p += 5; int i = 0; while (*p && *p != '&' && *p != ' ' && i < 31) freq[i++] = *p++; freq[i] = '\0'; }

        p = strstr(q, "name=");
        if (p) {
            p += 5; int i = 0;
            while (*p && *p != '&' && *p != ' ' && i < 63) {
                if (*p == '+') { namebuf[i++] = ' '; p++; }
                else if (*p == '%' && p[1] && p[2]) {
                    char hex[3] = {p[1], p[2], 0};
                    namebuf[i++] = (char)strtol(hex, NULL, 16); p += 3;
                } else namebuf[i++] = *p++;
            }
            namebuf[i] = '\0';
        }
        (void)tmp;
    }
    logbook_handle_insert(call, comment, freq, namebuf);
    http_send_ok_text(idx, "ok\r\n");
}

static double client_uu_freq(const struct Client *cl)
{
    if (cl->band_idx < 0 || cl->band_idx >= config.num_bands) return 0.5;
    const struct BandState *b = &bands[cl->band_idx];
    double sr_hz = (b->samplerate > 0) ? (double)b->samplerate : 2048000.0;
    double bw_khz = sr_hz / 1000.0;

    double bin_frac = ((double)cl->filter_lo_bin + (double)cl->filter_hi_bin) * 0.5 / sr_hz;

    double tune_frac = (bw_khz > 0.0)
        ? (cl->tune_khz - b->centerfreq_khz + bw_khz * 0.5) / bw_khz
        : 0.5;
    double freq = bin_frac + tune_frac;
    if (freq < 0.0) freq = 0.0;
    if (freq > 1.0) freq = 1.0;
    return freq;
}

static void handle_tilde_othersjj(int idx)
{
    char body[65536];
    int n = 0;

    int client_chseq = net_qs_int(hp[idx].buf, "chseq");
    if (client_chseq < 0) client_chseq = 0;

    n = chat_write_othersjj(body, (int)sizeof(body), n, client_chseq);

    int cur_chseq = chat_get_chseq();

    if (client_chseq < g_stats.stats_chseq && n < (int)sizeof(body) - 512) {
        n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
            "statsobj.innerHTML=\"Past 10 seconds: CPUload=%.1f%%",
            g_stats.cpu_pct);
        if (g_stats.rc6_available)
            n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
                ", GPUload=%.1f%%", g_stats.gpu_pct);
        n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
            ", %.2f users; "
            "audio %.1f kb/s, waterfall %.1f kb/s, http %.1f kb/s\";\n",
            g_stats.avg_users,
            g_stats.audio_kbps, g_stats.wf_kbps, g_stats.http_kbps);
    }

    if (n < (int)sizeof(body) - 64) {
        int nusers = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            const struct Client *cl = &clients[i];
            if (client_is_audio_user(cl))
                nusers++;
        }
        n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
                      "numusersobj.innerHTML=\"%i\";\n", nusers);
    }

    for (int i = 0; i < MAX_CLIENTS && n < (int)sizeof(body) - 128; i++) {
        const struct Client *cl = &clients[i];
        if (client_chseq >= cl->uu_chseq) continue;

        const char *name = "";
        int band = (cl->band_idx >= 0) ? cl->band_idx : 0;
        double freq = 0.5;

        if (client_is_audio_user(cl)) {
            name = client_display_name(cl);
            band = (cl->band_idx >= 0) ? cl->band_idx : 0;
            freq = client_uu_freq(cl);
        }

        n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
                      "uu(%i,'%s',%i,%f);\n", i, name, band, freq);
        (void)cur_chseq;
    }

    http_send_response(idx, "200 OK", body);
    hp_close(idx);
}

static void handle_tilde_othersj(int idx)
{
    char body[65536];
    int n = 0;

    int client_chseq = net_qs_int(hp[idx].buf, "chseq");
    if (client_chseq < 0) client_chseq = 0;

    n = chat_write_othersjj(body, (int)sizeof(body), n, client_chseq);
    int cur_chseq = chat_get_chseq();

    if (client_chseq < g_stats.stats_chseq && n < (int)sizeof(body) - 512) {
        n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
            "statsobj.innerHTML=\"Past 10 seconds: CPUload=%.1f%%",
            g_stats.cpu_pct);
        if (g_stats.rc6_available)
            n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
                ", GPUload=%.1f%%", g_stats.gpu_pct);
        n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
            ", %.2f users", g_stats.avg_users);
        for (int bi = 0; bi < config.num_bands && n < (int)sizeof(body) - 128; bi++) {
            n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
                "<br>%s: audio %.1f kb/s, waterfall %.1f kb/s",
                bands[bi].name[0] ? bands[bi].name : "(band)",
                g_stats.audio_kbps, g_stats.wf_kbps);
        }
        n += snprintf(body + n, (size_t)((int)sizeof(body) - n), "\";\n");
    }

    if (client_chseq < cur_chseq && n < (int)sizeof(body) - 256) {
        int total = 0;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            const struct Client *cl = &clients[i];
            if (client_is_audio_user(cl)
                && cl->band_idx >= 0)
                total++;
        }
        n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
                      "numusersobj.innerHTML=\"%i\";\n", total);

        n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
                      "usersobj.innerHTML='");
        n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
                      "<div class=others>");

        static const char *colours[] = {
            "#ff4040","#40ff40","#4040ff","#ffff40",
            "#ff40ff","#40ffff","#ffffff","#ff8040"
        };
        for (int b = 0; b < config.num_bands && n < (int)sizeof(body) - 512; b++) {
            n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
                "<p><div align=\"left\" style=\"width:1024px; background-color:black;\">");
            for (int i = 0; i < MAX_CLIENTS && n < (int)sizeof(body) - 256; i++) {
                const struct Client *cl = &clients[i];
                if (cl->state != CLIENT_WEBSOCKET) continue;
                if (!client_is_audio_user(cl)) continue;
                if (cl->band_idx != b) continue;
                const char *uname = client_display_name(cl);
                double freq = client_uu_freq(cl);
                int left_px = (int)(freq * 1024.0) - 250;
                n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
                    "<div id=\"user%i\" align=\"center\" style=\"position:relative;"
                    "left:%ipx;width:500px; color:%s;\"><b>%s</b></div>",
                    i, left_px, colours[i & 7], uname);
            }
            n += snprintf(body + n, (size_t)((int)sizeof(body) - n), "</div></p>");
        }
        n += snprintf(body + n, (size_t)((int)sizeof(body) - n), "</div>';\n");
    }

    if (client_chseq > 2) {
        for (int i = 0; i < MAX_CLIENTS && n < (int)sizeof(body) - 128; i++) {
            const struct Client *cl = &clients[i];
            if (client_chseq >= cl->uu_chseq) continue;
            if (!client_is_audio_user(cl)) continue;
            double freq = client_uu_freq(cl);
            int left_px = (int)(freq * 1024.0) - 250;
            n += snprintf(body + n, (size_t)((int)sizeof(body) - n),
                "document.getElementById('user%i').style.left=\"%ipx\";\n",
                i, left_px);
        }
    }
    (void)cur_chseq;

    http_send_response(idx, "200 OK", body);
    hp_close(idx);
}

static void handle_tilde_otherstable(int idx)
{
    char body[65536];
    int n = 0;

    n += snprintf(body + n, sizeof(body) - (size_t)n,
        "<style type='text/css'>table {text-align:center; empty-cells:hide;}</style>\n");

    int total_audio = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        const struct Client *cl = &clients[i];
        if (client_is_audio_user(cl))
            total_audio++;
    }
    n += snprintf(body + n, sizeof(body) - (size_t)n,
        "This WebSDR is currently being used by %i user(s) simultaneously:<br>",
        total_audio);

    n += snprintf(body + n, sizeof(body) - (size_t)n,
        "<table border=1>"
        "<tr><th rowspan=2>Name</th><th colspan=4>Audio</th>"
        "<th rowspan=2><font size=-1>IP address</font></th>"
        "<th colspan=6>Waterfall</th></tr>\n"
        "<tr><th>band</th><th>freq/kHz</th><th>filter&amp;dem.</th><th>format;bps</th>"
        "<th>band</th><th>center/kHz</th><th>width/kHz</th><th>speed</th>"
        "<th>format</th><th>bits/pixel</th></tr>\n");

    int cnt_ws = 0, cnt_wf_ws = 0, cnt_sq = 0, cnt_an = 0, cnt_muted = 0;
    int cnt_android = 0, cnt_mobile_android = 0, cnt_mobile_ios = 0;
    int cnt_mobile_total = 0, cnt_ssb_wide = 0, cnt_am_wide = 0;

    for (int i = 0; i < MAX_CLIENTS && n < (int)sizeof(body) - 512; i++) {
        const struct Client *cl = &clients[i];
        int mobile_platform = client_mobile_platform(cl);
        if (cl->state != CLIENT_WEBSOCKET) continue;
        if (cl->audio_format == 36) cnt_wf_ws++;
        if (!client_is_audio_user(cl) && cl->audio_format != 36)
            continue;
        if (client_is_audio_user(cl)) {
            cnt_ws++;
            if (cl->squelch)   cnt_sq++;
            if (cl->autonotch) cnt_an++;
            if (cl->mute)      cnt_muted++;
            if (cl->js_mode == 0 && (cl->hi_khz - cl->lo_khz) > 3.1) cnt_ssb_wide++;
            if (cl->js_mode == 1 && (cl->hi_khz - cl->lo_khz) > 9.1) cnt_am_wide++;
            if (mobile_platform != 0) {
                cnt_mobile_total++;
                if (mobile_platform == 2) {
                    cnt_android++;
                    cnt_mobile_android++;
                } else if (mobile_platform == 3) {
                    cnt_mobile_ios++;
                }
            }
        }

        char uname[128];
        char masked_ip[64];
        html_escape_into(client_display_name(cl), uname, sizeof(uname));
        mask_ip_string(cl->description, masked_ip, sizeof(masked_ip));

        const char *modename = "SSB";
        switch (cl->js_mode) {
            case 1:  modename = "AM";  break;
            case 4:  modename = "FM";  break;
            default: modename = "SSB"; break;
        }
        double freq_khz = cl->tune_khz;
        int band_nr = cl->band_idx;
        int wf_band = (cl->band_idx >= 0) ? cl->band_idx : 0;
        double wf_center = (cl->band_idx >= 0 && cl->band_idx < config.num_bands)
            ? bands[cl->band_idx].centerfreq_khz : 0.0;
        double wf_width = (cl->band_idx >= 0 && cl->band_idx < config.num_bands)
            ? (double)bands[cl->band_idx].samplerate / 1000.0 / (double)(1 << cl->zoom) : 0.0;
        int wf_speed = (cl->wf_speed > 0) ? cl->wf_speed : 1;
        int bits_per_pixel = 0;
        if (cl->wf_format == 10) bits_per_pixel = 108;
        else if (cl->wf_format == 9) bits_per_pixel = 108;
        else if (cl->wf_format == 8) bits_per_pixel = 172;

        if (client_is_audio_user(cl)) {
            n += snprintf(body + n, sizeof(body) - (size_t)n,
                "<tr><td>%s</td><td>%i</td><td>%7.2f</td>"
                "<td>%.2f...%.2f %s</td><td style='background-color: #FFFFC0'>%iws/%i, %5.2f</td>"
                "<td><font size=-1>%s</font></td>",
                uname, band_nr, freq_khz,
                cl->lo_khz, cl->hi_khz, modename,
                cl->last_audio_rate, cl->audio_format, 6.72f,
                masked_ip);
        } else {
            n += snprintf(body + n, sizeof(body) - (size_t)n,
                "<tr><td colspan=5></td><td><font size=-1>%s</font></td>",
                masked_ip);
        }

        if (cl->audio_format == 36 || cl->wf_format > 0 || cl->wf_width > 0) {
            n += snprintf(body + n, sizeof(body) - (size_t)n,
                "<td>%i</td><td>%7.1f</td><td>%7.1f</td><td>%i</td>"
                "<td style='background-color: #C0FFFF'>%iws</td><td>%4.2f</td><td><td></tr>\n",
                wf_band, wf_center, wf_width, wf_speed,
                cl->wf_format, bits_per_pixel / 100.0f);
        } else {
            n += snprintf(body + n, sizeof(body) - (size_t)n,
                "<td colspan=6></td><td><td></tr>\n");
        }
    }
    n += snprintf(body + n, sizeof(body) - (size_t)n, "</table>");

    n += snprintf(body + n, sizeof(body) - (size_t)n,
        "<p><table>"
        "<tr><td>Audio websockets:</td><td>%i</td></tr>"
        "<tr><td>Waterfall websockets:</td><td>%i</td></tr>"
        "<tr><td>Enabled squelches:</td><td>%i</td></tr>"
        "<tr><td>Enabled autonotch:</td><td>%i</td></tr>"
        "<tr><td>Muted:</td><td>%i</td></tr>"
        "<tr><td>Android:</td><td>%i</td></tr>"
        "<tr><td>Mobile Android:</td><td>%i</td></tr>"
        "<tr><td>Mobile iOS:</td><td>%i</td></tr>"
        "<tr><td>Mobile total:</td><td>%i</td></tr>"
        "<tr><td>SSB wide:</td><td>%i</td></tr>"
        "<tr><td>AM wide:</td><td>%i</td></tr>"
        "</table>",
        cnt_ws, cnt_wf_ws, cnt_sq, cnt_an, cnt_muted,
        cnt_android, cnt_mobile_android, cnt_mobile_ios, cnt_mobile_total,
        cnt_ssb_wide, cnt_am_wide);

    http_send_response(idx, "200 OK", body);
    hp_close(idx);
}

static int user_histogram[24];

static void handle_tilde_histogram(int idx)
{
    char body[512];
    int n = 0;
    n += snprintf(body + n, sizeof(body) - (size_t)n, "[");
    for (int h = 0; h < 24; h++) {
        if (h > 0) body[n++] = ',';
        n += snprintf(body + n, sizeof(body) - (size_t)n, "%d", user_histogram[h]);
    }
    snprintf(body + n, sizeof(body) - (size_t)n, "]");
    http_send_response(idx, "200 OK", body);
    hp_close(idx);
}

void network_update_histogram(void)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    user_histogram[tm->tm_hour] = num_clients;
}

static char *ssi_process_file(const char *filepath, int depth, int *out_len)
{
    if (depth > 5) return NULL;

    FILE *f = fopen(filepath, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 2*1024*1024) { fclose(f); return NULL; }

    char *raw = (char *)malloc((size_t)fsize + 1);
    if (!raw) { fclose(f); return NULL; }
    size_t rd = fread(raw, 1, (size_t)fsize, f);
    fclose(f);
    raw[rd] = '\0';

    size_t outsize = (size_t)rd * 2 + 65536;
    char *out = (char *)malloc(outsize);
    if (!out) { free(raw); return NULL; }
    size_t opos = 0;

    const char *p = raw;
    while (*p) {
        const char *ssi = strstr(p, "<!--#include file=\"");
        if (!ssi) {

            size_t rem = strlen(p);
            if (opos + rem >= outsize) {
                outsize = opos + rem + 65536;
                out = (char *)realloc(out, outsize);
                if (!out) { free(raw); return NULL; }
            }
            memcpy(out + opos, p, rem);
            opos += rem;
            break;
        }

        size_t before = (size_t)(ssi - p);
        if (opos + before >= outsize) {
            outsize = opos + before + 65536;
            out = (char *)realloc(out, outsize);
            if (!out) { free(raw); return NULL; }
        }
        memcpy(out + opos, p, before);
        opos += before;

        const char *fname_start = ssi + 19;
        const char *fname_end = strchr(fname_start, '"');
        if (!fname_end) {

            out[opos++] = *ssi;
            p = ssi + 1;
            continue;
        }
        char incname[256];
        char clean_inc[256];
        size_t nlen = (size_t)(fname_end - fname_start);
        if (nlen >= sizeof(incname)) nlen = sizeof(incname) - 1;
        memcpy(incname, fname_start, nlen);
        incname[nlen] = '\0';
        if (!sanitize_relative_path(incname, clean_inc, sizeof(clean_inc)))
            continue;

        const char *close = strstr(fname_end, "-->");
        if (close)
            p = close + 3;
        else
            p = fname_end + 1;

        char incpath[768];
        struct stat incst;
        int found = 0;
        found = resolve_public_path(clean_inc, incpath, sizeof(incpath), &incst);

        if (found) {
            int inc_len = 0;
            char *inc_data = ssi_process_file(incpath, depth + 1, &inc_len);
            if (inc_data && inc_len > 0) {
                if (opos + (size_t)inc_len >= outsize) {
                    outsize = opos + (size_t)inc_len + 65536;
                    out = (char *)realloc(out, outsize);
                    if (!out) { free(raw); free(inc_data); return NULL; }
                }
                memcpy(out + opos, inc_data, (size_t)inc_len);
                opos += (size_t)inc_len;
                free(inc_data);
            }
        }

    }

    free(raw);
    *out_len = (int)opos;
    return out;
}

static char *http_build_blob_response(const char *status,
                                      const char *content_type,
                                      const char *cache_ctl,
                                      int close_after,
                                      const char *body,
                                      size_t body_len,
                                      size_t *out_len)
{
    char hdr[1024];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Server: WebSDR/%s\r\n"
        "Content-Length: %zu\r\n"
        "Content-Type: %s\r\n"
        "%s\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status, VERSION_STRING, body_len,
        content_type ? content_type : "application/octet-stream",
        cache_ctl ? cache_ctl : "Cache-control: no-cache",
        close_after ? "close" : "keep-alive");
    if (hlen <= 0) return NULL;
    char *resp = (char *)malloc((size_t)hlen + body_len);
    if (!resp) return NULL;
    memcpy(resp, hdr, (size_t)hlen);
    if (body_len > 0 && body)
        memcpy(resp + hlen, body, body_len);
    *out_len = (size_t)hlen + body_len;
    return resp;
}

static void http_async_build_result(const HttpAsyncJob *j, HttpAsyncResult *out)
{
    memset(out, 0, sizeof(*out));
    out->idx = j->idx;
    out->gen = j->gen;

    int close_after = (strstr(j->req, "Connection: close") != NULL ||
                       strstr(j->req, "connection: close") != NULL);
    out->close_after = close_after;

    char fpath[768];
    struct stat st;
    int found = 0;
    found = resolve_public_path(j->relpath, fpath, sizeof(fpath), &st);

    if (!found) {
        static const char not_found[] = "File not available.\r\n";
        out->resp = http_build_blob_response("404 Not Found", "text/html",
                                             "Cache-control: no-cache",
                                             close_after,
                                             not_found, sizeof(not_found) - 1,
                                             &out->resp_len);
        return;
    }

    const char *mime = j->mime[0] ? j->mime : "application/octet-stream";
    const char *cache_ctl = j->use_cache ? "Cache-control: max-age=3600"
                                         : "Cache-control: no-cache";

    if (j->is_html) {
        int body_len = 0;
        char *body = ssi_process_file(fpath, 0, &body_len);
        if (!body || body_len <= 0) {
            free(body);
            goto raw_file;
        }
        out->resp = http_build_blob_response("200 OK", "text/html", "Cache-control: no-cache",
                                             close_after, body, (size_t)body_len, &out->resp_len);
        free(body);
        return;
    }

raw_file:
    {
        int fd = open(fpath, O_RDONLY);
        if (fd < 0) {
            static const char not_found[] = "File not available.\r\n";
            out->resp = http_build_blob_response("404 Not Found", "text/html",
                                                 "Cache-control: no-cache",
                                                 close_after,
                                                 not_found, sizeof(not_found) - 1,
                                                 &out->resp_len);
            return;
        }
        off_t sz = st.st_size;
        if (sz < 0 || sz > (off_t)(32 * 1024 * 1024)) {
            close(fd);
            static const char too_big[] = "File too large.\r\n";
            out->resp = http_build_blob_response("413 Payload Too Large", "text/plain",
                                                 "Cache-control: no-cache",
                                                 1, too_big, sizeof(too_big) - 1,
                                                 &out->resp_len);
            out->close_after = 1;
            return;
        }
        char *body = (char *)malloc((size_t)sz);
        if (!body) {
            close(fd);
            return;
        }
        size_t off = 0;
        while (off < (size_t)sz) {
            ssize_t r = read(fd, body + off, (size_t)sz - off);
            if (r <= 0) break;
            off += (size_t)r;
        }
        close(fd);
        if (off != (size_t)sz) {
            free(body);
            return;
        }
        out->resp = http_build_blob_response("200 OK", mime, cache_ctl,
                                             close_after, body, (size_t)sz, &out->resp_len);
        free(body);
    }
}

static void *http_async_thread_body(void *unused)
{
    (void)unused;
    for (;;) {
        HttpAsyncJob job;
        pthread_mutex_lock(&http_async_mu);
        while (http_async_q_len == 0)
            pthread_cond_wait(&http_async_cv, &http_async_mu);
        job = http_async_q[http_async_q_head];
        http_async_q_head = (http_async_q_head + 1) % HTTP_ASYNC_QMAX;
        http_async_q_len--;
        pthread_mutex_unlock(&http_async_mu);

        HttpAsyncResult res;
        http_async_build_result(&job, &res);

        pthread_mutex_lock(&http_async_mu);
        if (http_async_r_len < HTTP_ASYNC_QMAX) {
            http_async_r[http_async_r_tail] = res;
            http_async_r_tail = (http_async_r_tail + 1) % HTTP_ASYNC_QMAX;
            http_async_r_len++;
        } else {
            free(res.resp);
        }
        pthread_mutex_unlock(&http_async_mu);
    }
    return NULL;
}

static void http_async_init(void)
{
    static int inited = 0;
    if (inited) return;
    inited = 1;
    for (int i = 0; i < HTTP_ASYNC_THREADS; i++) {
        int rc = pthread_create(&http_async_tid[i], NULL, http_async_thread_body, NULL);
        if (rc == 0) {
            pthread_detach(http_async_tid[i]);
        } else {
            log_printf("http_async: pthread_create failed (%d) worker=%d\n", rc, i);
        }
    }
}

static int http_async_enqueue(int idx, const char *req, const char *relpath,
                              const char *mime, int use_cache, int is_html)
{
    HttpAsyncJob j;
    memset(&j, 0, sizeof(j));
    j.idx = idx;
    j.gen = hp_gen[idx];
    j.use_cache = use_cache ? 1 : 0;
    j.is_html = is_html ? 1 : 0;
    snprintf(j.req, sizeof(j.req), "%s", req ? req : "");
    snprintf(j.relpath, sizeof(j.relpath), "%s", relpath ? relpath : "");
    if (mime)
        snprintf(j.mime, sizeof(j.mime), "%s", mime);

    pthread_mutex_lock(&http_async_mu);
    if (http_async_q_len >= HTTP_ASYNC_QMAX) {
        pthread_mutex_unlock(&http_async_mu);
        return -1;
    }
    http_async_q[http_async_q_tail] = j;
    http_async_q_tail = (http_async_q_tail + 1) % HTTP_ASYNC_QMAX;
    http_async_q_len++;
    pthread_cond_signal(&http_async_cv);
    pthread_mutex_unlock(&http_async_mu);
    return 0;
}

static void http_async_pump_results(void)
{
    for (;;) {
        HttpAsyncResult r;
        int have = 0;
        pthread_mutex_lock(&http_async_mu);
        if (http_async_r_len > 0) {
            r = http_async_r[http_async_r_head];
            http_async_r_head = (http_async_r_head + 1) % HTTP_ASYNC_QMAX;
            http_async_r_len--;
            have = 1;
        }
        pthread_mutex_unlock(&http_async_mu);
        if (!have) break;

        if (r.idx < 0 || r.idx >= HP_MAX || !r.resp) {
            free(r.resp);
            continue;
        }
        HttpConn *c = &hp[r.idx];
        if (!c->state || hp_gen[r.idx] != r.gen || c->ws_mode) {
            free(r.resp);
            continue;
        }
        if (c->async_resp) {
            free(c->async_resp);
            c->async_resp = NULL;
        }
        c->async_resp = r.resp;
        c->async_resp_len = r.resp_len;
        c->async_resp_off = 0;
        c->async_close_after = r.close_after;
        c->async_state = 2;
    }
}

static void http_handle_request(int idx)
{
    HttpConn *c = &hp[idx];
    const char *req = c->buf;

    int is_post = 0;
    const char *path_start;
    if (strncmp(req, "GET ", 4) == 0) {
        path_start = req + 4;
    } else if (strncmp(req, "POST ", 5) == 0) {
        path_start = req + 5;
        is_post = 1;
    } else {
        http_send_response(idx, "501 Not Implemented", "");
        hp_close(idx);
        return;
    }

    char path[512];
    int pi = 0;
    for (const char *p = path_start; *p && *p != ' ' && *p != '?'; p++) {
        if (pi < (int)sizeof(path) - 1) path[pi++] = *p;
    }
    path[pi] = '\0';

    if (strcmp(path, "/") == 0 || path[0] == '\0')
        strncpy(path, "/index.html", sizeof(path) - 1);

    if (strncmp(path, "/~~stream",        8)  == 0) { handle_tilde_stream(idx);        return; }
    if (strncmp(path, "/~~waterstream",   14) == 0) { handle_tilde_waterstream(idx);   return; }
    if (strncmp(path, "/~~iqbalance",     12) == 0) { handle_tilde_iqbalance(idx);     return; }
    if (strncmp(path, "/~~raw",            6) == 0) { handle_tilde_raw(idx);           return; }
    if (strncmp(path, "/~~status",         9) == 0) { handle_tilde_status(idx);        return; }
    if (strncmp(path, "/~~waterfalltext", 16) == 0) { handle_tilde_waterfalltext(idx); return; }
    if (strncmp(path, "/~~orgstatus",     12) == 0) { handle_tilde_orgstatus(idx);     return; }
    if (strncmp(path, "/~~configreload",  15) == 0) { handle_tilde_configreload(idx);  return; }
    if (strncmp(path, "/~~chat",           7) == 0 &&
        (path[7] == '\0' || path[7] == '?' || path[7] == ' ')) { handle_tilde_chat(idx, is_post); return; }
    if (strncmp(path, "/~~chatidentities",17) == 0) { handle_tilde_chatidentities(idx); return; }
    if (strncmp(path, "/~~chatcensor",    13) == 0) { handle_tilde_chatcensor(idx);     return; }
    if (strncmp(path, "/~~logbook",       10) == 0) { handle_tilde_logbook(idx);        return; }
    if (strncmp(path, "/~~loginsert",     12) == 0) { handle_tilde_loginsert(idx);      return; }
    if (strncmp(path, "/~~log?",           6) == 0) { handle_tilde_loginsert(idx);      return; }
    if (strncmp(path, "/~~otherstable",   14) == 0) { handle_tilde_otherstable(idx);    return; }
    if (strncmp(path, "/~~othersjj",      11) == 0) { handle_tilde_othersjj(idx);       return; }
    if (strncmp(path, "/~~othersj",       10) == 0) { handle_tilde_othersj(idx);        return; }
    if (strncmp(path, "/~~histogram",     12) == 0) { handle_tilde_histogram(idx);      return; }
    if (strncmp(path, "/~~fetchdx",       10) == 0) { handle_tilde_fetchdx(idx);         return; }
    if (strncmp(path, "/~~setconfig",     12) == 0) { handle_tilde_setconfig(idx);      return; }
    if (strncmp(path, "/~~setdir",         9) == 0) { handle_tilde_setdir(idx);         return; }
    if (strncmp(path, "/~~blockmee",      11) == 0) { handle_tilde_blockmee(idx);       return; }

    const char *relpath = (path[0] == '/') ? path + 1 : path;
    char safe_relpath[512];
    if (!sanitize_relative_path(relpath, safe_relpath, sizeof(safe_relpath))) {
        http_send_response(idx, "404 Not Found", "File not available.\n");
        hp_close(idx);
        return;
    }

    const char *dot   = strrchr(safe_relpath, '.');
    const char *slash = strrchr(safe_relpath, '/');
    if (dot && slash && dot < slash) dot = NULL;

    const char *mime = mime_for_ext(dot);

    int use_cache = (dot
                     && strcmp(dot, ".js")   != 0
                     && strcmp(dot, ".html") != 0
                     && strcmp(dot, ".htm")  != 0) ? 1 : 0;

    int is_html = (mime && strcmp(mime, "text/html") == 0);
    if (http_async_enqueue(idx, c->buf, safe_relpath, mime, use_cache, is_html) != 0) {
        http_send_response(idx, "503 Service Unavailable",
                           "Server busy: async HTTP queue full.\n");
        hp_close(idx);
        return;
    }
    c->async_state = 1;
}

static void ws_upgrade(int idx, const char *key)
{
    HttpConn *c = &hp[idx];

    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", key, WS_MAGIC);
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((unsigned char *)combined, strlen(combined), hash);

    char b64[64];
    EVP_EncodeBlock((unsigned char *)b64, hash, SHA_DIGEST_LENGTH);

    char resp[256];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", b64);

    if (socket_write_all(c->fd, resp, (size_t)rlen) != 0) {
        hp_close_write_fail(idx);
        return;
    }

    log_printf("fd=%i -%s-: upgraded HTTP connection to WebSocket\n", c->fd, c->ip_str);

    c->ws_mode = 1;
    c->buf_len = 0;

    pthread_mutex_lock(&worker_dsp_mutex);
    struct Client *cl = client_assign_fd(c->fd, c->ip_str);
    if (cl) {
        c->ws_ptr = cl;
        pthread_mutex_unlock(&worker_dsp_mutex);
    } else {
        pthread_mutex_unlock(&worker_dsp_mutex);
        log_printf("fd=%i -%s-: no free client slots are available\n", c->fd, c->ip_str);
        hp_close(idx);
    }
}

static void hp_process(int idx)
{
    HttpConn *c = &hp[idx];
    c->last_ts = time(NULL);

    char *ws_key = strstr(c->buf, "Sec-WebSocket-Key: ");
    if (ws_key) {
        ws_key += 19;
        char *eol = strchr(ws_key, '\r');
        if (!eol) eol = strchr(ws_key, '\n');
        if (eol) {
            *eol = '\0';
            if (!strstr(c->buf, "Host:")) {
                log_printf("fd=%i -%s-: WebSocket upgrade rejected because the Host header is missing\n",
                           c->fd, c->ip_str);
                hp_close(idx);
                return;
            }

            char saved_path[512] = "";
            const char *ps = c->buf;
            if (strncmp(ps, "GET ", 4) == 0) ps += 4;
            int pi = 0;

            while (*ps && *ps != ' ' && pi < (int)sizeof(saved_path) - 1)
                saved_path[pi++] = *ps++;
            saved_path[pi] = '\0';

            ws_upgrade(idx, ws_key);
            if (!hp[idx].state) return;

            snprintf(hp[idx].buf, sizeof(hp[idx].buf), "GET %s HTTP/1.1\r\n\r\n", saved_path);

            if (strncmp(saved_path, "/~~waterstream", 14) == 0)
                handle_tilde_waterstream(idx);
            else if (strncmp(saved_path, "/~~stream", 9) == 0)
                handle_tilde_stream(idx);
            else if (strncmp(saved_path, "/~~iqbalance", 12) == 0)
                handle_tilde_iqbalance(idx);
            else if (strncmp(saved_path, "/~~raw", 6) == 0)
                handle_tilde_raw(idx);

            return;
        }
    }

    http_handle_request(idx);
}

void init_network(int port)
{
    memset(hp, 0, sizeof(hp));
    memset(hp_gen, 0, sizeof(hp_gen));
    g_stats.last_rc6_ms = read_rc6_residency_ms();
    http_async_init();

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) { perror("epoll_create1"); exit(1); }

    listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        if (errno == EAFNOSUPPORT)
            fprintf(stderr, "IPv6 sockets are not supported by this kernel.\n");
        else
            fprintf(stderr, "Listen socket creation failed: %s\n", strerror(errno));
        exit(1);
    }

    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL, 0) | O_NONBLOCK);

    int optval = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = htons((uint16_t)port);

    if (config.myhost[0]) {
        struct hostent *he = gethostbyname2(config.myhost, AF_INET6);
        if (!he) {
            fprintf(stderr, "Host '%s' could not be resolved; binding to all interfaces instead.\n",
                    config.myhost);
            addr.sin6_addr = in6addr_any;
        } else {
            memcpy(&addr.sin6_addr, he->h_addr_list[0], (size_t)he->h_length);
        }
    } else {
        addr.sin6_addr = in6addr_any;
    }

    uid_t uid = getuid();
    if (seteuid(0) != 0 && port < 1024) {
        fprintf(stderr, "Temporary privilege elevation for privileged port %d failed: %s\n",
                port, strerror(errno));
    }
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (seteuid(uid) != 0) {}
        if (errno == EACCES && port < 1024)
            fprintf(stderr, "Bind to port %i failed: root privileges are required for ports below 1024.\n", port);
        else if (errno == EADDRINUSE)
            fprintf(stderr, "Bind to port %i failed: the address is already in use.\n", port);
        else
            fprintf(stderr, "Bind to port %i failed: %s\n", port, strerror(errno));
        exit(1);
    }
    if (seteuid(uid) != 0) {
        fprintf(stderr, "Privilege drop after bind failed: %s\n", strerror(errno));
        exit(1);
    }

    if (listen(listen_fd, SOMAXCONN) != 0) {
        fprintf(stderr, "listen() failed on port %d: %s\n", port, strerror(errno));
        exit(1);
    }

    struct epoll_event ev;
    ev.events    = EPOLLIN;
    ev.data.u64  = EPOLL_TAG_LISTEN;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        fprintf(stderr, "epoll_ctl(ADD listen_fd) failed: %s\n", strerror(errno));
        exit(1);
    }

    log_printf("Network listener is ready on port %d.\n", port);
}

void network_add_alsa_fd(int alsa_fd, int band_idx)
{
    struct epoll_event ev;
    ev.events   = EPOLLIN;
    ev.data.u64 = EPOLL_TAG_ALSA + (uint64_t)band_idx;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, alsa_fd, &ev) == -1) {
        if (errno == EEXIST) {
            if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, alsa_fd, &ev) == -1) {
                log_printf("epoll add/mod failed for audio fd=%d band=%d: %s\n",
                           alsa_fd, band_idx, strerror(errno));
            }
        } else {
            log_printf("epoll add failed for audio fd=%d band=%d: %s\n",
                       alsa_fd, band_idx, strerror(errno));
        }
    }
}

void network_add_stdin_fd(int fd, int band_idx)
{
    struct epoll_event ev;
    ev.events   = EPOLLIN | EPOLLET;
    ev.data.u64 = EPOLL_TAG_ALSA + (uint64_t)band_idx;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        if (errno == EEXIST) {
            if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == -1) {
                log_printf("epoll add/mod failed for stdin fd=%d band=%d: %s\n",
                           fd, band_idx, strerror(errno));
            }
        } else {
            log_printf("epoll add failed for stdin fd=%d band=%d: %s\n",
                       fd, band_idx, strerror(errno));
        }
    }
}

void network_flush_ws_clients(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct Client *c = &clients[i];
        if (c->state == CLIENT_WEBSOCKET && c->fd >= 0 && c->outbuf_len > c->outbuf_sent)
            client_flush_outbuf(c);
    }
}

static time_t last_timeout_sweep = 0;

void process_network_events(void)
{
    struct epoll_event events[MAX_EVENTS];
    int nev = epoll_wait(epoll_fd, events, MAX_EVENTS, 1);
    if (nev < 0) return;
    http_async_pump_results();

    {
        struct timeval _tv;
        gettimeofday(&_tv, NULL);
        long long _now_us = (long long)_tv.tv_sec * 1000000LL + _tv.tv_usec;
        if (g_stats.next_update_us == 0 || _now_us >= g_stats.next_update_us)
            stats_update();
    }

    time_t now = time(NULL);
    if (now - last_timeout_sweep > 10) {
        last_timeout_sweep = now;
        for (int j = 0; j < HP_MAX; j++) {
            HttpConn *hc = &hp[j];
            if (!hc->state) continue;

            if (hc->ws_ptr || hc->raw_ptr) continue;

            if (HTTP_HEADER_TIMEOUT_SEC > 0 &&
                hc->buf_len > 0 &&
                now - hc->created_ts > HTTP_HEADER_TIMEOUT_SEC) {
                hp_close(j);
                continue;
            }
            if (now - hc->last_ts > HTTP_IDLE_TIMEOUT_SEC) {
                hp_close(j);
            }
        }
    }

    int need_ws_flush = 0;
    pthread_mutex_lock(&worker_dsp_mutex);
    for (int i = 0; i < nev; i++) {
        uint64_t tag = events[i].data.u64;
        if ((tag & EPOLL_TAG_MASK) != EPOLL_TAG_ALSA) continue;

        int bidx = (int)(tag & 0xFFFFFFFFu);
        if (bidx >= 0 && bidx < config.num_bands && bands[bidx].started) {
            struct BandState *b = &bands[bidx];

            if (b->device_type == DEVTYPE_STDIN)
                continue;
            int got;
            if (b->device_type == DEVTYPE_RTLSDR || b->device_type == DEVTYPE_TCPSDR)
                got = rtltcp_read_samples(b);
            else
                got = alsa_read_samples(b);
            if (got > 0)
                need_ws_flush = 1;
        }
    }
    if (need_ws_flush)
        network_flush_ws_clients();
    pthread_mutex_unlock(&worker_dsp_mutex);

    int async_scan_budget = 64;
    for (int j = 0; j < HP_MAX && async_scan_budget > 0; j++) {
        if (!hp[j].state || hp[j].async_state != 2)
            continue;
        hp_try_flush_async(j);
        async_scan_budget--;
    }

    int accept_budget = NET_ACCEPT_BUDGET;
    int http_budget = NET_HTTP_IO_BUDGET;
    int ws_budget = NET_WS_IO_BUDGET;
    long long http_slice_start_us = mono_us();
    for (int i = 0; i < nev; i++) {
        uint64_t tag = events[i].data.u64;
        if ((tag & EPOLL_TAG_MASK) == EPOLL_TAG_ALSA)
            continue;

        if ((tag & EPOLL_TAG_MASK) == EPOLL_TAG_LISTEN) {
            if (accept_budget <= 0)
                continue;
            for (;;) {
                if (accept_budget <= 0)
                    break;
                struct sockaddr_storage peer;
                socklen_t plen = sizeof(peer);
                int cfd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
                if (cfd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    fprintf(stderr, "accept() failed on the listening socket: %s\n", strerror(errno));
                    break;
                }
                accept_budget--;

                fcntl(cfd, F_SETFL, O_NONBLOCK);
                fcntl(cfd, F_SETFD, FD_CLOEXEC);
                set_tcp_options(cfd);

                char ip[72] = "";
                if (peer.ss_family == AF_INET6) {
                    struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&peer;
                    inet_ntop(AF_INET6, &s6->sin6_addr, ip, sizeof(ip));
                } else if (peer.ss_family == AF_INET) {
                    struct sockaddr_in *s4 = (struct sockaddr_in *)&peer;
                    inet_ntop(AF_INET, &s4->sin_addr, ip, sizeof(ip));
                }

                if (HTTP_MAX_CONN_PER_IP > 0 &&
                    count_http_slots_for_ip(ip) >= HTTP_MAX_CONN_PER_IP) {
                    log_printf("fd=%i -%s-: rejected HTTP connection because the per-IP limit is %d\n",
                               cfd, ip, HTTP_MAX_CONN_PER_IP);
                    close(cfd);
                    continue;
                }

                for (int j = 0; j < HP_MAX; j++) {
                    if (hp[j].state && hp[j].fd == cfd) {
                        log_printf("fd=%i -%s-: closing stale HTTP slot %i that still owns this fd (%s)\n",
                                   cfd, ip, j, hp[j].ip_str);
                        hp_close(j);
                        break;
                    }
                }

                int slot = -1;
                for (int j = 0; j < HP_MAX; j++) {
                    if (!hp[j].state) { slot = j; break; }
                }

                if (slot == -1) {
                    log_printf("fd=%i -%s-: HTTP slot pool is full; returning 500\n",
                               cfd, ip);
                    if (write(cfd,
                        "HTTP/1.1 500 Internal Server Error\r\n"
                        "Content-Length: 66\r\n"
                        "Content-Type: text/html\r\n"
                        "Cache-control: no-cache\r\n"
                        "\r\n"
                        "Server busy: too many simultaneous connections. Try again later.\r\n",
                        174) < 0) {}
                    close(cfd);
                    continue;
                }

                memset(&hp[slot], 0, sizeof(HttpConn));
                hp_gen[slot]++;
                hp[slot].state   = 1;
                hp[slot].fd      = cfd;
                hp[slot].last_ts = time(NULL);
                hp[slot].created_ts = hp[slot].last_ts;
                snprintf(hp[slot].ip_str, sizeof(hp[slot].ip_str), "%s", ip);

                struct epoll_event ev;
                ev.events   = EPOLLIN;
                ev.data.u64 = hp_encode(slot);
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cfd, &ev);

            }
            continue;
        }

        if ((tag & EPOLL_TAG_MASK) != EPOLL_TAG_HTTP) continue;
        int slot = hp_decode(tag);
        if (slot < 0 || slot >= HP_MAX || !hp[slot].state) continue;

        HttpConn *c = &hp[slot];

        if (c->async_state == 2 && c->async_resp) {
            hp_try_flush_async(slot);
            continue;
        }
        if (c->async_state == 1) {

            continue;
        }

        if (c->ws_mode) {
            if (ws_budget <= 0)
                continue;
            ws_budget--;

            uint8_t tmp[8192];
            ssize_t n = read(c->fd, tmp, sizeof(tmp));
            if (n > 0) {
                if (c->ws_ptr) {
                    struct Client *cl = (struct Client *)c->ws_ptr;
                    pthread_mutex_lock(&worker_dsp_mutex);
                    client_ws_recv(cl, tmp, (int)n);

                    int closed = (cl->state != CLIENT_WEBSOCKET || cl->fd < 0);
                    pthread_mutex_unlock(&worker_dsp_mutex);
                    if (closed) {
                        hp_close(slot);
                        continue;
                    }
                }
            } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                hp_close(slot);
            }
            continue;
        }

        if (http_budget <= 0)
            continue;
        if ((mono_us() - http_slice_start_us) >= NET_HTTP_TIMESLICE_US)
            continue;
        http_budget--;

        if (c->buf_len == 0)
            c->created_ts = time(NULL);

        int room = HP_BUF_SIZE - c->buf_len - 1;
        if (room <= 0) {

            log_printf("fd=%i -%s-: HTTP header buffer overflow; closing connection\n", c->fd, c->ip_str);
            hp_close(slot);
            continue;
        }

        ssize_t n = read(c->fd, c->buf + c->buf_len, (size_t)room);
        if (n > 0) {
            c->buf_len += (int)n;
            c->buf[c->buf_len] = '\0';
            c->last_ts = time(NULL);

            if (strstr(c->buf, "\r\n\r\n")) {
                hp_process(slot);
            }
        } else {

            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            log_printf("fd=%i -%s-: peer disconnected the socket\n", c->fd, c->ip_str);
            hp_close(slot);
        }
    }
}

void *net_get_conn(int slot)
{
    if (slot < 0 || slot >= HP_MAX) return NULL;
    return &hp[slot];
}

int net_conn_fd(int slot)
{
    if (slot < 0 || slot >= HP_MAX || !hp[slot].state) return -1;
    return hp[slot].fd;
}

int net_num_slots(void) { return HP_MAX; }
