// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#include "chat.h"
#include "client.h"
#include "config.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *mask_chat_ip(const char *ip)
{
    static char masked[64];
    if (!ip || !ip[0]) return "?";
    if (strncmp(ip, "::ffff:", 7) == 0)
        ip += 7;
    if (strchr(ip, '.')) {
        unsigned a, b, c, d;
        if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            snprintf(masked, sizeof(masked), "%u.%u.x.x", a, b);
            return masked;
        }
    }
    if (strchr(ip, ':')) {
        snprintf(masked, sizeof(masked), "%s", ip);
        char *last = strrchr(masked, ':');
        if (last) {
            *last = '\0';
            last = strrchr(masked, ':');
            if (last) {
                snprintf(last, sizeof(masked) - (size_t)(last - masked), ":x:x");
                return masked;
            }
        }
    }
    snprintf(masked, sizeof(masked), "%s", ip);
    return masked;
}

#define CHAT_SLOTS    20
#define CHAT_MSG_LEN  1024

static int  chat_seqno[CHAT_SLOTS];
static char chat_msg[CHAT_SLOTS][CHAT_MSG_LEN];

static int g_chseq    = 3;
static int g_last_msg = 3;
static int g_reload_seq_low = 0;
static int g_reload_seq_high = 0;

static int g_chat_disabled = 0;

static int chat_head = 0;

static time_t g_last_day = 0;

static FILE *chat_logfile = NULL;

void chat_init(void)
{
    memset(chat_seqno, 0, sizeof(chat_seqno));
    memset(chat_msg,   0, sizeof(chat_msg));
    chat_head  = 0;
    g_chseq    = 3;
    g_last_msg = 3;
    g_reload_seq_low = 0;
    g_reload_seq_high = 0;
    g_last_day = 0;

    if (config.chatboxlogfile[0]) {
        chat_logfile = fopen(config.chatboxlogfile, "a");
        if (!chat_logfile)
            fprintf(stderr, "Chat log disabled: unable to open '%s'.\n",
                    config.chatboxlogfile);
    }
}

int chat_get_chseq(void)
{
    return g_chseq;
}

int chat_get_pending_chseq(void)
{
    return g_chseq | 1;
}

void chat_force_reload_all(void)
{

    g_reload_seq_low = g_chseq;
    g_chseq |= 1;
    g_last_msg = g_chseq;
    g_reload_seq_high = g_chseq;
}

static void js_escape(const char *src, char *dst, int maxlen)
{
    int j = 0;
    for (int i = 0; src[i] && j < maxlen - 3; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '\\' || ch == '\'') {
            dst[j++] = '\\';
            dst[j++] = (char)ch;
        } else if (ch >= 32) {
            dst[j++] = (char)ch;
        }
    }
    dst[j] = '\0';
}

int chat_handle_post(const char *msg, const char *name, const char *ip)
{
    if (!msg || !msg[0]) return -1;
    if (g_chat_disabled) return -1;

    while (*msg == ' ' || *msg == '\t') msg++;
    if (!msg[0]) return -1;

    char msgbuf[1024];
    snprintf(msgbuf, sizeof(msgbuf), "%s", msg);
    for (char *p = msgbuf; *p; p++) {
        if (*p == '\n' || *p == '\r') *p = ' ';
    }

    const char *display = (name && name[0] && name[1]) ? name : mask_chat_ip(ip);

    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);

    static const char *months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };

    char timebuf[128];
    if (g_last_day != 0 &&
        (now < g_last_day + 21600 || now / 86400 == g_last_day / 86400)) {
        snprintf(timebuf, sizeof(timebuf), "%02d%02dz %s: ",
                 utc->tm_hour, utc->tm_min, display);
    } else {
        snprintf(timebuf, sizeof(timebuf), "%02d %s %02d%02dz %s: ",
                 utc->tm_mday, months[utc->tm_mon],
                 utc->tm_hour, utc->tm_min, display);
    }
    g_last_day = now;

    char line[CHAT_MSG_LEN + 64];
    size_t time_len = strlen(timebuf);
    size_t max_msg_len = sizeof(line) - time_len - 2;
    snprintf(line, sizeof(line), "%s%.*s\n", timebuf, (int)max_msg_len, msgbuf);

    char escaped[CHAT_MSG_LEN];
    js_escape(line, escaped, sizeof(escaped));

    int elen = (int)strlen(escaped);
    while (elen > 0 && escaped[elen - 1] == ' ') elen--;
    escaped[elen] = '\0';

    g_chseq    |= 1;
    g_last_msg  = g_chseq;

    chat_seqno[chat_head] = g_chseq;
    snprintf(chat_msg[chat_head], CHAT_MSG_LEN, "%s", escaped);
    chat_head = (chat_head + 1) % CHAT_SLOTS;

    if (chat_logfile) {
        fprintf(chat_logfile, "%s (%s) %s%s\n",
                log_dest, ip ? ip : "?", timebuf, msgbuf);
        fflush(chat_logfile);
    }

    log_printf("chat from %s: %s%s\n", ip ? ip : "?", timebuf, msgbuf);
    return 0;
}

static int write_chatnewlines(char *buf, int bufsize, int since)
{
    int n = 0;

    for (int k = 0; k < CHAT_SLOTS; k++) {
        int slot = (chat_head + k) % CHAT_SLOTS;
        if (!chat_msg[slot][0]) continue;
        if (chat_seqno[slot] <= since) continue;
        if (n + (int)strlen(chat_msg[slot]) + 32 >= bufsize) break;
        n += snprintf(buf + n, (size_t)(bufsize - n),
                      "chatnewline('%s');\r\n", chat_msg[slot]);
    }
    return n;
}

int chat_handle_get(char *buf, int bufsize)
{
    return write_chatnewlines(buf, bufsize, 0);
}

int chat_write_othersjj(char *buf, int bufsize, int n, int client_chseq)
{

    g_chseq = ((g_chseq + 1) | 1) - 1;

    n += snprintf(buf + n, (size_t)(bufsize - n), "chseq=%d;\n", g_chseq);

    if (client_chseq >= g_reload_seq_low && client_chseq < g_reload_seq_high) {
        if (n + 32 < bufsize)
            n += snprintf(buf + n, (size_t)(bufsize - n),
                          "location.reload(true);");
        return n;
    }

    int floor_seqno = g_chseq - (CHAT_SLOTS * 2);
    if (client_chseq > 2 && floor_seqno > 2 && client_chseq < floor_seqno) {
        if (n + 32 < bufsize)
            n += snprintf(buf + n, (size_t)(bufsize - n),
                          "location.reload(true);");
        return n;
    }

    if (client_chseq < g_last_msg && n < bufsize - 64) {
        n += write_chatnewlines(buf + n, bufsize - n, client_chseq);
    }

    return n;
}

int chat_handle_identities(char *buf, int bufsize)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS && n < bufsize - 64; i++) {
        if (clients[i].state != CLIENT_WEBSOCKET) continue;
        const char *uname = clients[i].username[0] ? clients[i].username : "?";
        int written = snprintf(buf + n, (size_t)(bufsize - n), "%s\n", uname);
        if (written > 0) n += written;
    }
    return n;
}

int chat_handle_censor(int seqno)
{
    for (int i = 0; i < CHAT_SLOTS; i++) {
        if (chat_seqno[i] == seqno) {
            char tmp[CHAT_MSG_LEN];
            snprintf(tmp, sizeof(tmp), "-%s", chat_msg[i]);
            snprintf(chat_msg[i], CHAT_MSG_LEN, "%s", tmp);
            return 0;
        }
    }
    return -1;
}

void chat_handle_censor_line(const char *line)
{
    for (int i = 0; i < CHAT_SLOTS; i++) {
        if (strncmp(chat_msg[i], line, strlen(line)) == 0) {
            char tmp[CHAT_MSG_LEN];
            snprintf(tmp, sizeof(tmp), "-%.1021s", chat_msg[i]);
            memcpy(chat_msg[i], tmp, CHAT_MSG_LEN);
            chat_msg[i][CHAT_MSG_LEN - 1] = '\0';
            g_chseq |= 1;
            g_last_msg = g_chseq;
            return;
        }
    }
}

void chat_set_disabled(int val)
{
    g_chat_disabled = val ? 1 : 0;
}

int chat_is_disabled(void)
{
    return g_chat_disabled;
}
