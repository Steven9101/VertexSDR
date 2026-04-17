// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#include "worker.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>

#define WORKER_QUEUE_MAX 10

typedef struct {
    worker_fn_t     fn;
    void           *arg;
    struct timeval  enqueue_ts;
} WorkerTask;

static WorkerTask  queue[WORKER_QUEUE_MAX];
static int         queue_head = 0;
static int         queue_tail = 0;
static int         queue_len  = 0;

static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  queue_cond  = PTHREAD_COND_INITIALIZER;

pthread_mutex_t worker_dsp_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_t worker_thread_id;

int worker_enqueue(worker_fn_t fn, void *arg)
{
    pthread_mutex_lock(&queue_mutex);

    if (queue_len >= WORKER_QUEUE_MAX) {
        pthread_mutex_unlock(&queue_mutex);
        fprintf(stderr, "Worker queue overflow: no free task slots remain.\n");
        abort();
    }

    gettimeofday(&queue[queue_tail].enqueue_ts, NULL);
    queue[queue_tail].fn  = fn;
    queue[queue_tail].arg = arg;
    queue_tail = (queue_tail + 1) % WORKER_QUEUE_MAX;
    queue_len++;

    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
    return 0;
}

static void *worker_thread_body(void *unused)
{
    (void)unused;

    for (;;) {
        pthread_mutex_lock(&queue_mutex);
        while (queue_len == 0)
            pthread_cond_wait(&queue_cond, &queue_mutex);

        WorkerTask t = queue[queue_head];
        queue_head = (queue_head + 1) % WORKER_QUEUE_MAX;
        queue_len--;

        pthread_mutex_unlock(&queue_mutex);

        pthread_mutex_lock(&worker_dsp_mutex);
        t.fn(t.arg);
        pthread_mutex_unlock(&worker_dsp_mutex);
    }
    return NULL;
}

int worker_init(int nthreads)
{
    (void)nthreads;
    int rc = pthread_create(&worker_thread_id, NULL, worker_thread_body, NULL);
    if (rc != 0) {
        fprintf(stderr, "Failed to start worker thread: %s\n", strerror(rc));
        return -1;
    }
    return 0;
}
