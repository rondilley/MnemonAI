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

mnemon_err_t mnemon_graph_put_edge(mnemon_graph_t *g, MDB_txn *txn,
                                   const mnemon_edge_t *e);
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

#endif /* MNEMON_GRAPH_H */
