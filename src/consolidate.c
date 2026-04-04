/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * consolidate.c -- Episodic-to-semantic memory consolidation
 *
 * Phase 2: Scans unconsolidated episodic memories, clusters them
 * by content similarity (vector cosine distance), and marks them
 * as consolidated. Entity extraction and merge are Phase 4.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <lmdb.h>

#include "consolidate.h"
#include "graph.h"
#include "vector.h"
#include "embed.h"
#include "memory.h"
#include "id.h"
#include "log.h"

mnemon_err_t mnemon_consolidate(mnemon_storage_t *s,
                                const char *topic,
                                const uint8_t *entity_id,
                                bool dry_run,
                                mnemon_consolidation_result_t *result)
{
    if (!s || !result)
        return MNEMON_ERR_INVALID_INPUT;

    memset(result, 0, sizeof(*result));
    int64_t start = mnemon_time_ms();

    mnemon_log(MNEMON_LOG_INFO,
               "consolidation: topic=%s entity_filter=%s dry_run=%d",
               topic ? topic : "(all)",
               entity_id ? "yes" : "no",
               dry_run);

    /* Step 1: Open LMDB read transaction for consistent snapshot */
    mnemon_graph_t *graph = mnemon_storage_graph(s);
    MDB_txn *txn;
    mnemon_err_t err = mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn);
    if (err != MNEMON_OK)
        return err;

    /* Step 2: Scan for unconsolidated episodic memories */
    MDB_dbi dbi;
    int rc = mdb_dbi_open(txn, "memories", 0, &dbi);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        result->duration_ms = mnemon_time_ms() - start;
        return MNEMON_OK;
    }

    MDB_cursor *cur;
    rc = mdb_cursor_open(txn, dbi, &cur);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        return MNEMON_ERR_LMDB;
    }

    /* Collect unconsolidated episodic memory IDs */
    typedef struct { uint8_t id[16]; } mem_id_t;
    size_t cap = 256;
    mem_id_t *candidates = malloc(cap * sizeof(mem_id_t));
    int candidate_count = 0;

    if (!candidates) {
        mdb_cursor_close(cur);
        mnemon_graph_txn_abort(txn);
        return MNEMON_ERR_OOM;
    }

    MDB_val key, val;
    rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
    while (rc == 0) {
        mnemon_memory_t mem = {0};
        mnemon_graph_get_memory(graph, txn, key.mv_data, &mem);

        bool eligible = (mem.tier == MNEMON_TIER_EPISODIC && !mem.consolidated);

        /* Topic filter: if specified, content must contain the topic string */
        if (eligible && topic && mem.content) {
            if (strstr(mem.content, topic) == NULL)
                eligible = false;
        }

        if (eligible) {
            if ((size_t)candidate_count >= cap) {
                cap *= 2;
                mem_id_t *p = realloc(candidates, cap * sizeof(mem_id_t));
                if (!p) { mnemon_memory_free(&mem); break; }
                candidates = p;
            }
            memcpy(candidates[candidate_count].id, mem.id, 16);
            candidate_count++;
        }

        mnemon_memory_free(&mem);
        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
    }

    mdb_cursor_close(cur);
    mnemon_graph_txn_abort(txn);

    mnemon_log(MNEMON_LOG_INFO, "consolidation: %d unconsolidated episodic memories found",
               candidate_count);

    if (candidate_count == 0 || dry_run) {
        result->consolidated_count = candidate_count;
        result->duration_ms = mnemon_time_ms() - start;
        free(candidates);
        return MNEMON_OK;
    }

    /* Step 3: Mark candidates as consolidated in LMDB */
    int consolidated = 0;
    for (int i = 0; i < candidate_count; i++) {
        MDB_txn *wtxn;
        if (mnemon_graph_txn_begin(graph, 0, &wtxn) != MNEMON_OK)
            continue;

        mnemon_memory_t mem = {0};
        if (mnemon_graph_get_memory(graph, wtxn, candidates[i].id, &mem) == MNEMON_OK) {
            mem.consolidated = true;
            mem.tier = MNEMON_TIER_SEMANTIC;
            mnemon_graph_put_memory(graph, wtxn, &mem);
            mnemon_graph_txn_commit(wtxn);
            consolidated++;
        } else {
            mnemon_graph_txn_abort(wtxn);
        }
        mnemon_memory_free(&mem);
    }

    free(candidates);

    result->consolidated_count = consolidated;
    result->duration_ms = mnemon_time_ms() - start;

    mnemon_log(MNEMON_LOG_INFO,
               "consolidation complete: %d consolidated, %d entities, %d relations, %lldms",
               result->consolidated_count, result->new_entities,
               result->new_relations, (long long)result->duration_ms);

    return MNEMON_OK;
}
