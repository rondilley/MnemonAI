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
#include <pthread.h>

#include "search.h"
#include "graph.h"
#include "fts.h"
#include "vector.h"
#include "embed.h"
#include "storage.h"
#include "threads.h"
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

/* Per-ranker thread argument and result */
typedef struct {
    mnemon_storage_t     *storage;
    const mnemon_query_t *query;
    const float          *query_emb;
    int                   dimensions;
    fusion_entry_t       *fusion;
    int                  *fusion_count;
    int                   fusion_capacity;
    pthread_mutex_t      *fusion_mutex;
    mnemon_err_t          err;
} ranker_arg_t;

/* Vector ranker thread function.
 *
 * The memory vector index contains both whole-memory embeddings and
 * per-chunk embeddings.  When a chunk UUID is returned, we resolve it
 * to the parent memory UUID so the fusion table works with memory IDs.
 * For multiple chunks from the same parent, keep the best (lowest rank). */
static void *vector_ranker_fn(void *arg)
{
    ranker_arg_t *ra = (ranker_arg_t *)arg;
    ra->err = MNEMON_OK;

    if (!ra->query_emb) return NULL;

    mnemon_vector_t *vec = mnemon_storage_vector(ra->storage);
    mnemon_vector_results_t vr;
    memset(&vr, 0, sizeof(vr));

    ra->err = mnemon_vector_search(vec, ra->query_emb, ra->dimensions,
                                   MAX_RANKER_RESULTS, false, &vr);
    if (ra->err == MNEMON_OK && vr.count > 0) {
        /* Open a read txn for chunk resolution */
        mnemon_graph_t *graph = mnemon_storage_graph(ra->storage);
        MDB_txn *txn = NULL;
        mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn);

        pthread_mutex_lock(ra->fusion_mutex);
        for (int i = 0; i < vr.count; i++) {
            uint8_t resolved_id[16];
            memcpy(resolved_id, vr.results[i].id, 16);

            /* Check if this is a chunk UUID and resolve to parent */
            if (txn) {
                mnemon_chunk_meta_t cm;
                if (mnemon_graph_get_chunk(graph, txn, vr.results[i].id,
                                           &cm) == MNEMON_OK) {
                    memcpy(resolved_id, cm.parent_id, 16);
                }
            }

            fusion_entry_t *fe = fusion_find_or_insert(
                ra->fusion, ra->fusion_count, ra->fusion_capacity,
                resolved_id);
            if (fe) {
                /* Keep the best rank (lowest i) for this parent */
                if (fe->vector_rank == 0 || i + 1 < fe->vector_rank) {
                    fe->vector_rank = i + 1;
                    fe->vector_score = 1.0f - vr.results[i].distance;
                }
            }
        }
        pthread_mutex_unlock(ra->fusion_mutex);

        if (txn) mnemon_graph_txn_abort(txn);
        mnemon_vector_results_free(&vr);
    }
    return NULL;
}

/* Keyword ranker thread function */
static void *keyword_ranker_fn(void *arg)
{
    ranker_arg_t *ra = (ranker_arg_t *)arg;
    ra->err = MNEMON_OK;

    if (!ra->query->query_text) return NULL;

    mnemon_fts_t *fts = mnemon_storage_fts(ra->storage);
    mnemon_fts_results_t fr;
    memset(&fr, 0, sizeof(fr));

    ra->err = mnemon_fts_search(fts, ra->query->query_text,
                                MAX_RANKER_RESULTS, &fr);
    if (ra->err == MNEMON_OK && fr.count > 0) {
        pthread_mutex_lock(ra->fusion_mutex);
        for (int i = 0; i < fr.count; i++) {
            if (fr.results[i].source_type != 0)
                continue; /* Skip entity results for memory search */
            fusion_entry_t *fe = fusion_find_or_insert(
                ra->fusion, ra->fusion_count, ra->fusion_capacity,
                fr.results[i].id);
            if (fe) {
                fe->keyword_rank = i + 1;
                fe->keyword_score = fr.results[i].score;
            }
        }
        pthread_mutex_unlock(ra->fusion_mutex);
        mnemon_fts_results_free(&fr);
    }
    return NULL;
}

/* Graph ranker thread function */
static void *graph_ranker_fn(void *arg)
{
    ranker_arg_t *ra = (ranker_arg_t *)arg;
    ra->err = MNEMON_OK;

    if (!ra->query_emb) return NULL;

    /* Search entity vector index for semantically relevant entities */
    mnemon_vector_t *vec = mnemon_storage_vector(ra->storage);
    mnemon_vector_results_t er;
    memset(&er, 0, sizeof(er));

    ra->err = mnemon_vector_search(vec, ra->query_emb, ra->dimensions,
                                   5, true, &er);
    if (ra->err == MNEMON_OK && er.count > 0) {
        /* BFS from top entities to find related memories */
        mnemon_graph_t *graph = mnemon_storage_graph(ra->storage);
        MDB_txn *txn;
        ra->err = mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn);
        if (ra->err == MNEMON_OK) {
            int graph_rank = 1;
            for (int ei = 0; ei < er.count; ei++) {
                /* Get edges from this entity to find connected memories */
                mnemon_edge_list_t edges;
                memset(&edges, 0, sizeof(edges));
                mnemon_err_t edge_err = mnemon_graph_get_edges_from(
                    graph, txn, er.results[ei].id, NULL, &edges);
                if (edge_err == MNEMON_OK) {
                    pthread_mutex_lock(ra->fusion_mutex);
                    for (uint32_t j = 0; j < edges.count && graph_rank <= MAX_RANKER_RESULTS; j++) {
                        fusion_entry_t *fe = fusion_find_or_insert(
                            ra->fusion, ra->fusion_count, ra->fusion_capacity,
                            edges.edges[j].target_id);
                        if (fe && fe->graph_rank == 0) {
                            fe->graph_rank = graph_rank++;
                            fe->graph_score = 1.0f / (float)(ei + 1);
                        }
                    }
                    pthread_mutex_unlock(ra->fusion_mutex);
                    mnemon_edge_list_free(&edges);
                }
            }
            mnemon_graph_txn_abort(txn);
        }
        mnemon_vector_results_free(&er);
    }
    return NULL;
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

/* Thin wrappers matching reader_task_fn (void(*)(void*)) signature */
static void vector_ranker_task(void *arg) { vector_ranker_fn(arg); }
static void keyword_ranker_task(void *arg) { keyword_ranker_fn(arg); }

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
                                query_emb, dimensions, true);
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

    /* --- Dispatch rankers in parallel --- */
    pthread_mutex_t fusion_mutex;
    pthread_mutex_init(&fusion_mutex, NULL);

    ranker_arg_t varg = {
        .storage = s, .query = q, .query_emb = query_emb,
        .dimensions = dimensions, .fusion = fusion,
        .fusion_count = &fusion_count, .fusion_capacity = fusion_capacity,
        .fusion_mutex = &fusion_mutex, .err = MNEMON_OK
    };
    ranker_arg_t karg = varg;
    ranker_arg_t garg = varg;

    /* Wrapper for reader pool task dispatch */
    mnemon_reader_pool_t *pool = mnemon_storage_reader_pool(s);

    if (pool) {
        /* Use persistent reader pool */
        reader_task_t vec_task = {0}, kw_task = {0};
        bool vec_submitted = false, kw_submitted = false;

        if (query_emb) {
            vec_task.fn = vector_ranker_task;
            vec_task.arg = &varg;
            if (mnemon_reader_pool_submit(pool, &vec_task) == MNEMON_OK)
                vec_submitted = true;
            else
                vector_ranker_fn(&varg);
        }
        if (q->query_text) {
            kw_task.fn = keyword_ranker_task;
            kw_task.arg = &karg;
            if (mnemon_reader_pool_submit(pool, &kw_task) == MNEMON_OK)
                kw_submitted = true;
            else
                keyword_ranker_fn(&karg);
        }

        /* Run graph ranker on the calling thread */
        graph_ranker_fn(&garg);

        if (vec_submitted) mnemon_reader_task_wait(&vec_task);
        if (kw_submitted)  mnemon_reader_task_wait(&kw_task);
    } else {
        /* Fallback: ad-hoc thread creation */
        pthread_t vec_thread, kw_thread;
        bool vec_started = false, kw_started = false;

        if (query_emb) {
            if (pthread_create(&vec_thread, NULL, vector_ranker_fn, &varg) == 0)
                vec_started = true;
            else
                vector_ranker_fn(&varg);
        }
        if (q->query_text) {
            if (pthread_create(&kw_thread, NULL, keyword_ranker_fn, &karg) == 0)
                kw_started = true;
            else
                keyword_ranker_fn(&karg);
        }

        graph_ranker_fn(&garg);

        if (vec_started) pthread_join(vec_thread, NULL);
        if (kw_started)  pthread_join(kw_thread, NULL);
    }

    pthread_mutex_destroy(&fusion_mutex);

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
                                query_emb, dimensions, true);
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

    /* The vector index holds per-chunk embeddings alongside whole-memory ones,
     * so a hit may be a chunk UUID. Resolve it to the parent memory UUID (as
     * the hybrid ranker does), dedup parents, and drop any vector that does not
     * resolve to a live memory -- otherwise orphaned/chunk vectors surface as
     * ghost results with empty content and tier "unknown". */
    mnemon_graph_t *graph = mnemon_storage_graph(s);
    MDB_txn *txn = NULL;
    mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn);

    int n = 0;
    for (int i = 0; i < vr.count; i++) {
        uint8_t mem_id[16];
        memcpy(mem_id, vr.results[i].id, 16);
        if (txn) {
            mnemon_chunk_meta_t cm;
            if (mnemon_graph_get_chunk(graph, txn, vr.results[i].id, &cm)
                == MNEMON_OK)
                memcpy(mem_id, cm.parent_id, 16);
        }

        mnemon_uuid_t uuid;
        memcpy(uuid.bytes, mem_id, 16);
        char id_str[37];
        mnemon_uuid_to_string(&uuid, id_str, sizeof(id_str));

        /* Dedup: a parent reached via several chunks appears once, best-first. */
        bool dup = false;
        for (int j = 0; j < n; j++)
            if (strcmp(out->results[j].id, id_str) == 0) { dup = true; break; }
        if (dup)
            continue;

        /* Read through the already-open txn -- opening a second read txn on
         * this thread (via mnemon_get_memory) would fail under LMDB's per-
         * thread reader slot and drop every result. */
        mnemon_memory_t mem;
        memset(&mem, 0, sizeof(mem));
        mnemon_err_t ge = txn
            ? mnemon_graph_get_memory(graph, txn, mem_id, &mem)
            : mnemon_get_memory(s, mem_id, &mem);
        if (ge != MNEMON_OK) {
            mnemon_memory_free(&mem);
            continue; /* orphaned vector -- skip, do not emit a ghost */
        }

        mnemon_result_t *r = &out->results[n];
        memcpy(r->id, id_str, sizeof(id_str));
        r->vector_score = 1.0f - vr.results[i].distance;
        r->score = r->vector_score;
        r->content = mem.content ? strdup(mem.content) : strdup("");
        switch (mem.tier) {
        case MNEMON_TIER_EPISODIC:   r->tier = strdup("episodic"); break;
        case MNEMON_TIER_SEMANTIC:   r->tier = strdup("semantic"); break;
        case MNEMON_TIER_PROCEDURAL: r->tier = strdup("procedural"); break;
        }
        mnemon_memory_free(&mem);
        n++;
    }

    if (txn)
        mnemon_graph_txn_abort(txn);

    out->count = n;
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
