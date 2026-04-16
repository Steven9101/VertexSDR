// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#include "orgserver.h"
#include "config.h"
#include "band.h"
#include "client.h"
#include "common.h"
#include "logging.h"
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>

static volatile int org_state = 0;
static char org_host[128] = "websdr.ewi.utwente.nl";
static char org_port[16] = "80";

enum {
    ORG_TIMEOUT_SEC = 10,
    SDRLIST_UPDATE_INTERVAL_SEC = 60,
    SDRLIST_BACKOFF_BASE_SEC = 30,
    SDRLIST_BACKOFF_MAX_SEC = 60 * 60,
    SDRLIST_RESP_MAX = 8192
};

static const char sdrlist_host[] = "sdr-list.xyz";
static const char sdrlist_path[] = "/api/update_websdr";

static void orgserver_parse_target(void)
{
    if (!g_config.orgserver[0]) {
        strncpy(org_host, "websdr.ewi.utwente.nl", sizeof(org_host) - 1);
        org_host[sizeof(org_host) - 1] = '\0';
        strncpy(org_port, "80", sizeof(org_port) - 1);
        org_port[sizeof(org_port) - 1] = '\0';
        return;
    }

    char host[128] = {0};
    int port = 80;
    if (sscanf(g_config.orgserver, " %127s %d", host, &port) >= 1) {
        strncpy(org_host, host, sizeof(org_host) - 1);
        org_host[sizeof(org_host) - 1] = '\0';
        if (port > 0 && port <= 65535)
            snprintf(org_port, sizeof(org_port), "%d", port);
        else
            strncpy(org_port, "80", sizeof(org_port) - 1);
        org_port[sizeof(org_port) - 1] = '\0';
    }
}

extern struct WebSDRConfig g_config;

static int tcp_connect_host(const char *host, const char *port, int timeout_sec)
{
    struct addrinfo hints, *res, *rp;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        return -1;
    }

    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = {timeout_sec, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int orgserver_connect(void)
{
    return tcp_connect_host(org_host, org_port, ORG_TIMEOUT_SEC);
}

static int ssl_write_all(SSL *ssl, const char *buf, int len)
{
    int off = 0;
    while (off < len) {
        int n = SSL_write(ssl, buf + off, len - off);
        if (n <= 0)
            return 0;
        off += n;
    }
    return 1;
}

static int http_status_success(const char *resp)
{
    const char *p = strchr(resp, ' ');
    if (!p) return 0;
    while (*p == ' ')
        p++;
    if (p[0] < '2' || p[0] > '2')
        return 0;
    if (p[1] < '0' || p[1] > '9' || p[2] < '0' || p[2] > '9')
        return 0;
    return 1;
}

static const char *http_body_ptr(const char *resp)
{
    const char *body = strstr(resp, "\r\n\r\n");
    if (!body) return "";
    return body + 4;
}

static int https_post_json(const char *host, const char *path, const char *json,
                           char *resp, size_t resp_cap)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx)
        return 0;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        SSL_CTX_free(ctx);
        return 0;
    }

    int fd = tcp_connect_host(host, "443", ORG_TIMEOUT_SEC);
    if (fd < 0) {
        SSL_CTX_free(ctx);
        return 0;
    }

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        close(fd);
        SSL_CTX_free(ctx);
        return 0;
    }
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host);
    SSL_set1_host(ssl, host);

    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl);
        close(fd);
        SSL_CTX_free(ctx);
        return 0;
    }

    char req[8192];
    int nreq = snprintf(req, sizeof(req),
                        "POST %s HTTP/1.1\r\n"
                        "Host: %s\r\n"
                        "User-Agent: VertexSDR/registration (+https://github.com/Steven9101/VertexSDR)\r\n"
                        "Accept: application/json\r\n"
                        "Content-Type: application/json\r\n"
                        "Connection: close\r\n"
                        "Content-Length: %zu\r\n\r\n"
                        "%s",
                        path, host, strlen(json), json);
    if (nreq <= 0 || nreq >= (int)sizeof(req) || !ssl_write_all(ssl, req, nreq)) {
        SSL_free(ssl);
        close(fd);
        SSL_CTX_free(ctx);
        return 0;
    }

    size_t total = 0;
    while (total + 1 < resp_cap) {
        int nr = SSL_read(ssl, resp + total, (int)(resp_cap - total - 1));
        if (nr <= 0)
            break;
        total += (size_t)nr;
    }
    resp[total] = '\0';

    SSL_free(ssl);
    close(fd);
    SSL_CTX_free(ctx);

    return total > 0;
}

static void orginfo_lookup(const char *label, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    out[0] = '\0';

    size_t label_len = strlen(label);
    const char *p = config.org_info;
    while (p && *p) {
        const char *line_end = strchr(p, '\n');
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        if (line_len > label_len + 1 &&
            strncasecmp(p, label, label_len) == 0 &&
            p[label_len] == ':') {
            const char *val = p + label_len + 1;
            while (*val == ' ' || *val == '\t')
                val++;
            size_t val_len = line_len - (size_t)(val - p);
            if (val_len >= out_size)
                val_len = out_size - 1;
            memcpy(out, val, val_len);
            out[val_len] = '\0';
            return;
        }
        p = line_end ? (line_end + 1) : NULL;
    }
}

static void json_escape_string(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    if (!dst || dst_size == 0)
        return;
    if (!src) src = "";

    for (size_t i = 0; src[i] && j + 1 < dst_size; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '\\' || ch == '"') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = (char)ch;
        } else if (ch == '\n') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (ch == '\r') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = 'r';
        } else if (ch == '\t') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = 't';
        } else if (ch < 32) {
            if (j + 6 >= dst_size) break;
            j += (size_t)snprintf(dst + j, dst_size - j, "\\u%04x", ch);
        } else {
            dst[j++] = (char)ch;
        }
    }
    dst[j] = '\0';
}

static unsigned int sdrlist_receiver_count(void)
{
    unsigned int count = 0;
    for (int i = 0; i < config.num_bands; i++) {
        if (bands[i].samplerate > 0 || config.bands[i].samplerate > 0)
            count++;
    }
    return count ? count : 1U;
}

static int sdrlist_band_user_count(int band_idx)
{
    int users = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        const struct Client *cl = &clients[i];
        if (cl->state != CLIENT_WEBSOCKET)
            continue;
        if (cl->band_idx != band_idx)
            continue;
        if (cl->audio_format <= 0 || cl->audio_format == 36)
            continue;
        users++;
    }
    return users;
}

static unsigned int sdrlist_backoff_seconds(unsigned int attempt)
{
    unsigned int shift = (attempt > 16U) ? 16U : attempt;
    unsigned long long secs = (unsigned long long)SDRLIST_BACKOFF_BASE_SEC << shift;
    if (secs > (unsigned long long)SDRLIST_BACKOFF_MAX_SEC)
        secs = SDRLIST_BACKOFF_MAX_SEC;
    return (unsigned int)secs;
}

static int sdrlist_build_payload(int band_idx, const char *instance_id,
                                 char *json, size_t json_size)
{
    char hostname[128];
    char qth[128];
    char name[512];
    char antenna[128];
    char receiver_id[64];
    char backend[32];
    char esc_hostname[256];
    char esc_qth[256];
    char esc_name[1024];
    char esc_antenna[256];
    char esc_receiver_id[128];

    if (band_idx < 0 || band_idx >= config.num_bands)
        return 0;

    snprintf(hostname, sizeof(hostname), "%s",
             config.hostname[0] ? config.hostname :
             (config.myhost[0] ? config.myhost : ""));
    orginfo_lookup("Qth", qth, sizeof(qth));
    orginfo_lookup("Description", name, sizeof(name));

    if (!name[0])
        snprintf(name, sizeof(name), "%s", hostname[0] ? hostname : bands[band_idx].name);

    snprintf(antenna, sizeof(antenna), "%s",
             config.bands[band_idx].antenna[0] ? config.bands[band_idx].antenna :
             (bands[band_idx].name[0] ? bands[band_idx].name : "unknown"));
    snprintf(receiver_id, sizeof(receiver_id), "band-%d-%s",
             band_idx, bands[band_idx].name[0] ? bands[band_idx].name : "rx");
    snprintf(backend, sizeof(backend), "%s",
             (config.fftbackend == FFT_BACKEND_VKFFT) ? "vkfft" : "fftw");

    json_escape_string(hostname, esc_hostname, sizeof(esc_hostname));
    json_escape_string(qth, esc_qth, sizeof(esc_qth));
    json_escape_string(name, esc_name, sizeof(esc_name));
    json_escape_string(antenna, esc_antenna, sizeof(esc_antenna));
    json_escape_string(receiver_id, esc_receiver_id, sizeof(esc_receiver_id));

    long long bandwidth = (long long)((bands[band_idx].samplerate > 0)
        ? bands[band_idx].samplerate
        : config.bands[band_idx].samplerate);
    if (bandwidth < 0)
        bandwidth = 0;

    long long range_start_hz = (long long)(bands[band_idx].centerfreq_khz * 1000.0) - bandwidth / 2;
    if (range_start_hz < 0)
        range_start_hz = 0;
    long long range_end_hz = range_start_hz + bandwidth;
    long long center_frequency = range_start_hz + bandwidth / 2;
    int users = sdrlist_band_user_count(band_idx);
    if (users < 0)
        users = 0;

    if (!esc_hostname[0]) {
        log_printf("sdr-list: skipping band %d registration because hostname is empty.\n", band_idx);
        return 0;
    }

    int n = snprintf(json, json_size,
                     "{"
                     "\"id\":\"%s\","
                     "\"name\":\"%s\","
                     "\"antenna\":\"%s\","
                     "\"bandwidth\":%lld,"
                     "\"users\":%d,"
                     "\"center_frequency\":%lld,"
                     "\"grid_locator\":\"%s\","
                     "\"hostname\":\"%s\","
                     "\"max_users\":%d,"
                     "\"port\":%d,"
                     "\"software\":\"VertexSDR\","
                     "\"backend\":\"%s\","
                     "\"version\":\"%s\","
                     "\"receiver_count\":%u,"
                     "\"receiver_id\":\"%s\","
                     "\"range_start_hz\":%lld,"
                     "\"range_end_hz\":%lld"
                     "}",
                     instance_id, esc_name, esc_antenna, bandwidth, users,
                     center_frequency, esc_qth, esc_hostname, config.max_clients,
                     config.tcp_port, backend, VERSION_STRING,
                     sdrlist_receiver_count(), esc_receiver_id,
                     range_start_hz, range_end_hz);
    return n > 0 && (size_t)n < json_size;
}

static int sdrlist_send_updates(const char *instance_id)
{
    char json[4096];
    char resp[SDRLIST_RESP_MAX];

    for (int i = 0; i < config.num_bands; i++) {
        if (!sdrlist_build_payload(i, instance_id, json, sizeof(json)))
            return 0;
        if (!https_post_json(sdrlist_host, sdrlist_path, json, resp, sizeof(resp))) {
            log_printf("sdr-list: HTTPS POST failed for band %d.\n", i);
            return 0;
        }
        if (!http_status_success(resp)) {
            const char *body = http_body_ptr(resp);
            log_printf("sdr-list: registration failed for band %d: %.120s\n", i, body);
            return 0;
        }
    }
    return 1;
}

static void sdrlist_maybe_update(const char *instance_id, time_t *next_attempt,
                                 unsigned int *attempt)
{
    time_t now = time(NULL);
    if (!next_attempt || !attempt)
        return;
    if (*next_attempt != 0 && now < *next_attempt)
        return;

    if (sdrlist_send_updates(instance_id)) {
        *attempt = 0;
        *next_attempt = now + SDRLIST_UPDATE_INTERVAL_SEC;
    } else {
        *attempt += 1;
        *next_attempt = now + (time_t)sdrlist_backoff_seconds(*attempt);
    }
}

static void *orgserver_thread(void *arg)
{
    (void)arg;

    int fd = -1;
    time_t sdrlist_next_attempt = 0;
    unsigned int sdrlist_attempt = 0;
    char sdrlist_instance_id[32];

    orgserver_parse_target();
    org_state = g_config.noorgserver ? 1 : 0;
    snprintf(sdrlist_instance_id, sizeof(sdrlist_instance_id), "%u",
             (unsigned int)((unsigned int)time(NULL) ^ (unsigned int)getpid()));

    for (;;) {
        int st = org_state;

        while (st > 0) {
            if (fd > 0) {
                close(fd);
                fd = -1;
                st = org_state;
            }

            while (1) {
                int wait_s = (st == 1) ? 900 : 86400;
                for (int j = 0; j < wait_s; j++) {
                    sleep(1);
                    st = org_state;
                }
                if (st <= 0)
                    break;

                int nfd = orgserver_connect();
                if (nfd > 0) {
                    char notreq[256];
                    snprintf(notreq, sizeof(notreq),
                             "GET /~~websdrNOorg?port=%i HTTP/1.1\r\n\r\n",
                             g_config.tcp_port);
                    write(nfd, notreq, strlen(notreq));
                    close(nfd);
                }
                org_state = 2;
                st = 2;
            }
        }

        if (fd < 0) {
            if (org_port[0] == '\0')
                goto sleep_cycle;
            fd = orgserver_connect();
        }

        if (fd > 0) {
            char req[1064];
            if (g_config.hostname[0]) {
                snprintf(req, sizeof(req),
                         "GET /~~websdrorg?host=%s&port=%i HTTP/1.1\r\n\r\n",
                         g_config.hostname, g_config.tcp_port);
            } else {
                snprintf(req, sizeof(req),
                         "GET /~~websdrorg?port=%i HTTP/1.1\r\n\r\n",
                         g_config.tcp_port);
            }

            if (write(fd, req, strlen(req)) < (ssize_t)strlen(req)) {
                close(fd);
                fd = -1;
            }

            for (int j = 0; j < 10; j++) {
                sleep(1);
                sdrlist_maybe_update(sdrlist_instance_id, &sdrlist_next_attempt, &sdrlist_attempt);
            }

            if (fd > 0) {
                char resp[0x400];
                if (recv(fd, resp, sizeof(resp), 64) <= 0) {
                    close(fd);
                    fd = -1;
                }
            }
        }

        sdrlist_maybe_update(sdrlist_instance_id, &sdrlist_next_attempt, &sdrlist_attempt);

sleep_cycle:

        for (int k = 0; k < 50; k++) {
            sleep(1);
            sdrlist_maybe_update(sdrlist_instance_id, &sdrlist_next_attempt, &sdrlist_attempt);
        }
    }
    return NULL;
}

void orgserver_start(void)
{
    pthread_t tid;
    int rc = pthread_create(&tid, NULL, orgserver_thread, NULL);
    if (rc != 0)
        perror("pthread_create orgserver");
    pthread_detach(tid);
}
