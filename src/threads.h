/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * threads.h -- Threading: writer queue, reader pool, shutdown
 */

#ifndef MNEMON_THREADS_H
#define MNEMON_THREADS_H

#include "mnemon.h"
#include <stdatomic.h>

/* Global shutdown flag -- checked by all loops */
extern atomic_bool g_shutdown;

void mnemon_request_shutdown(void);
bool mnemon_shutdown_requested(void);

/*
 * Write queue (MPSC -- multiple producers, single consumer).
 * MCP tool handlers enqueue write operations. The writer thread
 * drains the queue and executes them sequentially.
 *
 * Phase 2: For now, operations are still synchronous but the queue
 * infrastructure is in place for async operation.
 */

typedef struct mnemon_storage mnemon_storage_t;

/* Write operation types */
typedef enum {
    WRITE_OP_STORE_MEMORY,
    WRITE_OP_UPDATE_MEMORY,
    WRITE_OP_DELETE_MEMORY,
    WRITE_OP_STORE_ENTITY,
    WRITE_OP_STORE_EDGE,
    WRITE_OP_DELETE_ENTITY,
} write_op_type_t;

/* Write operation (queued for the writer thread) */
typedef struct write_op {
    write_op_type_t  type;
    void            *data;      /* Operation-specific data (caller-owned) */
    struct write_op *next;      /* Linked list for queue */
    int              result;    /* 0 = success, set by writer thread */
    bool             done;      /* Set true when writer completes */
    pthread_mutex_t  mutex;     /* For signaling completion */
    pthread_cond_t   cond;
} write_op_t;

/* Writer thread state */
typedef struct {
    pthread_t        thread;
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
    write_op_t      *head;
    write_op_t      *tail;
    size_t           depth;
    size_t           max_depth;
    mnemon_storage_t *storage;
    bool             running;
} mnemon_writer_t;

mnemon_err_t mnemon_writer_start(mnemon_writer_t *w, mnemon_storage_t *s,
                                 size_t max_depth);
void         mnemon_writer_stop(mnemon_writer_t *w);

/* Enqueue a write op and wait for completion (synchronous from caller's POV) */
mnemon_err_t mnemon_writer_submit(mnemon_writer_t *w, write_op_t *op);

#endif /* MNEMON_THREADS_H */
