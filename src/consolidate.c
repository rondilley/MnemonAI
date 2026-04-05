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
#include <math.h>

#include <lmdb.h>

#include "consolidate.h"
#include "storage.h"
#include "graph.h"
#include "vector.h"
#include "embed.h"
#include "hardware.h"
#include "memory.h"
#include "id.h"
#include "log.h"

/* Entity deduplication entry for merge tracking */
typedef struct {
    uint8_t id[16];
    char   *name;
    char   *entity_type;
} entity_ref_t;

/* Cosine distance between two embeddings using SIMD dispatch.
 * Returns 0.0 = identical, 2.0 = opposite. */
static float embedding_distance(const float *a, const float *b, int dims)
{
    if (g_simd_ops.cosine_distance && a && b)
        return g_simd_ops.cosine_distance(a, b, (size_t)dims);
    /* Scalar fallback */
    float dot = 0, na = 0, nb = 0;
    for (int i = 0; i < dims; i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    float denom = sqrtf(na) * sqrtf(nb);
    return (denom > 1e-8f) ? (1.0f - dot / denom) : 1.0f;
}

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

    /* Step 3: Cluster candidates by embedding similarity (SIMD-accelerated).
     * Load embeddings for all candidates, then group by cosine similarity
     * within the configurable threshold (default 0.7 = distance < 0.3). */
    {
        /* Load embeddings */
        MDB_txn *rtxn;
        mnemon_graph_txn_begin(graph, MDB_RDONLY, &rtxn);

        float **embeddings = calloc((size_t)candidate_count, sizeof(float *));
        int *cluster_id = calloc((size_t)candidate_count, sizeof(int));
        int dims = 768; /* default embedding dimension */
        int num_clusters = 0;

        if (embeddings && cluster_id) {
            for (int i = 0; i < candidate_count; i++) {
                mnemon_memory_t mem = {0};
                if (mnemon_graph_get_memory(graph, rtxn, candidates[i].id, &mem) == MNEMON_OK
                    && mem.embedding) {
                    embeddings[i] = malloc((size_t)dims * sizeof(float));
                    if (embeddings[i])
                        memcpy(embeddings[i], mem.embedding, (size_t)dims * sizeof(float));
                }
                mnemon_memory_free(&mem);
                cluster_id[i] = -1; /* unassigned */
            }

            /* Single-pass greedy clustering: assign each memory to the first
             * cluster whose centroid is within threshold distance, or create
             * a new cluster. Uses g_simd_ops.cosine_distance for the
             * comparison (AVX-512 on this system). */
            float threshold = 0.3f; /* cosine distance < 0.3 = similarity > 0.7 */

            for (int i = 0; i < candidate_count; i++) {
                if (!embeddings[i]) { cluster_id[i] = num_clusters++; continue; }

                /* Compare against existing cluster representatives */
                bool assigned = false;
                for (int j = 0; j < i; j++) {
                    if (cluster_id[j] != cluster_id[j]) continue; /* not a rep */
                    if (!embeddings[j]) continue;

                    float dist = embedding_distance(embeddings[i], embeddings[j], dims);
                    if (dist < threshold) {
                        cluster_id[i] = cluster_id[j];
                        assigned = true;
                        break;
                    }
                }
                if (!assigned)
                    cluster_id[i] = num_clusters++;
            }

            mnemon_log(MNEMON_LOG_INFO, "consolidation: %d clusters from %d candidates",
                       num_clusters, candidate_count);

            /* Clean up embeddings */
            for (int i = 0; i < candidate_count; i++)
                free(embeddings[i]);
        }
        free(embeddings);
        free(cluster_id);

        if (rtxn) mnemon_graph_txn_abort(rtxn);
    }

    /* Step 4: Entity merge -- deduplicate entities referenced by consolidated
     * memories. Entities with matching name+type are merged: observations
     * from duplicates are appended to the canonical entity, and duplicate
     * entity records are deleted. */
    {
        /* Collect all unique entity references from candidate memories */
        size_t ref_cap = 64;
        entity_ref_t *refs = calloc(ref_cap, sizeof(entity_ref_t));
        int ref_count = 0;

        MDB_txn *rtxn;
        mnemon_graph_txn_begin(graph, MDB_RDONLY, &rtxn);

        if (refs && rtxn) {
            for (int i = 0; i < candidate_count; i++) {
                mnemon_memory_t mem = {0};
                if (mnemon_graph_get_memory(graph, rtxn, candidates[i].id, &mem) != MNEMON_OK) {
                    mnemon_memory_free(&mem);
                    continue;
                }
                for (uint32_t ei = 0; ei < mem.entity_id_count; ei++) {
                    /* Check if already in refs */
                    bool found = false;
                    for (int ri = 0; ri < ref_count; ri++) {
                        if (memcmp(refs[ri].id, mem.entity_ids[ei], 16) == 0) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        if ((size_t)ref_count >= ref_cap) {
                            ref_cap *= 2;
                            entity_ref_t *p = realloc(refs, ref_cap * sizeof(entity_ref_t));
                            if (!p) break;
                            refs = p;
                        }
                        mnemon_entity_t ent = {0};
                        if (mnemon_graph_get_entity(graph, rtxn, mem.entity_ids[ei], &ent) == MNEMON_OK) {
                            memcpy(refs[ref_count].id, ent.id, 16);
                            refs[ref_count].name = ent.name ? strdup(ent.name) : NULL;
                            refs[ref_count].entity_type = ent.entity_type ? strdup(ent.entity_type) : NULL;
                            ref_count++;
                            mnemon_entity_free(&ent);
                        }
                    }
                }
                mnemon_memory_free(&mem);
            }
        }
        if (rtxn) mnemon_graph_txn_abort(rtxn);

        /* Find entities with matching name+type and merge them */
        for (int i = 0; i < ref_count; i++) {
            if (!refs[i].name) continue;
            for (int j = i + 1; j < ref_count; j++) {
                if (!refs[j].name) continue;
                if (strcmp(refs[i].name, refs[j].name) != 0) continue;
                /* Type must also match (or both NULL) */
                if (refs[i].entity_type && refs[j].entity_type &&
                    strcmp(refs[i].entity_type, refs[j].entity_type) != 0)
                    continue;

                /* Merge j into i: append observations from j to i */
                mnemon_entity_t canonical = {0}, dup = {0};
                if (mnemon_get_entity(s, refs[i].id, &canonical) == MNEMON_OK &&
                    mnemon_get_entity(s, refs[j].id, &dup) == MNEMON_OK) {

                    /* Append dup observations to canonical */
                    if (dup.observation_count > 0) {
                        uint32_t new_count = canonical.observation_count + dup.observation_count;
                        char **new_obs = realloc(canonical.observations,
                                                  new_count * sizeof(char *));
                        if (new_obs) {
                            for (uint32_t oi = 0; oi < dup.observation_count; oi++)
                                new_obs[canonical.observation_count + oi] =
                                    strdup(dup.observations[oi]);
                            canonical.observations = new_obs;
                            canonical.observation_count = new_count;
                        }
                    }
                    canonical.updated_at = mnemon_time_ms();
                    mnemon_store_entity(s, &canonical);

                    /* Delete the duplicate entity */
                    mnemon_delete_entity(s, refs[j].id);
                    result->new_entities++;

                    mnemon_log(MNEMON_LOG_INFO,
                        "consolidation: merged entity \"%s\" (duplicate removed)",
                        refs[i].name);
                }
                mnemon_entity_free(&canonical);
                mnemon_entity_free(&dup);

                /* Mark j as consumed */
                free(refs[j].name);
                refs[j].name = NULL;
                free(refs[j].entity_type);
                refs[j].entity_type = NULL;
            }
        }

        /* Cleanup refs */
        for (int i = 0; i < ref_count; i++) {
            free(refs[i].name);
            free(refs[i].entity_type);
        }
        free(refs);
    }

    /* Step 5: Mark candidates as consolidated in LMDB */
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
