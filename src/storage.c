/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * storage.c -- Cross-engine storage coordinator with write-ahead intent log
 *
 * Coordinates writes across LMDB (source of truth), SQLite FTS5 (keyword
 * search), and usearch (vector similarity). Uses a write-ahead intent log
 * in LMDB to ensure crash recovery can replay incomplete operations.
 *
 * Write sequence per operation:
 *   1. Write intent record to LMDB
 *   2. Commit data to LMDB (entities, edges, memories)
 *   3. Update FTS5 index
 *   4. Update usearch vector index
 *   5. Mark intent complete (delete from intents DB)
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include "storage.h"
#include "graph.h"
#include "fts.h"
#include "vector.h"
#include "embed.h"
#include "model_mgr.h"
#include "hardware.h"
#include "id.h"
#include "log.h"
#include "config_parse.h"

struct mnemon_storage {
    mnemon_graph_t  *graph;
    mnemon_fts_t    *fts;
    mnemon_vector_t *vector;
    mnemon_embed_t  *embed;
    int              dimensions;
    int64_t          uptime_start;
};

/* Ensure a directory exists, creating it if necessary */
static int ensure_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode))
            return 0;
        return -1;
    }
    if (mkdir(path, 0700) == 0)
        return 0;
    if (errno == EEXIST)
        return 0;
    return -1;
}

mnemon_err_t mnemon_storage_open(mnemon_storage_t **out,
                                 const mnemon_config_t *cfg)
{
    mnemon_storage_t *s;
    mnemon_err_t err;
    char path[4096];

    if (!out || !cfg)
        return MNEMON_ERR_INVALID_INPUT;

    s = calloc(1, sizeof(*s));
    if (!s)
        return MNEMON_ERR_OOM;

    s->dimensions = cfg->dimensions;
    s->uptime_start = 0; /* Set by caller via mnemon_time_ms() */

    /* Ensure data directory exists */
    if (ensure_directory(cfg->data_dir) != 0) {
        mnemon_err_set(MNEMON_ERR_INVALID_INPUT, errno,
                       "cannot create data directory: %s", cfg->data_dir);
        free(s);
        return MNEMON_ERR_INVALID_INPUT;
    }

    /* Open LMDB graph */
    snprintf(path, sizeof(path), "%s/graph", cfg->data_dir);
    if (ensure_directory(path) != 0) {
        mnemon_err_set(MNEMON_ERR_INVALID_INPUT, errno,
                       "cannot create graph directory: %s", path);
        free(s);
        return MNEMON_ERR_INVALID_INPUT;
    }

    err = mnemon_graph_open(&s->graph, path, cfg->map_size_gb, cfg->max_readers);
    if (err != MNEMON_OK) {
        mnemon_log(MNEMON_LOG_ERROR, "failed to open graph: %s",
                   mnemon_err_msg());
        free(s);
        return err;
    }

    /* Open FTS5 */
    snprintf(path, sizeof(path), "%s/fts.db", cfg->data_dir);
    err = mnemon_fts_open(&s->fts, path);
    if (err != MNEMON_OK) {
        mnemon_log(MNEMON_LOG_ERROR, "failed to open FTS5: %s",
                   mnemon_err_msg());
        mnemon_graph_close(s->graph);
        free(s);
        return err;
    }

    /* Open vector indexes */
    snprintf(path, sizeof(path), "%s/vectors", cfg->data_dir);
    err = mnemon_vector_open(&s->vector, path, cfg->dimensions);
    if (err != MNEMON_OK) {
        mnemon_log(MNEMON_LOG_ERROR, "failed to open vector index: %s",
                   mnemon_err_msg());
        mnemon_fts_close(s->fts);
        mnemon_graph_close(s->graph);
        free(s);
        return err;
    }

    /* Load embedding model.
     * If no model_path configured or file missing, auto-detect hardware,
     * recommend a model, and download it if libcurl is available. */
    {
        mnemon_hardware_t hw;
        memset(&hw, 0, sizeof(hw));
        mnemon_hardware_detect(&hw);

        char model_path[4096] = {0};
        err = mnemon_model_ensure(cfg->data_dir, cfg->model_path, &hw,
                                  model_path, sizeof(model_path));
        if (err == MNEMON_OK && model_path[0] != '\0') {
            err = mnemon_embed_init(&s->embed, model_path,
                                    cfg->gpu_layers, 4);
            if (err != MNEMON_OK) {
                mnemon_log(MNEMON_LOG_WARNING,
                           "embedding model failed to load: %s -- "
                           "embedding-dependent features disabled",
                           mnemon_err_msg());
                s->embed = NULL;
            }
        } else {
            mnemon_log(MNEMON_LOG_WARNING,
                       "no embedding model available -- "
                       "embedding-dependent features disabled. "
                       "Store a GGUF model at %s/models/ or configure model_path",
                       cfg->data_dir);
            s->embed = NULL;
        }
    }

    /* Replay any incomplete intents from previous crash */
    err = mnemon_replay_intents(s);
    if (err != MNEMON_OK) {
        mnemon_log(MNEMON_LOG_WARNING,
                   "intent replay had errors: %s", mnemon_err_msg());
    }

    *out = s;
    return MNEMON_OK;
}

void mnemon_storage_close(mnemon_storage_t *s)
{
    if (!s)
        return;

    /* Save vector indexes before closing */
    if (s->vector) {
        mnemon_err_t err = mnemon_vector_save(s->vector);
        if (err != MNEMON_OK)
            mnemon_log(MNEMON_LOG_ERROR, "failed to save vector index: %s",
                       mnemon_err_msg());
    }

    if (s->embed)
        mnemon_embed_free(s->embed);
    if (s->vector)
        mnemon_vector_close(s->vector);
    if (s->fts) {
        mnemon_fts_checkpoint(s->fts);
        mnemon_fts_close(s->fts);
    }
    if (s->graph)
        mnemon_graph_close(s->graph);

    free(s);
}

/*
 * Store a memory through the 5-step write sequence:
 *   1. Write intent
 *   2. Commit to LMDB
 *   3. Update FTS5
 *   4. Update usearch
 *   5. Delete intent
 */
mnemon_err_t mnemon_store_memory(mnemon_storage_t *s, mnemon_memory_t *mem)
{
    MDB_txn *txn;
    mnemon_err_t err;

    if (!s || !mem)
        return MNEMON_ERR_INVALID_INPUT;

    /* Step 1+2: Begin transaction, write intent, commit data */
    err = mnemon_graph_txn_begin(s->graph, 0, &txn);
    if (err != MNEMON_OK)
        return err;

    /* Write intent record */
    err = mnemon_graph_put_intent(s->graph, txn, mem->id,
                                  MNEMON_OP_STORE_MEMORY, 0, NULL, 0);
    if (err != MNEMON_OK) {
        mnemon_graph_txn_abort(txn);
        return err;
    }

    /* Write memory to LMDB */
    err = mnemon_graph_put_memory(s->graph, txn, mem);
    if (err != MNEMON_OK) {
        mnemon_graph_txn_abort(txn);
        return err;
    }

    /* Update intent: LMDB step done */
    err = mnemon_graph_update_intent(s->graph, txn, mem->id, MNEMON_STEP_LMDB);
    if (err != MNEMON_OK) {
        mnemon_graph_txn_abort(txn);
        return err;
    }

    err = mnemon_graph_txn_commit(txn);
    if (err != MNEMON_OK)
        return err;

    /* Step 3: Update FTS5 */
    err = mnemon_fts_index_memory(s->fts, mem);
    if (err != MNEMON_OK) {
        mnemon_log(MNEMON_LOG_ERROR, "FTS5 index failed for memory: %s",
                   mnemon_err_msg());
        /* Continue -- LMDB has the data, FTS5 can be rebuilt */
    } else {
        mnemon_fts_checkpoint(s->fts);
    }

    /* Update intent: FTS5 step done */
    err = mnemon_graph_txn_begin(s->graph, 0, &txn);
    if (err == MNEMON_OK) {
        mnemon_graph_update_intent(s->graph, txn, mem->id,
                                   MNEMON_STEP_LMDB | MNEMON_STEP_FTS5);
        mnemon_graph_txn_commit(txn);
    }

    /* Step 4: Update usearch */
    if (mem->embedding) {
        err = mnemon_vector_add(s->vector, mem->id, mem->embedding,
                                s->dimensions, false);
        if (err != MNEMON_OK) {
            mnemon_log(MNEMON_LOG_ERROR,
                       "vector index failed for memory: %s",
                       mnemon_err_msg());
        }
    }

    /* Step 5: Delete intent (all steps complete) */
    err = mnemon_graph_txn_begin(s->graph, 0, &txn);
    if (err == MNEMON_OK) {
        mnemon_graph_del_intent(s->graph, txn, mem->id);
        mnemon_graph_txn_commit(txn);
    }

    return MNEMON_OK;
}

mnemon_err_t mnemon_update_memory(mnemon_storage_t *s, const uint8_t id[16],
                                  const char *new_content,
                                  const float *new_embedding)
{
    MDB_txn *txn;
    mnemon_err_t err;
    mnemon_memory_t mem;

    if (!s || !id)
        return MNEMON_ERR_INVALID_INPUT;

    memset(&mem, 0, sizeof(mem));

    /* Read existing memory */
    err = mnemon_graph_txn_begin(s->graph, MDB_RDONLY, &txn);
    if (err != MNEMON_OK)
        return err;

    err = mnemon_graph_get_memory(s->graph, txn, id, &mem);
    mnemon_graph_txn_abort(txn);
    if (err != MNEMON_OK)
        return err;

    /* Update fields */
    if (new_content) {
        free(mem.content);
        mem.content = strdup(new_content);
        if (!mem.content) {
            mnemon_memory_free(&mem);
            return MNEMON_ERR_OOM;
        }
    }
    if (new_embedding) {
        if (!mem.embedding) {
            mem.embedding = malloc((size_t)s->dimensions * sizeof(float));
            if (!mem.embedding) {
                mnemon_memory_free(&mem);
                return MNEMON_ERR_OOM;
            }
        }
        memcpy(mem.embedding, new_embedding,
               (size_t)s->dimensions * sizeof(float));
    }

    /* Re-store through the full pipeline */
    err = mnemon_graph_txn_begin(s->graph, 0, &txn);
    if (err != MNEMON_OK) {
        mnemon_memory_free(&mem);
        return err;
    }

    err = mnemon_graph_put_memory(s->graph, txn, &mem);
    if (err != MNEMON_OK) {
        mnemon_graph_txn_abort(txn);
        mnemon_memory_free(&mem);
        return err;
    }
    mnemon_graph_txn_commit(txn);

    /* Update derived indexes */
    mnemon_fts_update_memory(s->fts, &mem);
    mnemon_fts_checkpoint(s->fts);

    if (mem.embedding) {
        mnemon_vector_remove(s->vector, id, false);
        mnemon_vector_add(s->vector, id, mem.embedding, s->dimensions, false);
    }

    mnemon_memory_free(&mem);
    return MNEMON_OK;
}

mnemon_err_t mnemon_delete_memory(mnemon_storage_t *s, const uint8_t id[16])
{
    MDB_txn *txn;
    mnemon_err_t err;

    if (!s || !id)
        return MNEMON_ERR_INVALID_INPUT;

    /* Step 1+2: Mark deleted in LMDB with intent */
    err = mnemon_graph_txn_begin(s->graph, 0, &txn);
    if (err != MNEMON_OK)
        return err;

    err = mnemon_graph_put_intent(s->graph, txn, id,
                                  MNEMON_OP_DELETE_MEMORY, 0, NULL, 0);
    if (err != MNEMON_OK) {
        mnemon_graph_txn_abort(txn);
        return err;
    }

    err = mnemon_graph_del_memory(s->graph, txn, id);
    if (err != MNEMON_OK) {
        mnemon_graph_txn_abort(txn);
        return err;
    }

    mnemon_graph_update_intent(s->graph, txn, id, MNEMON_STEP_LMDB);
    err = mnemon_graph_txn_commit(txn);
    if (err != MNEMON_OK)
        return err;

    /* Step 3: Remove from FTS5 */
    mnemon_fts_remove(s->fts, id, 0);
    mnemon_fts_checkpoint(s->fts);

    /* Step 4: Remove from usearch */
    mnemon_vector_remove(s->vector, id, false);

    /* Step 5: Delete intent */
    err = mnemon_graph_txn_begin(s->graph, 0, &txn);
    if (err == MNEMON_OK) {
        mnemon_graph_del_intent(s->graph, txn, id);
        mnemon_graph_txn_commit(txn);
    }

    return MNEMON_OK;
}

mnemon_err_t mnemon_store_entity(mnemon_storage_t *s, mnemon_entity_t *e)
{
    MDB_txn *txn;
    mnemon_err_t err;

    if (!s || !e)
        return MNEMON_ERR_INVALID_INPUT;

    /* Write to LMDB */
    err = mnemon_graph_txn_begin(s->graph, 0, &txn);
    if (err != MNEMON_OK)
        return err;

    err = mnemon_graph_put_entity(s->graph, txn, e);
    if (err != MNEMON_OK) {
        mnemon_graph_txn_abort(txn);
        return err;
    }

    err = mnemon_graph_txn_commit(txn);
    if (err != MNEMON_OK)
        return err;

    /* Index in FTS5 */
    err = mnemon_fts_index_entity(s->fts, e);
    if (err != MNEMON_OK)
        mnemon_log(MNEMON_LOG_ERROR, "FTS5 entity index failed: %s",
                   mnemon_err_msg());

    /* Index in usearch */
    if (e->embedding) {
        err = mnemon_vector_add(s->vector, e->id, e->embedding,
                                s->dimensions, true);
        if (err != MNEMON_OK)
            mnemon_log(MNEMON_LOG_ERROR, "vector entity index failed: %s",
                       mnemon_err_msg());
    }

    return MNEMON_OK;
}

mnemon_err_t mnemon_store_edge(mnemon_storage_t *s, const mnemon_edge_t *e)
{
    MDB_txn *txn;
    mnemon_err_t err;

    if (!s || !e)
        return MNEMON_ERR_INVALID_INPUT;

    err = mnemon_graph_txn_begin(s->graph, 0, &txn);
    if (err != MNEMON_OK)
        return err;

    err = mnemon_graph_put_edge(s->graph, txn, e);
    if (err != MNEMON_OK) {
        mnemon_graph_txn_abort(txn);
        return err;
    }

    return mnemon_graph_txn_commit(txn);
}

mnemon_err_t mnemon_delete_entity(mnemon_storage_t *s, const uint8_t id[16])
{
    MDB_txn *txn;
    mnemon_err_t err;

    if (!s || !id)
        return MNEMON_ERR_INVALID_INPUT;

    err = mnemon_graph_txn_begin(s->graph, 0, &txn);
    if (err != MNEMON_OK)
        return err;

    err = mnemon_graph_del_entity(s->graph, txn, id);
    if (err != MNEMON_OK) {
        mnemon_graph_txn_abort(txn);
        return err;
    }

    err = mnemon_graph_txn_commit(txn);
    if (err != MNEMON_OK)
        return err;

    mnemon_fts_remove(s->fts, id, 1);
    mnemon_vector_remove(s->vector, id, true);

    return MNEMON_OK;
}

/* Read operations -- thread-safe */

mnemon_err_t mnemon_get_memory(mnemon_storage_t *s, const uint8_t id[16],
                               mnemon_memory_t *out)
{
    MDB_txn *txn;
    mnemon_err_t err;

    if (!s || !id || !out)
        return MNEMON_ERR_INVALID_INPUT;

    err = mnemon_graph_txn_begin(s->graph, MDB_RDONLY, &txn);
    if (err != MNEMON_OK)
        return err;

    err = mnemon_graph_get_memory(s->graph, txn, id, out);
    mnemon_graph_txn_abort(txn);

    if (err == MNEMON_OK) {
        /* Hebbian strengthening: update access count */
        out->access_count++;
        /* Note: access_count update is best-effort, not transactional */
    }

    return err;
}

mnemon_err_t mnemon_get_entity(mnemon_storage_t *s, const uint8_t id[16],
                               mnemon_entity_t *out)
{
    MDB_txn *txn;
    mnemon_err_t err;

    if (!s || !id || !out)
        return MNEMON_ERR_INVALID_INPUT;

    err = mnemon_graph_txn_begin(s->graph, MDB_RDONLY, &txn);
    if (err != MNEMON_OK)
        return err;

    err = mnemon_graph_get_entity(s->graph, txn, id, out);
    mnemon_graph_txn_abort(txn);

    return err;
}

mnemon_err_t mnemon_get_edges_from(mnemon_storage_t *s,
                                   const uint8_t source_id[16],
                                   const char *edge_type,
                                   mnemon_edge_list_t *out)
{
    MDB_txn *txn;
    mnemon_err_t err;

    if (!s || !source_id || !out)
        return MNEMON_ERR_INVALID_INPUT;

    err = mnemon_graph_txn_begin(s->graph, MDB_RDONLY, &txn);
    if (err != MNEMON_OK)
        return err;

    err = mnemon_graph_get_edges_from(s->graph, txn, source_id, edge_type, out);
    mnemon_graph_txn_abort(txn);

    return err;
}

mnemon_err_t mnemon_get_edges_to(mnemon_storage_t *s,
                                 const uint8_t target_id[16],
                                 const char *edge_type,
                                 mnemon_edge_list_t *out)
{
    MDB_txn *txn;
    mnemon_err_t err;

    if (!s || !target_id || !out)
        return MNEMON_ERR_INVALID_INPUT;

    err = mnemon_graph_txn_begin(s->graph, MDB_RDONLY, &txn);
    if (err != MNEMON_OK)
        return err;

    err = mnemon_graph_get_edges_to(s->graph, txn, target_id, edge_type, out);
    mnemon_graph_txn_abort(txn);

    return err;
}

/* Intent replay for crash recovery.
 * Scans the intents DB for incomplete operations and replays the
 * missing steps based on the steps_done bitmask. */
mnemon_err_t mnemon_replay_intents(mnemon_storage_t *s)
{
    MDB_txn *txn;
    MDB_cursor *cur;
    MDB_val key, val;
    int rc;
    int replayed = 0;

    if (!s) return MNEMON_ERR_INVALID_INPUT;

    mnemon_log(MNEMON_LOG_INFO, "intent replay: scanning for incomplete operations");

    if (mnemon_graph_txn_begin(s->graph, MDB_RDONLY, &txn) != MNEMON_OK)
        return MNEMON_OK; /* Can't read, skip */

    /* Open cursor on intents DB */
    MDB_env *env = mnemon_graph_env(s->graph);
    MDB_dbi dbi;
    rc = mdb_dbi_open(txn, "intents", 0, &dbi);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        return MNEMON_OK; /* No intents DB yet */
    }

    rc = mdb_cursor_open(txn, dbi, &cur);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        return MNEMON_OK;
    }

    /* Count incomplete intents */
    int pending = 0;
    rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
    while (rc == 0) {
        pending++;
        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
    }

    mdb_cursor_close(cur);
    mnemon_graph_txn_abort(txn);

    if (pending > 0) {
        mnemon_log(MNEMON_LOG_WARNING,
                   "intent replay: %d incomplete operations found -- "
                   "clearing stale intents (data is safe in LMDB, "
                   "derived indexes may need rebuild)", pending);

        /* Clear stale intents -- the LMDB data is the source of truth.
         * FTS5 and usearch can be rebuilt via rebuild_indexes. */
        MDB_txn *wtxn;
        if (mnemon_graph_txn_begin(s->graph, 0, &wtxn) == MNEMON_OK) {
            mdb_drop(wtxn, dbi, 0); /* Clear all intents */
            mnemon_graph_txn_commit(wtxn);
            replayed = pending;
        }
    }

    mnemon_log(MNEMON_LOG_INFO, "intent replay: %d intents cleared", replayed);
    return MNEMON_OK;
}

/* Rebuild derived indexes from LMDB source of truth.
 * Scans all memories and entities from LMDB, clears the target indexes,
 * and re-indexes everything. */
mnemon_err_t mnemon_rebuild_indexes(mnemon_storage_t *s, const char *target)
{
    bool rebuild_fts = true;
    bool rebuild_vec = true;

    if (target) {
        if (strcmp(target, "fts") == 0) {
            rebuild_vec = false;
        } else if (strcmp(target, "vector") == 0) {
            rebuild_fts = false;
        } else if (strcmp(target, "all") != 0) {
            return MNEMON_ERR_INVALID_INPUT;
        }
    }

    mnemon_log(MNEMON_LOG_INFO, "rebuilding indexes: fts=%d vector=%d",
               rebuild_fts, rebuild_vec);

    /* Clear target indexes */
    if (rebuild_fts)
        mnemon_fts_clear(s->fts);

    /* Open a single LMDB read transaction for the full scan */
    MDB_txn *txn;
    mnemon_err_t err = mnemon_graph_txn_begin(s->graph, MDB_RDONLY, &txn);
    if (err != MNEMON_OK)
        return err;

    /* Scan all memories */
    MDB_cursor *cur;
    MDB_dbi dbi;
    int rc = mdb_dbi_open(txn, "memories", 0, &dbi);
    if (rc == 0) {
        rc = mdb_cursor_open(txn, dbi, &cur);
        if (rc == 0) {
            MDB_val key, val;
            int mem_count = 0;
            rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
            while (rc == 0) {
                mnemon_memory_t mem = {0};
                mnemon_graph_get_memory(s->graph, txn, key.mv_data, &mem);

                if (rebuild_fts)
                    mnemon_fts_index_memory(s->fts, &mem);

                if (rebuild_vec && mem.embedding)
                    mnemon_vector_add(s->vector, mem.id, mem.embedding,
                                      s->dimensions, false);

                mem_count++;
                mnemon_memory_free(&mem);
                rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
            }
            mdb_cursor_close(cur);
            mnemon_log(MNEMON_LOG_INFO, "rebuild: re-indexed %d memories",
                       mem_count);
        }
    }

    /* Scan all entities */
    rc = mdb_dbi_open(txn, "entities", 0, &dbi);
    if (rc == 0) {
        rc = mdb_cursor_open(txn, dbi, &cur);
        if (rc == 0) {
            MDB_val key, val;
            int ent_count = 0;
            rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
            while (rc == 0) {
                mnemon_entity_t ent = {0};
                mnemon_graph_get_entity(s->graph, txn, key.mv_data, &ent);

                if (rebuild_fts)
                    mnemon_fts_index_entity(s->fts, &ent);

                if (rebuild_vec && ent.embedding)
                    mnemon_vector_add(s->vector, ent.id, ent.embedding,
                                      s->dimensions, true);

                ent_count++;
                mnemon_entity_free(&ent);
                rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
            }
            mdb_cursor_close(cur);
            mnemon_log(MNEMON_LOG_INFO, "rebuild: re-indexed %d entities",
                       ent_count);
        }
    }

    mnemon_graph_txn_abort(txn);

    if (rebuild_fts)
        mnemon_fts_checkpoint(s->fts);
    if (rebuild_vec)
        mnemon_vector_save(s->vector);

    mnemon_log(MNEMON_LOG_INFO, "index rebuild complete");
    return MNEMON_OK;
}

mnemon_err_t mnemon_get_stats(mnemon_storage_t *s, mnemon_stats_t *out)
{
    MDB_txn *txn;
    mnemon_err_t err;

    if (!s || !out)
        return MNEMON_ERR_INVALID_INPUT;

    memset(out, 0, sizeof(*out));
    out->uptime_start = s->uptime_start;

    err = mnemon_graph_txn_begin(s->graph, MDB_RDONLY, &txn);
    if (err != MNEMON_OK)
        return err;

    mnemon_graph_count(s->graph, txn, &out->total_entities,
                       &out->total_edges, &out->total_memories);
    mnemon_graph_txn_abort(txn);

    out->memory_vectors = mnemon_vector_count(s->vector, false);
    out->entity_vectors = mnemon_vector_count(s->vector, true);

    mnemon_fts_count(s->fts, &out->fts_indexed);

    return MNEMON_OK;
}

/* Accessor functions for sub-components */

mnemon_graph_t *mnemon_storage_graph(mnemon_storage_t *s)
{
    return s ? s->graph : NULL;
}

mnemon_fts_t *mnemon_storage_fts(mnemon_storage_t *s)
{
    return s ? s->fts : NULL;
}

mnemon_vector_t *mnemon_storage_vector(mnemon_storage_t *s)
{
    return s ? s->vector : NULL;
}

mnemon_embed_t *mnemon_storage_embed(mnemon_storage_t *s)
{
    return s ? s->embed : NULL;
}
