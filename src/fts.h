/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * fts.h -- SQLite FTS5 full-text search interface
 */

#ifndef MNEMON_FTS_H
#define MNEMON_FTS_H

#include "mnemon.h"

typedef struct mnemon_fts mnemon_fts_t;

typedef struct {
    uint8_t  id[16];
    float    score;
    int      source_type; /* 0=memory, 1=entity */
} mnemon_fts_result_t;

typedef struct {
    mnemon_fts_result_t *results;
    int                  count;
} mnemon_fts_results_t;

mnemon_err_t mnemon_fts_open(mnemon_fts_t **out, const char *db_path);
void         mnemon_fts_close(mnemon_fts_t *f);

mnemon_err_t mnemon_fts_index_memory(mnemon_fts_t *f, const mnemon_memory_t *mem);
mnemon_err_t mnemon_fts_index_entity(mnemon_fts_t *f, const mnemon_entity_t *e);
mnemon_err_t mnemon_fts_remove(mnemon_fts_t *f, const uint8_t id[16], int source_type);
mnemon_err_t mnemon_fts_update_memory(mnemon_fts_t *f, const mnemon_memory_t *mem);
mnemon_err_t mnemon_fts_update_entity(mnemon_fts_t *f, const mnemon_entity_t *e);

mnemon_err_t mnemon_fts_search(mnemon_fts_t *f, const char *query, int top_k,
                               mnemon_fts_results_t *out);
/* Like mnemon_fts_search but restricts to a document class:
 * source_type 0 = memories, 1 = entities, <0 = any. */
mnemon_err_t mnemon_fts_search_typed(mnemon_fts_t *f, const char *query,
                                     int source_type, int top_k,
                                     mnemon_fts_results_t *out);
void         mnemon_fts_results_free(mnemon_fts_results_t *r);

mnemon_err_t mnemon_fts_checkpoint(mnemon_fts_t *f);
mnemon_err_t mnemon_fts_clear(mnemon_fts_t *f);
mnemon_err_t mnemon_fts_count(mnemon_fts_t *f, size_t *out);

#endif /* MNEMON_FTS_H */
