// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#ifndef CHAT_H
#define CHAT_H

void chat_init(void);

int chat_get_chseq(void);
int chat_get_pending_chseq(void);
void chat_force_reload_all(void);

int chat_handle_post(const char *msg, const char *name, const char *ip);

int chat_handle_get(char *buf, int bufsize);

int chat_write_othersjj(char *buf, int bufsize, int n, int client_chseq);

int chat_handle_identities(char *buf, int bufsize);

int chat_handle_censor(int seqno);

void chat_handle_censor_line(const char *line);

void chat_set_disabled(int val);
int  chat_is_disabled(void);

#endif
