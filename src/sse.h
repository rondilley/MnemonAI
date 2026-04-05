/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * sse.h -- Thread-safe event queue for SSE (Server-Sent Events) streaming
 */

#ifndef MNEMON_SSE_H
#define MNEMON_SSE_H

#include "mnemon.h"
#include <pthread.h>

#define SSE_QUEUE_CAPACITY 64

typedef struct {
    char *event_type;   /* e.g. "message", "endpoint" */
    char *data;         /* JSON payload */
} sse_event_t;

typedef struct {
    sse_event_t      events[SSE_QUEUE_CAPACITY];
    int              head;
    int              tail;
    int              count;
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
    bool             closed;
} sse_queue_t;

mnemon_err_t sse_queue_init(sse_queue_t *q);
void         sse_queue_destroy(sse_queue_t *q);

/* Push an event. Copies strings. Returns MNEMON_ERR_QUEUE_FULL if full. */
mnemon_err_t sse_queue_push(sse_queue_t *q, const char *event_type,
                            const char *json_data);

/* Blocking pop with timeout (ms). Returns MNEMON_ERR_NOT_FOUND on timeout,
 * MNEMON_ERR_SHUTDOWN if queue closed. Caller must free event fields. */
mnemon_err_t sse_queue_pop(sse_queue_t *q, sse_event_t *out, int timeout_ms);

/* Close the queue, unblocking any waiters. */
void         sse_queue_close(sse_queue_t *q);

#endif /* MNEMON_SSE_H */
