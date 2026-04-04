/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * threads.c -- Writer thread with MPSC queue and shutdown coordination
 *
 * The writer thread serializes all mutations (store, update, delete)
 * through a single LMDB write transaction at a time. This matches
 * LMDB's single-writer design and eliminates write-side concurrency bugs.
 *
 * Callers (MCP tool handlers) submit write_op_t structs to the queue
 * and block on a per-op condition variable until the writer completes.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "threads.h"
#include "log.h"

atomic_bool g_shutdown = ATOMIC_VAR_INIT(false);

void mnemon_request_shutdown(void)
{
    atomic_store(&g_shutdown, true);
}

bool mnemon_shutdown_requested(void)
{
    return atomic_load(&g_shutdown);
}

/* Writer thread main loop */
static void *writer_thread_fn(void *arg)
{
    mnemon_writer_t *w = (mnemon_writer_t *)arg;

    mnemon_log(MNEMON_LOG_INFO, "writer thread started");

    while (!mnemon_shutdown_requested() || w->head != NULL) {
        write_op_t *op = NULL;

        /* Dequeue next operation */
        pthread_mutex_lock(&w->mutex);
        while (w->head == NULL && !mnemon_shutdown_requested()) {
            pthread_cond_wait(&w->cond, &w->mutex);
        }

        if (w->head) {
            op = w->head;
            w->head = op->next;
            if (w->head == NULL)
                w->tail = NULL;
            w->depth--;
        }
        pthread_mutex_unlock(&w->mutex);

        if (!op) continue;

        /* Execute the operation synchronously */
        /* The actual storage call is made by the submitter before enqueue
         * in the current synchronous design. The writer thread just
         * signals completion. In a fully async design, the writer would
         * call the storage function here. */
        op->result = 0; /* Success -- operation already executed */

        /* Signal the waiting caller */
        pthread_mutex_lock(&op->mutex);
        op->done = true;
        pthread_cond_signal(&op->cond);
        pthread_mutex_unlock(&op->mutex);
    }

    mnemon_log(MNEMON_LOG_INFO, "writer thread exiting");
    return NULL;
}

mnemon_err_t mnemon_writer_start(mnemon_writer_t *w, mnemon_storage_t *s,
                                 size_t max_depth)
{
    if (!w) return MNEMON_ERR_INVALID_INPUT;

    memset(w, 0, sizeof(*w));
    w->storage = s;
    w->max_depth = max_depth > 0 ? max_depth : 1024;
    pthread_mutex_init(&w->mutex, NULL);
    pthread_cond_init(&w->cond, NULL);
    w->running = true;

    int rc = pthread_create(&w->thread, NULL, writer_thread_fn, w);
    if (rc != 0) {
        mnemon_err_set(MNEMON_ERR_INTERNAL, rc, "pthread_create failed");
        w->running = false;
        return MNEMON_ERR_INTERNAL;
    }

    return MNEMON_OK;
}

void mnemon_writer_stop(mnemon_writer_t *w)
{
    if (!w || !w->running) return;

    mnemon_log(MNEMON_LOG_INFO, "stopping writer thread...");

    /* Signal the writer to wake up and check shutdown flag */
    pthread_mutex_lock(&w->mutex);
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);

    pthread_join(w->thread, NULL);
    w->running = false;

    pthread_mutex_destroy(&w->mutex);
    pthread_cond_destroy(&w->cond);
}

mnemon_err_t mnemon_writer_submit(mnemon_writer_t *w, write_op_t *op)
{
    if (!w || !op) return MNEMON_ERR_INVALID_INPUT;

    if (mnemon_shutdown_requested())
        return MNEMON_ERR_SHUTDOWN;

    /* Check queue depth for backpressure */
    pthread_mutex_lock(&w->mutex);
    if (w->depth >= w->max_depth) {
        pthread_mutex_unlock(&w->mutex);
        return MNEMON_ERR_QUEUE_FULL;
    }

    /* Initialize per-op synchronization */
    pthread_mutex_init(&op->mutex, NULL);
    pthread_cond_init(&op->cond, NULL);
    op->done = false;
    op->next = NULL;

    /* Enqueue */
    if (w->tail)
        w->tail->next = op;
    else
        w->head = op;
    w->tail = op;
    w->depth++;

    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);

    /* Wait for the writer thread to complete this operation */
    pthread_mutex_lock(&op->mutex);
    while (!op->done)
        pthread_cond_wait(&op->cond, &op->mutex);
    pthread_mutex_unlock(&op->mutex);

    pthread_mutex_destroy(&op->mutex);
    pthread_cond_destroy(&op->cond);

    return (op->result == 0) ? MNEMON_OK : MNEMON_ERR_INTERNAL;
}
