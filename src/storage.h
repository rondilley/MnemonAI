/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * storage.h -- Cross-engine storage coordinator interface
 */

#ifndef MNEMON_STORAGE_H
#define MNEMON_STORAGE_H

#include "mnemon.h"

/* Forward declarations */
typedef struct mnemon_config mnemon_config_t;
typedef struct mnemon_graph mnemon_graph_t;
typedef struct mnemon_fts mnemon_fts_t;
typedef struct mnemon_vector mnemon_vector_t;
typedef struct mnemon_embed mnemon_embed_t;
typedef struct mnemon_honeypot mnemon_honeypot_t;

typedef struct mnemon_storage mnemon_storage_t;

/* Intent operation types */
#define MNEMON_OP_STORE_MEMORY  1
#define MNEMON_OP_UPDATE_MEMORY 2
#define MNEMON_OP_DELETE_MEMORY 3
#define MNEMON_OP_STORE_ENTITY  4
#define MNEMON_OP_STORE_EDGE    5
#define MNEMON_OP_DELETE_ENTITY 6

/* Intent step bitmasks */
#define MNEMON_STEP_LMDB    0x01
#define MNEMON_STEP_FTS5    0x02
#define MNEMON_STEP_USEARCH 0x04

/* Stats */
typedef struct {
    size_t   total_memories;
    size_t   episodic_count;
    size_t   semantic_count;
    size_t   procedural_count;
    size_t   total_entities;
    size_t   total_edges;
    size_t   memory_vectors;
    size_t   entity_vectors;
    size_t   fts_indexed;
    uint64_t lmdb_map_size;
    uint64_t lmdb_used_size;
    int64_t  uptime_start;
} mnemon_stats_t;

/* Lifecycle */
mnemon_err_t mnemon_storage_open(mnemon_storage_t **out,
                                 const mnemon_config_t *cfg);
void         mnemon_storage_close(mnemon_storage_t *s);

/* Write operations */
mnemon_err_t mnemon_store_memory(mnemon_storage_t *s,
                                 mnemon_memory_t *mem);
mnemon_err_t mnemon_update_memory(mnemon_storage_t *s, const uint8_t id[16],
                                  const char *new_content,
                                  const float *new_embedding);
mnemon_err_t mnemon_delete_memory(mnemon_storage_t *s, const uint8_t id[16]);

mnemon_err_t mnemon_store_entity(mnemon_storage_t *s,
                                 mnemon_entity_t *e);
mnemon_err_t mnemon_store_edge(mnemon_storage_t *s,
                               const mnemon_edge_t *e);
mnemon_err_t mnemon_delete_entity(mnemon_storage_t *s, const uint8_t id[16]);

/* Connect entities to memories that mention them by name ("mentioned_in"
 * edges). Idempotent. *created receives the number of new edges. */
mnemon_err_t mnemon_link_entities(mnemon_storage_t *s, size_t *created);

/* Merge entities sharing a normalized name + entity_type into one canonical
 * entity (most observations wins); appends observations, re-points edges,
 * deletes duplicates. *merged receives the number removed. */
mnemon_err_t mnemon_resolve_entities(mnemon_storage_t *s, size_t *merged);

/* Write a creation-time baseline version snapshot for entities that lack one
 * (predate versioning). Idempotent. *count receives the number written. */
mnemon_err_t mnemon_backfill_versions(mnemon_storage_t *s, size_t *count);

/* Read operations (thread-safe) */
mnemon_err_t mnemon_get_memory(mnemon_storage_t *s, const uint8_t id[16],
                               mnemon_memory_t *out);
mnemon_err_t mnemon_get_entity(mnemon_storage_t *s, const uint8_t id[16],
                               mnemon_entity_t *out);
mnemon_err_t mnemon_get_edges_from(mnemon_storage_t *s,
                                   const uint8_t source_id[16],
                                   const char *edge_type,
                                   mnemon_edge_list_t *out);
mnemon_err_t mnemon_get_edges_to(mnemon_storage_t *s,
                                 const uint8_t target_id[16],
                                 const char *edge_type,
                                 mnemon_edge_list_t *out);

/* Maintenance */
mnemon_err_t mnemon_rebuild_indexes(mnemon_storage_t *s, const char *target);
mnemon_err_t mnemon_replay_intents(mnemon_storage_t *s);
mnemon_err_t mnemon_get_stats(mnemon_storage_t *s, mnemon_stats_t *out);

/* Access sub-components (for search module) */
mnemon_graph_t  *mnemon_storage_graph(mnemon_storage_t *s);
mnemon_fts_t    *mnemon_storage_fts(mnemon_storage_t *s);
mnemon_vector_t *mnemon_storage_vector(mnemon_storage_t *s);
mnemon_embed_t      *mnemon_storage_embed(mnemon_storage_t *s);
mnemon_honeypot_t       *mnemon_storage_honeypot(mnemon_storage_t *s);
void                     mnemon_storage_set_honeypot(mnemon_storage_t *s,
                                                     mnemon_honeypot_t *hp);

typedef struct mnemon_reader_pool mnemon_reader_pool_t;
mnemon_reader_pool_t    *mnemon_storage_reader_pool(mnemon_storage_t *s);
void                     mnemon_storage_set_reader_pool(mnemon_storage_t *s,
                                                        mnemon_reader_pool_t *pool);

#endif /* MNEMON_STORAGE_H */
