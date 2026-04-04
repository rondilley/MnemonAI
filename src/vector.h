/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * vector.h -- usearch HNSW vector index interface
 */

#ifndef MNEMON_VECTOR_H
#define MNEMON_VECTOR_H

#include "mnemon.h"
#include <pthread.h>

typedef struct mnemon_vector mnemon_vector_t;

typedef struct {
    uint8_t id[16];
    float   distance;
} mnemon_vector_result_t;

typedef struct {
    mnemon_vector_result_t *results;
    int                     count;
} mnemon_vector_results_t;

mnemon_err_t mnemon_vector_open(mnemon_vector_t **out, const char *dir,
                                int dimensions);
void         mnemon_vector_close(mnemon_vector_t *v);

mnemon_err_t mnemon_vector_add(mnemon_vector_t *v, const uint8_t id[16],
                               const float *embedding, int dimensions,
                               bool is_entity);
mnemon_err_t mnemon_vector_remove(mnemon_vector_t *v, const uint8_t id[16],
                                  bool is_entity);
mnemon_err_t mnemon_vector_search(mnemon_vector_t *v, const float *query,
                                  int dimensions, int top_k,
                                  bool search_entities,
                                  mnemon_vector_results_t *out);
void         mnemon_vector_results_free(mnemon_vector_results_t *r);
mnemon_err_t mnemon_vector_save(mnemon_vector_t *v);
size_t       mnemon_vector_count(mnemon_vector_t *v, bool entities);
void         mnemon_vector_read_lock(mnemon_vector_t *v);
void         mnemon_vector_read_unlock(mnemon_vector_t *v);
void         mnemon_vector_write_lock(mnemon_vector_t *v);
void         mnemon_vector_write_unlock(mnemon_vector_t *v);

#endif /* MNEMON_VECTOR_H */
