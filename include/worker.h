// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#ifndef WORKER_H
#define WORKER_H

#include <pthread.h>

typedef void (*worker_fn_t)(void *arg);

extern pthread_mutex_t worker_dsp_mutex;

int worker_init(int nthreads);
int worker_enqueue(worker_fn_t fn, void *arg);

#endif
