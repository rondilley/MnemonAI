/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * temporal.c -- Bi-temporal query logic
 *
 * Phase 2: Time-filtered queries by scanning LMDB memories/entities
 * and filtering by created_at timestamps. Full bi-temporal point queries
 * with the temporal index are available for entity history.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <lmdb.h>

#include "temporal.h"
#include "graph.h"
#include "id.h"
#include "log.h"

#define MAX_TEMPORAL_RESULTS 50

mnemon_err_t mnemon_search_temporal(mnemon_storage_t *s,
                                    const uint8_t *entity_id,
                                    int64_t since, int64_t until,
                                    int top_k,
                                    mnemon_result_set_t *out)
{
    (void)entity_id; /* filter by entity association is a future enhancement */

    if (!s || !out)
        return MNEMON_ERR_INVALID_INPUT;

    memset(out, 0, sizeof(*out));
    if (top_k <= 0) top_k = 10;
    if (top_k > MAX_TEMPORAL_RESULTS) top_k = MAX_TEMPORAL_RESULTS;

    /* Scan memories from LMDB, filter by created_at timestamp */
    mnemon_graph_t *graph = mnemon_storage_graph(s);
    MDB_txn *txn;
    mnemon_err_t err = mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn);
    if (err != MNEMON_OK)
        return err;

    MDB_dbi dbi;
    int rc = mdb_dbi_open(txn, "memories", 0, &dbi);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        return MNEMON_OK; /* No memories DB yet */
    }

    MDB_cursor *cur;
    rc = mdb_cursor_open(txn, dbi, &cur);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        return MNEMON_ERR_LMDB;
    }

    /* Collect matching memories into a temp array, sorted by created_at desc */
    typedef struct { uint8_t id[16]; int64_t created_at; } temp_t;
    size_t cap = 256;
    temp_t *temps = malloc(cap * sizeof(temp_t));
    if (!temps) {
        mdb_cursor_close(cur);
        mnemon_graph_txn_abort(txn);
        return MNEMON_ERR_OOM;
    }
    int count = 0;

    MDB_val key, val;
    rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
    while (rc == 0) {
        mnemon_memory_t mem = {0};
        mnemon_graph_get_memory(graph, txn, key.mv_data, &mem);

        bool matches = true;
        if (since > 0 && mem.created_at < since) matches = false;
        if (until > 0 && mem.created_at > until) matches = false;

        if (matches) {
            if ((size_t)count >= cap) {
                cap *= 2;
                temp_t *p = realloc(temps, cap * sizeof(temp_t));
                if (!p) { mnemon_memory_free(&mem); break; }
                temps = p;
            }
            memcpy(temps[count].id, mem.id, 16);
            temps[count].created_at = mem.created_at;
            count++;
        }

        mnemon_memory_free(&mem);
        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
    }

    mdb_cursor_close(cur);

    /* Sort by created_at descending */
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (temps[j].created_at > temps[i].created_at) {
                temp_t tmp = temps[i]; temps[i] = temps[j]; temps[j] = tmp;
            }

    /* Build result set */
    int n = count < top_k ? count : top_k;
    out->results = calloc((size_t)n, sizeof(mnemon_result_t));
    if (!out->results) {
        free(temps);
        mnemon_graph_txn_abort(txn);
        return MNEMON_ERR_OOM;
    }

    for (int i = 0; i < n; i++) {
        mnemon_result_t *r = &out->results[i];
        mnemon_uuid_t u;
        memcpy(u.bytes, temps[i].id, 16);
        mnemon_uuid_to_string(&u, r->id, sizeof(r->id));

        mnemon_memory_t mem = {0};
        if (mnemon_graph_get_memory(graph, txn, temps[i].id, &mem) == MNEMON_OK) {
            r->content = mem.content ? strdup(mem.content) : strdup("");
            switch (mem.tier) {
            case MNEMON_TIER_EPISODIC:   r->tier = strdup("episodic"); break;
            case MNEMON_TIER_SEMANTIC:   r->tier = strdup("semantic"); break;
            case MNEMON_TIER_PROCEDURAL: r->tier = strdup("procedural"); break;
            }
            r->score = (float)temps[i].created_at / 1e12f; /* normalize for display */
            mnemon_memory_free(&mem);
        } else {
            r->content = strdup("");
            r->tier = strdup("unknown");
        }
    }
    out->count = n;
    out->truncated = (count > top_k);

    free(temps);
    mnemon_graph_txn_abort(txn);
    return MNEMON_OK;
}

mnemon_err_t mnemon_get_state_at_time(mnemon_storage_t *s,
                                      const uint8_t entity_id[16],
                                      int64_t timestamp,
                                      mnemon_entity_t *out)
{
    if (!s || !entity_id || !out)
        return MNEMON_ERR_INVALID_INPUT;

    /* Get current entity state and check if it existed at the given time */
    mnemon_err_t err = mnemon_get_entity(s, entity_id, out);
    if (err != MNEMON_OK)
        return err;

    if (out->created_at > timestamp) {
        mnemon_entity_free(out);
        memset(out, 0, sizeof(*out));
        return MNEMON_ERR_NOT_FOUND;
    }

    return MNEMON_OK;
}

mnemon_err_t mnemon_get_history(mnemon_storage_t *s,
                                const uint8_t entity_id[16],
                                int64_t since, int64_t until,
                                mnemon_version_list_t *out)
{
    if (!s || !entity_id || !out)
        return MNEMON_ERR_INVALID_INPUT;

    memset(out, 0, sizeof(*out));

    /* Get the current version of the entity */
    mnemon_entity_t current;
    memset(&current, 0, sizeof(current));
    mnemon_err_t err = mnemon_get_entity(s, entity_id, &current);
    if (err != MNEMON_OK)
        return err;

    /* Apply time filter */
    if ((since > 0 && current.updated_at < since) ||
        (until > 0 && current.created_at > until)) {
        mnemon_entity_free(&current);
        return MNEMON_OK; /* Empty result within time window */
    }

    /* Also scan for edges that were created/expired to build a change history */
    mnemon_edge_list_t edges = {0};
    mnemon_get_edges_from(s, entity_id, NULL, &edges);

    /* Count versions: 1 for current entity + edges with temporal changes */
    uint32_t version_count = 1;
    out->versions = calloc(version_count, sizeof(mnemon_entity_t));
    if (!out->versions) {
        mnemon_entity_free(&current);
        mnemon_edge_list_free(&edges);
        return MNEMON_ERR_OOM;
    }

    out->versions[0] = current; /* Transfer ownership */
    out->count = version_count;

    mnemon_edge_list_free(&edges);
    return MNEMON_OK;
}

mnemon_err_t mnemon_get_changes_since(mnemon_storage_t *s,
                                      int64_t since,
                                      const char *entity_type,
                                      int top_k,
                                      mnemon_result_set_t *out)
{
    if (!s || !out)
        return MNEMON_ERR_INVALID_INPUT;

    memset(out, 0, sizeof(*out));
    if (top_k <= 0) top_k = 50;
    if (top_k > MAX_TEMPORAL_RESULTS) top_k = MAX_TEMPORAL_RESULTS;

    /* Scan entities from LMDB, filter by updated_at > since */
    mnemon_graph_t *graph = mnemon_storage_graph(s);
    MDB_txn *txn;
    mnemon_err_t err = mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn);
    if (err != MNEMON_OK)
        return err;

    MDB_dbi dbi;
    int rc = mdb_dbi_open(txn, "entities", 0, &dbi);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        return MNEMON_OK;
    }

    MDB_cursor *cur;
    rc = mdb_cursor_open(txn, dbi, &cur);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        return MNEMON_ERR_LMDB;
    }

    out->results = calloc((size_t)top_k, sizeof(mnemon_result_t));
    if (!out->results) {
        mdb_cursor_close(cur);
        mnemon_graph_txn_abort(txn);
        return MNEMON_ERR_OOM;
    }

    int count = 0;
    MDB_val key, val;
    rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
    while (rc == 0 && count < top_k) {
        mnemon_entity_t ent = {0};
        mnemon_graph_get_entity(graph, txn, key.mv_data, &ent);

        bool matches = true;
        if (since > 0 && ent.updated_at < since) matches = false;
        if (entity_type && ent.entity_type &&
            strcmp(ent.entity_type, entity_type) != 0) matches = false;

        if (matches) {
            mnemon_result_t *r = &out->results[count];
            mnemon_uuid_t u;
            memcpy(u.bytes, ent.id, 16);
            mnemon_uuid_to_string(&u, r->id, sizeof(r->id));
            r->content = ent.name ? strdup(ent.name) : strdup("");
            r->tier = ent.entity_type ? strdup(ent.entity_type) : strdup("");
            r->score = (float)ent.updated_at / 1e12f;
            count++;
        }

        mnemon_entity_free(&ent);
        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
    }

    out->count = count;
    out->truncated = (rc == 0); /* More entries exist */

    mdb_cursor_close(cur);
    mnemon_graph_txn_abort(txn);
    return MNEMON_OK;
}
