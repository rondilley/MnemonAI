/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mnemon.h -- Common definitions for mnemond
 */

#ifndef MNEMON_H
#define MNEMON_H

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    MNEMON_OK = 0,
    MNEMON_ERR_NOT_FOUND,
    MNEMON_ERR_ALREADY_EXISTS,
    MNEMON_ERR_LMDB,
    MNEMON_ERR_SQLITE,
    MNEMON_ERR_USEARCH,
    MNEMON_ERR_EMBED,
    MNEMON_ERR_EXTRACTION,
    MNEMON_ERR_SECRET_DETECTED,
    MNEMON_ERR_INVALID_INPUT,
    MNEMON_ERR_QUEUE_FULL,
    MNEMON_ERR_SHUTDOWN,
    MNEMON_ERR_OOM,
    MNEMON_ERR_IO,
    MNEMON_ERR_INTERNAL
} mnemon_err_t;

const char *mnemon_strerror(mnemon_err_t err);
const char *mnemon_err_msg(void);
int         mnemon_err_code(void);
void        mnemon_err_set(mnemon_err_t err, int native_code,
                           const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* Memory tier                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    MNEMON_TIER_EPISODIC   = 0,
    MNEMON_TIER_SEMANTIC   = 1,
    MNEMON_TIER_PROCEDURAL = 2,
} mnemon_memory_tier_t;

/* ------------------------------------------------------------------ */
/* Entity                                                              */
/* ------------------------------------------------------------------ */

typedef struct mnemon_entity {
    uint8_t     id[16];
    char       *name;
    char       *entity_type;
    char      **observations;
    uint32_t    observation_count;
    float      *embedding;
    int64_t     created_at;
    int64_t     updated_at;
    float       importance;
    uint32_t    access_count;
    int64_t     last_accessed;
} mnemon_entity_t;

void mnemon_entity_free(mnemon_entity_t *e);

/* ------------------------------------------------------------------ */
/* Edge (bi-temporal)                                                   */
/* ------------------------------------------------------------------ */

typedef struct mnemon_edge {
    uint8_t     id[16];
    uint8_t     source_id[16];
    uint8_t     target_id[16];
    char       *edge_type;
    char       *description;
    float       weight;
    int64_t     valid_from;
    int64_t     valid_to;
    int64_t     created_at;
    int64_t     expired_at;
} mnemon_edge_t;

void mnemon_edge_free(mnemon_edge_t *e);

typedef struct {
    mnemon_edge_t *edges;
    uint32_t       count;
} mnemon_edge_list_t;

void mnemon_edge_list_free(mnemon_edge_list_t *list);

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

typedef struct mnemon_memory {
    uint8_t              id[16];
    mnemon_memory_tier_t tier;
    char                *content;
    char                *source_type;
    char                *source_id;
    char                *source_author;
    int64_t              source_timestamp;
    char               **tags;
    uint32_t             tag_count;
    float               *embedding;
    float                importance;
    uint32_t             access_count;
    int64_t              created_at;
    int64_t              last_accessed;
    uint8_t            **entity_ids;
    uint32_t             entity_id_count;
    bool                 consolidated;
} mnemon_memory_t;

void mnemon_memory_free(mnemon_memory_t *mem);

/* ------------------------------------------------------------------ */
/* Search results                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char    id[37];
    char   *content;
    float   score;
    float   graph_score;
    float   vector_score;
    float   keyword_score;
    char   *tier;
} mnemon_result_t;

typedef struct {
    mnemon_result_t *results;
    int              count;
    bool             truncated;
} mnemon_result_set_t;

void mnemon_result_set_free(mnemon_result_set_t *rs);

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *query_text;
    float      *query_embedding;
    int         top_k;
    float       min_score;
    const char *tier_filter;
    const char *entity_type_filter;
    int64_t     since;
    int64_t     until;
} mnemon_query_t;

/* ------------------------------------------------------------------ */
/* Version list (temporal)                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    mnemon_entity_t *versions;
    uint32_t         count;
} mnemon_version_list_t;

void mnemon_version_list_free(mnemon_version_list_t *vl);

#endif /* MNEMON_H */
