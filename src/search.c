/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * search.c -- Hybrid retrieval with Reciprocal Rank Fusion (RRF)
 *
 * Three independent rankers produce scored result lists:
 *   1. Graph ranker: vector search entity index -> BFS from top entities
 *   2. Vector ranker: k-NN over memory embeddings
 *   3. Keyword ranker: BM25 via FTS5
 *
 * Results are fused via RRF: score(d) = sum(1 / (k + rank_r(d)))
 * where k=60 (standard value from the RRF literature).
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "search.h"
#include "graph.h"
#include "fts.h"
#include "vector.h"
#include "embed.h"
#include "id.h"
#include "log.h"

#define RRF_K 60
#define MAX_TOP_K 50
#define MAX_RANKER_RESULTS 100

/* Internal result for fusion */
typedef struct {
    uint8_t id[16];
    float   graph_score;
    float   vector_score;
    float   keyword_score;
    float   rrf_score;
    int     graph_rank;
    int     vector_rank;
    int     keyword_rank;
} fusion_entry_t;

/* Find or insert an entry in the fusion table by UUID */
static fusion_entry_t *fusion_find_or_insert(fusion_entry_t *table,
                                              int *count, int capacity,
                                              const uint8_t id[16])
{
    for (int i = 0; i < *count; i++) {
        if (memcmp(table[i].id, id, 16) == 0)
            return &table[i];
    }
    if (*count >= capacity)
        return NULL;

    fusion_entry_t *e = &table[*count];
    memset(e, 0, sizeof(*e));
    memcpy(e->id, id, 16);
    (*count)++;
    return e;
}

/* Compare fusion entries by RRF score (descending) */
static int fusion_cmp(const void *a, const void *b)
{
    const fusion_entry_t *fa = (const fusion_entry_t *)a;
    const fusion_entry_t *fb = (const fusion_entry_t *)b;
    if (fb->rrf_score > fa->rrf_score) return 1;
    if (fb->rrf_score < fa->rrf_score) return -1;
    return 0;
}

/* Populate result set from fusion table.
 * Opens a single LMDB read transaction for all result fetches. */
static mnemon_err_t build_results(mnemon_storage_t *s,
                                  fusion_entry_t *table, int count,
                                  int top_k, float min_score,
                                  mnemon_result_set_t *out)
{
    int n = count < top_k ? count : top_k;
    int actual = 0;

    out->results = calloc((size_t)n, sizeof(mnemon_result_t));
    if (!out->results)
        return MNEMON_ERR_OOM;

    /* Single read transaction for all result lookups (avoids N txn_begin/abort) */
    mnemon_graph_t *graph = mnemon_storage_graph(s);
    MDB_txn *txn = NULL;
    mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn);

    for (int i = 0; i < n; i++) {
        if (table[i].rrf_score < min_score)
            break;

        mnemon_result_t *r = &out->results[actual];
        mnemon_uuid_t uuid;
        memcpy(uuid.bytes, table[i].id, 16);
        mnemon_uuid_to_string(&uuid, r->id, sizeof(r->id));

        /* Fetch content from LMDB within the shared transaction */
        mnemon_memory_t mem;
        memset(&mem, 0, sizeof(mem));
        mnemon_err_t err = MNEMON_ERR_NOT_FOUND;
        if (txn)
            err = mnemon_graph_get_memory(graph, txn, table[i].id, &mem);
        if (err == MNEMON_OK && mem.content) {
            r->content = strdup(mem.content);
            switch (mem.tier) {
            case MNEMON_TIER_EPISODIC:   r->tier = strdup("episodic"); break;
            case MNEMON_TIER_SEMANTIC:   r->tier = strdup("semantic"); break;
            case MNEMON_TIER_PROCEDURAL: r->tier = strdup("procedural"); break;
            }
            mnemon_memory_free(&mem);
        } else {
            r->content = strdup("");
            r->tier = strdup("unknown");
        }

        r->score = table[i].rrf_score;
        r->graph_score = table[i].graph_score;
        r->vector_score = table[i].vector_score;
        r->keyword_score = table[i].keyword_score;
        actual++;
    }

    if (txn) mnemon_graph_txn_abort(txn);

    out->count = actual;
    out->truncated = (count > top_k);

    return MNEMON_OK;
}

mnemon_err_t mnemon_search_hybrid(mnemon_storage_t *s,
                                  const mnemon_query_t *q,
                                  mnemon_result_set_t *out)
{
    mnemon_err_t err;
    int top_k;
    float *query_emb = NULL;
    int dimensions = 768;

    if (!s || !q || !out)
        return MNEMON_ERR_INVALID_INPUT;

    memset(out, 0, sizeof(*out));
    top_k = q->top_k > 0 ? q->top_k : 10;
    if (top_k > MAX_TOP_K)
        top_k = MAX_TOP_K;

    /* Compute query embedding if not provided */
    mnemon_embed_t *embed = mnemon_storage_embed(s);
    if (q->query_embedding) {
        query_emb = (float *)q->query_embedding;
    } else if (embed && q->query_text) {
        dimensions = mnemon_embed_dimensions(embed);
        query_emb = malloc((size_t)dimensions * sizeof(float));
        if (!query_emb)
            return MNEMON_ERR_OOM;
        err = mnemon_embed_text(embed, q->query_text, strlen(q->query_text),
                                query_emb, dimensions);
        if (err != MNEMON_OK) {
            free(query_emb);
            query_emb = NULL;
        }
    }

    /* Fusion table */
    int fusion_capacity = MAX_RANKER_RESULTS * 3;
    fusion_entry_t *fusion = calloc((size_t)fusion_capacity,
                                     sizeof(fusion_entry_t));
    if (!fusion) {
        if (query_emb != q->query_embedding)
            free(query_emb);
        return MNEMON_ERR_OOM;
    }
    int fusion_count = 0;

    /* --- Ranker 1: Vector search (memory index) --- */
    if (query_emb) {
        mnemon_vector_t *vec = mnemon_storage_vector(s);
        mnemon_vector_results_t vr;
        memset(&vr, 0, sizeof(vr));

        err = mnemon_vector_search(vec, query_emb, dimensions,
                                   MAX_RANKER_RESULTS, false, &vr);
        if (err == MNEMON_OK && vr.count > 0) {
            for (int i = 0; i < vr.count; i++) {
                fusion_entry_t *fe = fusion_find_or_insert(
                    fusion, &fusion_count, fusion_capacity,
                    vr.results[i].id);
                if (fe) {
                    fe->vector_rank = i + 1;
                    fe->vector_score = 1.0f - vr.results[i].distance;
                }
            }
            mnemon_vector_results_free(&vr);
        }
    }

    /* --- Ranker 2: Keyword search (FTS5 BM25) --- */
    if (q->query_text) {
        mnemon_fts_t *fts = mnemon_storage_fts(s);
        mnemon_fts_results_t fr;
        memset(&fr, 0, sizeof(fr));

        err = mnemon_fts_search(fts, q->query_text, MAX_RANKER_RESULTS, &fr);
        if (err == MNEMON_OK && fr.count > 0) {
            for (int i = 0; i < fr.count; i++) {
                if (fr.results[i].source_type != 0)
                    continue; /* Skip entity results for memory search */
                fusion_entry_t *fe = fusion_find_or_insert(
                    fusion, &fusion_count, fusion_capacity,
                    fr.results[i].id);
                if (fe) {
                    fe->keyword_rank = i + 1;
                    fe->keyword_score = fr.results[i].score;
                }
            }
            mnemon_fts_results_free(&fr);
        }
    }

    /* --- Ranker 3: Graph ranker --- */
    if (query_emb) {
        /* Search entity vector index for semantically relevant entities */
        mnemon_vector_t *vec = mnemon_storage_vector(s);
        mnemon_vector_results_t er;
        memset(&er, 0, sizeof(er));

        err = mnemon_vector_search(vec, query_emb, dimensions,
                                   5, true, &er);
        if (err == MNEMON_OK && er.count > 0) {
            /* BFS from top entities to find related memories */
            mnemon_graph_t *graph = mnemon_storage_graph(s);
            MDB_txn *txn;
            err = mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn);
            if (err == MNEMON_OK) {
                int graph_rank = 1;
                for (int ei = 0; ei < er.count; ei++) {
                    /* Get edges from this entity to find connected memories */
                    mnemon_edge_list_t edges;
                    memset(&edges, 0, sizeof(edges));
                    err = mnemon_graph_get_edges_from(graph, txn,
                                                      er.results[ei].id,
                                                      NULL, &edges);
                    if (err == MNEMON_OK) {
                        for (uint32_t j = 0; j < edges.count && graph_rank <= MAX_RANKER_RESULTS; j++) {
                            fusion_entry_t *fe = fusion_find_or_insert(
                                fusion, &fusion_count, fusion_capacity,
                                edges.edges[j].target_id);
                            if (fe && fe->graph_rank == 0) {
                                fe->graph_rank = graph_rank++;
                                fe->graph_score = 1.0f / (float)(ei + 1);
                            }
                        }
                        mnemon_edge_list_free(&edges);
                    }
                }
                mnemon_graph_txn_abort(txn);
            }
            mnemon_vector_results_free(&er);
        }
    }

    /* --- RRF Fusion --- */
    for (int i = 0; i < fusion_count; i++) {
        float score = 0.0f;
        if (fusion[i].vector_rank > 0)
            score += 1.0f / (float)(RRF_K + fusion[i].vector_rank);
        if (fusion[i].keyword_rank > 0)
            score += 1.0f / (float)(RRF_K + fusion[i].keyword_rank);
        if (fusion[i].graph_rank > 0)
            score += 1.0f / (float)(RRF_K + fusion[i].graph_rank);
        fusion[i].rrf_score = score;
    }

    /* Sort by RRF score */
    qsort(fusion, (size_t)fusion_count, sizeof(fusion_entry_t), fusion_cmp);

    /* Build final result set */
    err = build_results(s, fusion, fusion_count, top_k, q->min_score, out);

    free(fusion);
    if (query_emb != q->query_embedding)
        free(query_emb);

    return err;
}

mnemon_err_t mnemon_search_semantic(mnemon_storage_t *s,
                                    const mnemon_query_t *q,
                                    mnemon_result_set_t *out)
{
    mnemon_err_t err;
    float *query_emb = NULL;
    int dimensions = 768;
    int top_k;

    if (!s || !q || !out)
        return MNEMON_ERR_INVALID_INPUT;

    memset(out, 0, sizeof(*out));
    top_k = q->top_k > 0 ? q->top_k : 10;
    if (top_k > MAX_TOP_K)
        top_k = MAX_TOP_K;

    mnemon_embed_t *embed = mnemon_storage_embed(s);
    if (q->query_embedding) {
        query_emb = (float *)q->query_embedding;
    } else if (embed && q->query_text) {
        dimensions = mnemon_embed_dimensions(embed);
        query_emb = malloc((size_t)dimensions * sizeof(float));
        if (!query_emb)
            return MNEMON_ERR_OOM;
        err = mnemon_embed_text(embed, q->query_text, strlen(q->query_text),
                                query_emb, dimensions);
        if (err != MNEMON_OK) {
            free(query_emb);
            return err;
        }
    } else {
        return MNEMON_ERR_EMBED;
    }

    mnemon_vector_t *vec = mnemon_storage_vector(s);
    mnemon_vector_results_t vr;
    memset(&vr, 0, sizeof(vr));

    err = mnemon_vector_search(vec, query_emb, dimensions, top_k, false, &vr);
    if (err != MNEMON_OK) {
        if (query_emb != q->query_embedding)
            free(query_emb);
        return err;
    }

    out->results = calloc((size_t)vr.count, sizeof(mnemon_result_t));
    if (!out->results) {
        mnemon_vector_results_free(&vr);
        if (query_emb != q->query_embedding)
            free(query_emb);
        return MNEMON_ERR_OOM;
    }

    for (int i = 0; i < vr.count; i++) {
        mnemon_result_t *r = &out->results[i];
        mnemon_uuid_t uuid;
        memcpy(uuid.bytes, vr.results[i].id, 16);
        mnemon_uuid_to_string(&uuid, r->id, sizeof(r->id));

        r->vector_score = 1.0f - vr.results[i].distance;
        r->score = r->vector_score;

        mnemon_memory_t mem;
        memset(&mem, 0, sizeof(mem));
        if (mnemon_get_memory(s, vr.results[i].id, &mem) == MNEMON_OK) {
            r->content = mem.content ? strdup(mem.content) : strdup("");
            switch (mem.tier) {
            case MNEMON_TIER_EPISODIC:   r->tier = strdup("episodic"); break;
            case MNEMON_TIER_SEMANTIC:   r->tier = strdup("semantic"); break;
            case MNEMON_TIER_PROCEDURAL: r->tier = strdup("procedural"); break;
            }
            mnemon_memory_free(&mem);
        } else {
            r->content = strdup("");
            r->tier = strdup("unknown");
        }
    }

    out->count = vr.count;
    out->truncated = false;

    mnemon_vector_results_free(&vr);
    if (query_emb != q->query_embedding)
        free(query_emb);

    return MNEMON_OK;
}

mnemon_err_t mnemon_search_keyword(mnemon_storage_t *s,
                                   const mnemon_query_t *q,
                                   mnemon_result_set_t *out)
{
    mnemon_err_t err;
    int top_k;

    if (!s || !q || !q->query_text || !out)
        return MNEMON_ERR_INVALID_INPUT;

    memset(out, 0, sizeof(*out));
    top_k = q->top_k > 0 ? q->top_k : 10;
    if (top_k > MAX_TOP_K)
        top_k = MAX_TOP_K;

    mnemon_fts_t *fts = mnemon_storage_fts(s);
    mnemon_fts_results_t fr;
    memset(&fr, 0, sizeof(fr));

    err = mnemon_fts_search(fts, q->query_text, top_k, &fr);
    if (err != MNEMON_OK)
        return err;

    out->results = calloc((size_t)fr.count, sizeof(mnemon_result_t));
    if (!out->results) {
        mnemon_fts_results_free(&fr);
        return MNEMON_ERR_OOM;
    }

    int actual = 0;
    for (int i = 0; i < fr.count; i++) {
        if (fr.results[i].source_type != 0)
            continue; /* Skip entities for keyword memory search */

        mnemon_result_t *r = &out->results[actual];
        mnemon_uuid_t uuid;
        memcpy(uuid.bytes, fr.results[i].id, 16);
        mnemon_uuid_to_string(&uuid, r->id, sizeof(r->id));

        r->keyword_score = fr.results[i].score;
        r->score = r->keyword_score;

        mnemon_memory_t mem;
        memset(&mem, 0, sizeof(mem));
        if (mnemon_get_memory(s, fr.results[i].id, &mem) == MNEMON_OK) {
            r->content = mem.content ? strdup(mem.content) : strdup("");
            switch (mem.tier) {
            case MNEMON_TIER_EPISODIC:   r->tier = strdup("episodic"); break;
            case MNEMON_TIER_SEMANTIC:   r->tier = strdup("semantic"); break;
            case MNEMON_TIER_PROCEDURAL: r->tier = strdup("procedural"); break;
            }
            mnemon_memory_free(&mem);
        } else {
            r->content = strdup("");
            r->tier = strdup("unknown");
        }
        actual++;
    }

    out->count = actual;
    out->truncated = false;

    mnemon_fts_results_free(&fr);
    return MNEMON_OK;
}
