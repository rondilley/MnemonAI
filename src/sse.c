/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * sse.c -- Thread-safe event queue for SSE (Server-Sent Events) streaming
 *
 * Fixed-size ring buffer with blocking pop (condvar + timeout).
 * Push copies strings via strdup; pop transfers ownership to caller.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "sse.h"

mnemon_err_t sse_queue_init(sse_queue_t *q)
{
    if (!q) return MNEMON_ERR_INVALID_INPUT;

    memset(q, 0, sizeof(*q));

    if (pthread_mutex_init(&q->mutex, NULL) != 0)
        return MNEMON_ERR_INTERNAL;

    if (pthread_cond_init(&q->cond, NULL) != 0) {
        pthread_mutex_destroy(&q->mutex);
        return MNEMON_ERR_INTERNAL;
    }

    return MNEMON_OK;
}

void sse_queue_destroy(sse_queue_t *q)
{
    if (!q) return;

    /* Free any remaining events */
    while (q->count > 0) {
        sse_event_t *e = &q->events[q->head];
        free(e->event_type);
        free(e->data);
        e->event_type = NULL;
        e->data = NULL;
        q->head = (q->head + 1) % SSE_QUEUE_CAPACITY;
        q->count--;
    }

    pthread_cond_destroy(&q->cond);
    pthread_mutex_destroy(&q->mutex);
}

mnemon_err_t sse_queue_push(sse_queue_t *q, const char *event_type,
                            const char *json_data)
{
    if (!q || !event_type || !json_data)
        return MNEMON_ERR_INVALID_INPUT;

    pthread_mutex_lock(&q->mutex);

    if (q->closed) {
        pthread_mutex_unlock(&q->mutex);
        return MNEMON_ERR_SHUTDOWN;
    }

    if (q->count >= SSE_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&q->mutex);
        return MNEMON_ERR_QUEUE_FULL;
    }

    sse_event_t *e = &q->events[q->tail];
    e->event_type = strdup(event_type);
    e->data = strdup(json_data);
    if (!e->event_type || !e->data) {
        free(e->event_type);
        free(e->data);
        e->event_type = NULL;
        e->data = NULL;
        pthread_mutex_unlock(&q->mutex);
        return MNEMON_ERR_OOM;
    }

    q->tail = (q->tail + 1) % SSE_QUEUE_CAPACITY;
    q->count++;

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);

    return MNEMON_OK;
}

mnemon_err_t sse_queue_pop(sse_queue_t *q, sse_event_t *out, int timeout_ms)
{
    if (!q || !out) return MNEMON_ERR_INVALID_INPUT;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&q->mutex);

    while (q->count == 0 && !q->closed) {
        int rc = pthread_cond_timedwait(&q->cond, &q->mutex, &ts);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&q->mutex);
            return MNEMON_ERR_NOT_FOUND;
        }
    }

    if (q->closed && q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return MNEMON_ERR_SHUTDOWN;
    }

    /* Dequeue */
    *out = q->events[q->head];
    q->events[q->head].event_type = NULL;
    q->events[q->head].data = NULL;
    q->head = (q->head + 1) % SSE_QUEUE_CAPACITY;
    q->count--;

    pthread_mutex_unlock(&q->mutex);
    return MNEMON_OK;
}

void sse_queue_close(sse_queue_t *q)
{
    if (!q) return;

    pthread_mutex_lock(&q->mutex);
    q->closed = true;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}
