/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * graph.h -- LMDB knowledge graph interface
 */

#ifndef MNEMON_GRAPH_H
#define MNEMON_GRAPH_H

#include "mnemon.h"
#include <lmdb.h>

typedef struct mnemon_graph mnemon_graph_t;

typedef int (*mnemon_visit_fn)(const mnemon_entity_t *entity,
                               const mnemon_edge_t *edge,
                               int depth, void *user_ctx);

mnemon_err_t mnemon_graph_open(mnemon_graph_t **out, const char *path,
                               int map_size_gb, int max_readers);
void         mnemon_graph_close(mnemon_graph_t *g);
MDB_env     *mnemon_graph_env(mnemon_graph_t *g);

mnemon_err_t mnemon_graph_put_entity(mnemon_graph_t *g, MDB_txn *txn,
                                     const mnemon_entity_t *e);
mnemon_err_t mnemon_graph_get_entity(mnemon_graph_t *g, MDB_txn *txn,
                                     const uint8_t id[16],
                                     mnemon_entity_t *out);
mnemon_err_t mnemon_graph_del_entity(mnemon_graph_t *g, MDB_txn *txn,
                                     const uint8_t id[16]);

/* Bi-temporal versioning: write an immutable entity snapshot under version_id
 * and index it in the temporal DBI by (entity_id, valid_from=updated_at). */
mnemon_err_t mnemon_graph_put_version(mnemon_graph_t *g, MDB_txn *txn,
                                      const uint8_t version_id[16],
                                      const mnemon_entity_t *e);
/* Load an entity's snapshots with valid_from in [since,until] (0=unbounded),
 * ascending by valid_from. Caller frees each entity and the array. */
mnemon_err_t mnemon_graph_load_versions(mnemon_graph_t *g, MDB_txn *txn,
                                        const uint8_t entity_id[16],
                                        int64_t since, int64_t until,
                                        mnemon_entity_t **out, uint32_t *count);

/* Remove temporal entries whose index valid_from differs from the referenced
 * snapshot's updated_at by more than tol_ms (mis-keyed backfill artifacts).
 * Deletes the temporal entry and the orphaned snapshot. *pruned = count. */
mnemon_err_t mnemon_graph_prune_miskeyed_versions(mnemon_graph_t *g,
                                                  MDB_txn *txn,
                                                  int64_t tol_ms,
                                                  size_t *pruned);

mnemon_err_t mnemon_graph_put_edge(mnemon_graph_t *g, MDB_txn *txn,
                                   const mnemon_edge_t *e);
/* True if an edge source --type--> target already exists (forward index). */
bool mnemon_graph_edge_exists(mnemon_graph_t *g, MDB_txn *txn,
                              const uint8_t source[16], const char *type,
                              const uint8_t target[16]);
mnemon_err_t mnemon_graph_get_edges_from(mnemon_graph_t *g, MDB_txn *txn,
                                         const uint8_t source_id[16],
                                         const char *edge_type,
                                         mnemon_edge_list_t *out);
mnemon_err_t mnemon_graph_get_edges_to(mnemon_graph_t *g, MDB_txn *txn,
                                       const uint8_t target_id[16],
                                       const char *edge_type,
                                       mnemon_edge_list_t *out);

mnemon_err_t mnemon_graph_put_memory(mnemon_graph_t *g, MDB_txn *txn,
                                     const mnemon_memory_t *mem);
mnemon_err_t mnemon_graph_get_memory(mnemon_graph_t *g, MDB_txn *txn,
                                     const uint8_t id[16],
                                     mnemon_memory_t *out);
mnemon_err_t mnemon_graph_del_memory(mnemon_graph_t *g, MDB_txn *txn,
                                     const uint8_t id[16]);

mnemon_err_t mnemon_graph_bfs(mnemon_graph_t *g, MDB_txn *txn,
                              const uint8_t start_id[16], int max_depth,
                              mnemon_visit_fn visit, void *user_ctx);

mnemon_err_t mnemon_graph_put_intent(mnemon_graph_t *g, MDB_txn *txn,
                                     const uint8_t id[16], uint8_t op_type,
                                     uint8_t steps_done,
                                     const uint8_t *payload,
                                     size_t payload_len);
mnemon_err_t mnemon_graph_update_intent(mnemon_graph_t *g, MDB_txn *txn,
                                        const uint8_t id[16],
                                        uint8_t steps_done);
mnemon_err_t mnemon_graph_del_intent(mnemon_graph_t *g, MDB_txn *txn,
                                     const uint8_t id[16]);

mnemon_err_t mnemon_graph_get_meta(mnemon_graph_t *g, MDB_txn *txn,
                                   const char *key, char *buf,
                                   size_t buf_len);
mnemon_err_t mnemon_graph_put_meta(mnemon_graph_t *g, MDB_txn *txn,
                                   const char *key, const char *value);

mnemon_err_t mnemon_graph_txn_begin(mnemon_graph_t *g, unsigned int flags,
                                    MDB_txn **txn);
mnemon_err_t mnemon_graph_txn_commit(MDB_txn *txn);
void         mnemon_graph_txn_abort(MDB_txn *txn);

mnemon_err_t mnemon_graph_count(mnemon_graph_t *g, MDB_txn *txn,
                                size_t *entities, size_t *edges,
                                size_t *memories);

/* Chunk metadata */
mnemon_err_t mnemon_graph_put_chunk(mnemon_graph_t *g, MDB_txn *txn,
                                    const mnemon_chunk_meta_t *c);
mnemon_err_t mnemon_graph_get_chunk(mnemon_graph_t *g, MDB_txn *txn,
                                    const uint8_t chunk_id[16],
                                    mnemon_chunk_meta_t *out);
mnemon_err_t mnemon_graph_del_chunk(mnemon_graph_t *g, MDB_txn *txn,
                                    const uint8_t chunk_id[16]);
/* Collect the chunk UUIDs whose parent is parent_id.  Scans the chunks DBI;
 * writes up to max_ids 16-byte IDs into out_ids and sets *count. */
mnemon_err_t mnemon_graph_get_chunks_by_parent(mnemon_graph_t *g, MDB_txn *txn,
                                               const uint8_t parent_id[16],
                                               uint8_t (*out_ids)[16],
                                               size_t max_ids, size_t *count);
/* Empty the chunks DBI entirely (used during vector rebuild). */
mnemon_err_t mnemon_graph_clear_chunks(mnemon_graph_t *g, MDB_txn *txn);

/* Delete every edge (forward + reverse index) where entity_id is the source
 * or the target. Used to cascade entity deletion so no dangling edges remain.
 * *deleted (optional) receives the number of edges removed. */
mnemon_err_t mnemon_graph_del_edges_for_entity(mnemon_graph_t *g, MDB_txn *txn,
                                               const uint8_t entity_id[16],
                                               size_t *deleted);

/* Delete edges whose source or target no longer exists as an entity or
 * memory. Cleans dangling edges left by older deletes. *pruned (optional)
 * receives the count removed. */
mnemon_err_t mnemon_graph_prune_orphan_edges(mnemon_graph_t *g, MDB_txn *txn,
                                             size_t *pruned);

#endif /* MNEMON_GRAPH_H */
