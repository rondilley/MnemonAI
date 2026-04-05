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

/*
 * Reader pool -- persistent thread pool for concurrent read tasks.
 * Tasks are submitted via mnemon_reader_pool_submit() and executed
 * asynchronously. Callers can wait for completion.
 */

typedef void (*reader_task_fn)(void *arg);

typedef struct reader_task {
    reader_task_fn       fn;
    void                *arg;
    struct reader_task  *next;
    bool                 done;
    pthread_mutex_t      mutex;
    pthread_cond_t       cond;
} reader_task_t;

typedef struct mnemon_reader_pool {
    pthread_t       *threads;
    int              pool_size;
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
    reader_task_t   *head;
    reader_task_t   *tail;
    bool             running;
} mnemon_reader_pool_t;

mnemon_err_t mnemon_reader_pool_start(mnemon_reader_pool_t *pool, int size);
void         mnemon_reader_pool_stop(mnemon_reader_pool_t *pool);

/* Submit a task and optionally wait for completion.
 * If wait=true, blocks until task completes. If wait=false, caller
 * must call mnemon_reader_task_wait() later. */
mnemon_err_t mnemon_reader_pool_submit(mnemon_reader_pool_t *pool,
                                       reader_task_t *task);
void         mnemon_reader_task_wait(reader_task_t *task);

#endif /* MNEMON_THREADS_H */
