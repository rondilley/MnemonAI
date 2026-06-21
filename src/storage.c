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
#include <ctype.h>
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
#include "memory.h"
#include "config_parse.h"

typedef struct mnemon_reader_pool mnemon_reader_pool_t;

struct mnemon_storage {
    mnemon_graph_t      *graph;
    mnemon_fts_t        *fts;
    mnemon_vector_t     *vector;
    mnemon_embed_t      *embed;
    mnemon_honeypot_t   *honeypot;
    mnemon_reader_pool_t *reader_pool;
    int                  dimensions;
    int64_t              uptime_start;
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

    /* One-shot migration: backfill creation-time version baselines for entities
     * that predate bi-temporal versioning. Guarded by a versioned meta flag so
     * it runs once per algorithm version. The backfill is idempotent (skips
     * entities already anchored at created_at), but the flag avoids re-scanning
     * every entity on every startup. Bump BACKFILL_VERSION when the backfill
     * logic changes so the corrected pass re-runs. (v2: fixed a valid_from bump
     * that mis-dated baselines for entities with a later snapshot. v3: prune the
     * mis-keyed baseline duplicates that v1 left behind.) */
    {
        const char *BACKFILL_VERSION = "3";
        MDB_txn *mtxn;
        char flag[8] = {0};
        bool current = false;
        if (mnemon_graph_txn_begin(s->graph, MDB_RDONLY, &mtxn) == MNEMON_OK) {
            if (mnemon_graph_get_meta(s->graph, mtxn, "versions_backfilled",
                                      flag, sizeof(flag)) == MNEMON_OK)
                current = (strcmp(flag, BACKFILL_VERSION) == 0);
            mnemon_graph_txn_abort(mtxn);
        }
        if (!current) {
            /* Remove mis-keyed backfill duplicates (valid_from far from the
             * snapshot's own updated_at), then backfill any still-missing
             * creation baselines. 1s tolerance keeps legitimate same-ms bumps. */
            if (mnemon_graph_txn_begin(s->graph, 0, &mtxn) == MNEMON_OK) {
                size_t pruned = 0;
                mnemon_graph_prune_miskeyed_versions(s->graph, mtxn, 1000,
                                                     &pruned);
                mnemon_graph_txn_commit(mtxn);
                if (pruned)
                    mnemon_log(MNEMON_LOG_INFO,
                               "version cleanup: pruned %zu mis-keyed baselines",
                               pruned);
            }
            size_t n = 0;
            mnemon_backfill_versions(s, &n);
            if (mnemon_graph_txn_begin(s->graph, 0, &mtxn) == MNEMON_OK) {
                mnemon_graph_put_meta(s->graph, mtxn, "versions_backfilled",
                                      BACKFILL_VERSION);
                mnemon_graph_txn_commit(mtxn);
            }
        }
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

/* ------------------------------------------------------------------ */
/* Chunked vector indexing                                             */
/* ------------------------------------------------------------------ */

/* Minimum content length to trigger chunking.  Short memories get a
 * single whole-memory embedding which is already effective. */
#define CHUNK_MIN_CONTENT  800

/* Target chunk size in bytes.  Chosen so each chunk tokenizes well
 * within the 2048-token context of nomic-embed-text-v1.5. */
#define CHUNK_TARGET_SIZE  800

/* Split content into chunks at conversation turn boundaries.
 * Returns number of chunks written into out[] (caller-allocated).
 * Each chunk stores byte_offset and byte_length into the original content. */
static int chunk_text(const char *content, size_t content_len,
                      mnemon_chunk_meta_t *out, int max_chunks)
{
    int n = 0;
    size_t pos = 0;

    while (pos < content_len && n < max_chunks) {
        /* Find the end of this chunk: look for a turn boundary near the
         * target size, or split at the target if no boundary found. */
        size_t end = pos + CHUNK_TARGET_SIZE;
        if (end >= content_len) {
            end = content_len;
        } else {
            /* Look for a turn boundary ("\nUser: " or "\nAssistant: ")
             * in the region [target-200 .. target+200] */
            size_t search_start = end > 200 ? end - 200 : pos;
            size_t search_end = end + 200;
            if (search_end > content_len) search_end = content_len;
            size_t best = 0;
            for (size_t i = search_start; i < search_end; i++) {
                if (content[i] == '\n') {
                    if (i + 6 < content_len &&
                        memcmp(content + i + 1, "User:", 5) == 0) {
                        best = i + 1;  /* split before "User:" */
                        break;
                    }
                    if (i + 11 < content_len &&
                        memcmp(content + i + 1, "Assistant:", 10) == 0) {
                        best = i + 1;
                        break;
                    }
                }
            }
            if (best > pos)
                end = best;
        }

        /* Skip empty chunks */
        if (end > pos) {
            memset(&out[n], 0, sizeof(out[n]));
            out[n].sequence = (uint32_t)n;
            out[n].byte_offset = (uint32_t)pos;
            out[n].byte_length = (uint32_t)(end - pos);
            n++;
        }
        pos = end;
    }
    return n;
}

/* Store chunk embeddings for a memory.  Called after the memory itself
 * is stored.  Generates one embedding per chunk and stores:
 *   - Chunk metadata in LMDB (chunk_uuid -> parent_uuid + offset)
 *   - Chunk embedding in the memory vector index */
static void store_chunks(mnemon_storage_t *s, const mnemon_memory_t *mem)
{
    mnemon_embed_t *embed = s->embed;
    if (!embed || !mnemon_embed_available(embed))
        return;
    if (!mem->content)
        return;

    size_t content_len = strlen(mem->content);
    if (content_len < CHUNK_MIN_CONTENT)
        return;

    int dims = mnemon_embed_dimensions(embed);
    mnemon_chunk_meta_t chunks[128];
    int nchunks = chunk_text(mem->content, content_len, chunks, 128);
    if (nchunks <= 1)
        return;  /* single chunk = whole memory embedding is sufficient */

    float *emb = malloc((size_t)dims * sizeof(float));
    if (!emb) return;

    for (int i = 0; i < nchunks; i++) {
        /* Generate chunk UUID */
        mnemon_uuid_t cu;
        mnemon_uuid_generate(&cu);
        memcpy(chunks[i].id, cu.bytes, 16);
        memcpy(chunks[i].parent_id, mem->id, 16);

        /* Embed the chunk text */
        const char *chunk_start = mem->content + chunks[i].byte_offset;
        size_t chunk_len = chunks[i].byte_length;
        if (mnemon_embed_text(embed, chunk_start, chunk_len,
                              emb, dims, false) != MNEMON_OK)
            continue;

        /* Store chunk metadata in LMDB */
        MDB_txn *txn;
        if (mnemon_graph_txn_begin(s->graph, 0, &txn) == MNEMON_OK) {
            if (mnemon_graph_put_chunk(s->graph, txn, &chunks[i]) == MNEMON_OK)
                mnemon_graph_txn_commit(txn);
            else
                mnemon_graph_txn_abort(txn);
        }

        /* Store chunk embedding in vector index */
        mnemon_vector_add(s->vector, chunks[i].id, emb, dims, false);
    }

    free(emb);
}

/* Upper bound on chunks per memory -- matches the chunks[] buffer in
 * store_chunks(), so a parent never has more chunks than this. */
#define MAX_CHUNKS_PER_PARENT 128

/* Remove every chunk vector and chunk-metadata row belonging to a parent
 * memory.  Without this, deleting a chunked memory leaves its chunk vectors
 * orphaned in the usearch index (they outlive the parent forever). */
static void delete_chunks_for_parent(mnemon_storage_t *s,
                                     const uint8_t parent_id[16])
{
    uint8_t chunk_ids[MAX_CHUNKS_PER_PARENT][16];
    size_t n = 0;

    MDB_txn *rtxn;
    if (mnemon_graph_txn_begin(s->graph, MDB_RDONLY, &rtxn) != MNEMON_OK)
        return;
    mnemon_graph_get_chunks_by_parent(s->graph, rtxn, parent_id,
                                      chunk_ids, MAX_CHUNKS_PER_PARENT, &n);
    mnemon_graph_txn_abort(rtxn);

    if (n == 0)
        return;

    /* Drop chunk vectors from the index. */
    for (size_t i = 0; i < n; i++)
        mnemon_vector_remove(s->vector, chunk_ids[i], false);

    /* Drop chunk metadata from LMDB. */
    MDB_txn *wtxn;
    if (mnemon_graph_txn_begin(s->graph, 0, &wtxn) == MNEMON_OK) {
        for (size_t i = 0; i < n; i++)
            mnemon_graph_del_chunk(s->graph, wtxn, chunk_ids[i]);
        mnemon_graph_txn_commit(wtxn);
    }
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

    /* Step 4b: Store chunk embeddings for long content */
    store_chunks(s, mem);

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

    /* Content changed -> chunk boundaries and text changed, so the old chunk
     * vectors/metadata are stale.  Drop them and re-chunk from the new
     * content, mirroring store_memory().  (When only the embedding changed,
     * chunk text is unchanged, so re-chunking is unnecessary.) */
    if (new_content) {
        delete_chunks_for_parent(s, id);
        store_chunks(s, &mem);
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

    /* Step 4: Remove from usearch (whole-memory vector + any chunk vectors) */
    mnemon_vector_remove(s->vector, id, false);
    delete_chunks_for_parent(s, id);

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

    /* Record an immutable version snapshot in the same transaction so entity
     * history is atomic with the write (bi-temporal versioning). */
    {
        mnemon_uuid_t vu;
        mnemon_uuid_generate(&vu);
        mnemon_graph_put_version(s->graph, txn, vu.bytes, e);
    }

    err = mnemon_graph_txn_commit(txn);
    if (err != MNEMON_OK)
        return err;

    /* Index in FTS5. Remove any prior entity doc first so re-stores (e.g.
     * add_observation) replace rather than duplicate the FTS entry. */
    mnemon_fts_remove(s->fts, e->id, 1);
    err = mnemon_fts_index_entity(s->fts, e);
    if (err != MNEMON_OK)
        mnemon_log(MNEMON_LOG_ERROR, "FTS5 entity index failed: %s",
                   mnemon_err_msg());

    /* Index in usearch (remove-then-add so re-stores replace, not duplicate). */
    if (e->embedding) {
        mnemon_vector_remove(s->vector, e->id, true);
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

/* Case-insensitive whole-word/phrase containment: does haystack contain needle
 * bounded by non-alphanumeric characters (or string ends)? Avoids "Ron"
 * matching inside "Bronson". */
static bool content_mentions(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen < 3) return false;
    const char *p = haystack;
    while ((p = strcasestr(p, needle)) != NULL) {
        bool left_ok  = (p == haystack) || !isalnum((unsigned char)p[-1]);
        char after    = p[nlen];
        bool right_ok = (after == '\0') || !isalnum((unsigned char)after);
        if (left_ok && right_ok)
            return true;
        p += 1;
    }
    return false;
}

/* Connect entities to the memories that mention them by name, creating
 * "mentioned_in" edges (entity -> memory). Idempotent: existing edges are
 * skipped, so it can be re-run as memories/entities grow. */
mnemon_err_t mnemon_link_entities(mnemon_storage_t *s, size_t *created)
{
    if (!s) return MNEMON_ERR_INVALID_INPUT;
    if (created) *created = 0;

    typedef struct { uint8_t id[16]; char *name; } ent_t;
    ent_t *ents = NULL;
    size_t nent = 0, ecap = 0;

    MDB_txn *txn;
    if (mnemon_graph_txn_begin(s->graph, MDB_RDONLY, &txn) != MNEMON_OK)
        return MNEMON_ERR_LMDB;

    /* Collect entities (id + name >= 3 chars). */
    MDB_dbi dbi;
    if (mdb_dbi_open(txn, "entities", 0, &dbi) == 0) {
        MDB_cursor *cur;
        if (mdb_cursor_open(txn, dbi, &cur) == 0) {
            MDB_val k, v;
            int rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST);
            while (rc == 0) {
                mnemon_entity_t e = {0};
                if (mnemon_graph_get_entity(s->graph, txn, k.mv_data, &e)
                        == MNEMON_OK && e.name && strlen(e.name) >= 3) {
                    if (nent >= ecap) {
                        ecap = ecap ? ecap * 2 : 64;
                        ent_t *p = realloc(ents, ecap * sizeof(ent_t));
                        if (!p) { mnemon_entity_free(&e); break; }
                        ents = p;
                    }
                    memcpy(ents[nent].id, e.id, 16);
                    ents[nent].name = strdup(e.name);
                    nent++;
                }
                mnemon_entity_free(&e);
                rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
            }
            mdb_cursor_close(cur);
        }
    }

    /* Scan memories; record (entity, memory) mention pairs. */
    typedef struct { size_t ei; uint8_t mid[16]; } pair_t;
    pair_t *pairs = NULL;
    size_t np = 0, pcap = 0;
    if (mdb_dbi_open(txn, "memories", 0, &dbi) == 0) {
        MDB_cursor *cur;
        if (mdb_cursor_open(txn, dbi, &cur) == 0) {
            MDB_val k, v;
            int rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST);
            while (rc == 0) {
                mnemon_memory_t m = {0};
                if (mnemon_graph_get_memory(s->graph, txn, k.mv_data, &m)
                        == MNEMON_OK && m.content) {
                    for (size_t i = 0; i < nent; i++) {
                        if (content_mentions(m.content, ents[i].name)) {
                            if (np >= pcap) {
                                pcap = pcap ? pcap * 2 : 256;
                                pair_t *p = realloc(pairs, pcap * sizeof(pair_t));
                                if (!p) break;
                                pairs = p;
                            }
                            pairs[np].ei = i;
                            memcpy(pairs[np].mid, m.id, 16);
                            np++;
                        }
                    }
                }
                mnemon_memory_free(&m);
                rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
            }
            mdb_cursor_close(cur);
        }
    }
    mnemon_graph_txn_abort(txn);

    /* Create mentioned_in edges (skip any that already exist). */
    size_t made = 0;
    MDB_txn *wtxn;
    if (mnemon_graph_txn_begin(s->graph, 0, &wtxn) == MNEMON_OK) {
        int64_t now = mnemon_time_ms();
        for (size_t p = 0; p < np; p++) {
            const uint8_t *src = ents[pairs[p].ei].id;
            if (mnemon_graph_edge_exists(s->graph, wtxn, src, "mentioned_in",
                                         pairs[p].mid))
                continue;
            mnemon_edge_t e = {0};
            mnemon_uuid_t eu;
            mnemon_uuid_generate(&eu);
            memcpy(e.id, eu.bytes, 16);
            memcpy(e.source_id, src, 16);
            memcpy(e.target_id, pairs[p].mid, 16);
            e.edge_type = "mentioned_in";   /* literal: put_edge copies it */
            e.weight = 1.0f;
            e.valid_from = now;
            e.created_at = now;
            if (mnemon_graph_put_edge(s->graph, wtxn, &e) == MNEMON_OK)
                made++;
        }
        mnemon_graph_txn_commit(wtxn);
    }

    for (size_t i = 0; i < nent; i++)
        free(ents[i].name);
    free(ents);
    free(pairs);

    if (created) *created = made;
    mnemon_log(MNEMON_LOG_INFO, "link_entities: created %zu mention edges", made);
    return MNEMON_OK;
}

/* Backfill a baseline version snapshot at each existing entity's created_at,
 * for entities that predate bi-temporal versioning (or were only versioned
 * recently). Without this, get_state_at_time for a date before the entity's
 * first recorded snapshot falls back to current state instead of returning a
 * creation-anchored version. The baseline captures the entity's CURRENT state
 * (the only data available) dated at created_at -- it anchors the timeline at
 * true creation rather than reconstructing intermediate history. Idempotent:
 * an entity that already has a snapshot at/before created_at is skipped.
 * *count receives the number of baselines written. */
mnemon_err_t mnemon_backfill_versions(mnemon_storage_t *s, size_t *count)
{
    if (!s) return MNEMON_ERR_INVALID_INPUT;
    if (count) *count = 0;

    /* Collect entity IDs up front (snapshot writes use their own write txns). */
    uint8_t (*ids)[16] = NULL;
    size_t n = 0, cap = 0;

    MDB_txn *rtxn;
    if (mnemon_graph_txn_begin(s->graph, MDB_RDONLY, &rtxn) != MNEMON_OK)
        return MNEMON_ERR_LMDB;
    MDB_dbi dbi;
    if (mdb_dbi_open(rtxn, "entities", 0, &dbi) == 0) {
        MDB_cursor *cur;
        if (mdb_cursor_open(rtxn, dbi, &cur) == 0) {
            MDB_val k, v;
            int rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST);
            while (rc == 0) {
                if (k.mv_size == 16) {
                    if (n >= cap) {
                        cap = cap ? cap * 2 : 128;
                        uint8_t (*p)[16] = realloc(ids, cap * 16);
                        if (!p) break;
                        ids = p;
                    }
                    memcpy(ids[n++], k.mv_data, 16);
                }
                rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
            }
            mdb_cursor_close(cur);
        }
    }
    mnemon_graph_txn_abort(rtxn);

    size_t made = 0;
    for (size_t i = 0; i < n; i++) {
        MDB_txn *wtxn;
        if (mnemon_graph_txn_begin(s->graph, 0, &wtxn) != MNEMON_OK)
            continue;

        mnemon_entity_t e = {0};
        if (mnemon_graph_get_entity(s->graph, wtxn, ids[i], &e) != MNEMON_OK) {
            mnemon_entity_free(&e);
            mnemon_graph_txn_abort(wtxn);
            continue;
        }

        /* Skip if a snapshot already anchors this entity at/before creation. */
        mnemon_entity_t *vers = NULL;
        uint32_t vn = 0;
        mnemon_graph_load_versions(s->graph, wtxn, ids[i], 0, e.created_at,
                                   &vers, &vn);
        bool have_baseline = (vn > 0);
        for (uint32_t j = 0; j < vn; j++)
            mnemon_entity_free(&vers[j]);
        free(vers);

        if (!have_baseline && e.created_at > 0) {
            /* Write the baseline at created_at (not updated_at) so the index
             * key lands on the entity's creation time. */
            mnemon_entity_t baseline = e;
            baseline.updated_at = e.created_at; /* put_version keys on this */
            mnemon_uuid_t vu;
            mnemon_uuid_generate(&vu);
            if (mnemon_graph_put_version(s->graph, wtxn, vu.bytes, &baseline)
                    == MNEMON_OK)
                made++;
        }

        mnemon_entity_free(&e);
        mnemon_graph_txn_commit(wtxn);
    }

    free(ids);
    if (count) *count = made;
    mnemon_log(MNEMON_LOG_INFO,
               "version backfill: wrote %zu creation-time baselines", made);
    return MNEMON_OK;
}

/* Lowercase + trim a name into buf for duplicate grouping. */
static void normalize_name(const char *name, char *buf, size_t buflen)
{
    size_t n = 0;
    const char *p = name;
    while (*p && isspace((unsigned char)*p)) p++;       /* leading trim */
    const char *end = p + strlen(p);
    while (end > p && isspace((unsigned char)end[-1])) end--; /* trailing */
    for (; p < end && n + 1 < buflen; p++)
        buf[n++] = (char)tolower((unsigned char)*p);
    buf[n] = '\0';
}

/* Re-point every edge touching `dup` so it touches `canonical` instead,
 * skipping self-loops and edges that already exist. The dup's own edges are
 * removed afterward by the caller's delete_entity cascade. */
static void repoint_edges(mnemon_storage_t *s, const uint8_t dup[16],
                          const uint8_t canonical[16])
{
    mnemon_edge_list_t outb = {0}, inb = {0};
    mnemon_get_edges_from(s, dup, NULL, &outb);
    mnemon_get_edges_to(s, dup, NULL, &inb);

    MDB_txn *wtxn;
    if (mnemon_graph_txn_begin(s->graph, 0, &wtxn) == MNEMON_OK) {
        for (uint32_t i = 0; i < outb.count; i++) {
            mnemon_edge_t e = outb.edges[i];
            if (memcmp(e.target_id, canonical, 16) == 0) continue; /* self */
            if (!e.edge_type) continue;
            if (mnemon_graph_edge_exists(s->graph, wtxn, canonical,
                                         e.edge_type, e.target_id)) continue;
            mnemon_edge_t ne = e;
            mnemon_uuid_t u; mnemon_uuid_generate(&u);
            memcpy(ne.id, u.bytes, 16);
            memcpy(ne.source_id, canonical, 16);
            mnemon_graph_put_edge(s->graph, wtxn, &ne);
        }
        for (uint32_t i = 0; i < inb.count; i++) {
            mnemon_edge_t e = inb.edges[i];
            if (memcmp(e.source_id, canonical, 16) == 0) continue; /* self */
            if (!e.edge_type) continue;
            if (mnemon_graph_edge_exists(s->graph, wtxn, e.source_id,
                                         e.edge_type, canonical)) continue;
            mnemon_edge_t ne = e;
            mnemon_uuid_t u; mnemon_uuid_generate(&u);
            memcpy(ne.id, u.bytes, 16);
            memcpy(ne.target_id, canonical, 16);
            mnemon_graph_put_edge(s->graph, wtxn, &ne);
        }
        mnemon_graph_txn_commit(wtxn);
    }
    mnemon_edge_list_free(&outb);
    mnemon_edge_list_free(&inb);
}

/* Merge entities that share a normalized name and entity_type into one
 * canonical entity (the one with the most observations). Duplicate
 * observations are appended (deduped), edges are re-pointed to the canonical,
 * and duplicates are deleted. *merged receives the number removed. */
mnemon_err_t mnemon_resolve_entities(mnemon_storage_t *s, size_t *merged)
{
    if (!s) return MNEMON_ERR_INVALID_INPUT;
    if (merged) *merged = 0;

    typedef struct { uint8_t id[16]; char *norm; char *type;
                     uint32_t obs; } row_t;
    row_t *rows = NULL;
    size_t n = 0, cap = 0;

    MDB_txn *txn;
    if (mnemon_graph_txn_begin(s->graph, MDB_RDONLY, &txn) != MNEMON_OK)
        return MNEMON_ERR_LMDB;
    MDB_dbi dbi;
    if (mdb_dbi_open(txn, "entities", 0, &dbi) == 0) {
        MDB_cursor *cur;
        if (mdb_cursor_open(txn, dbi, &cur) == 0) {
            MDB_val k, v;
            int rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST);
            while (rc == 0) {
                mnemon_entity_t e = {0};
                if (mnemon_graph_get_entity(s->graph, txn, k.mv_data, &e)
                        == MNEMON_OK && e.name && e.name[0]) {
                    if (n >= cap) {
                        cap = cap ? cap * 2 : 64;
                        row_t *p = realloc(rows, cap * sizeof(row_t));
                        if (!p) { mnemon_entity_free(&e); break; }
                        rows = p;
                    }
                    char nb[256];
                    normalize_name(e.name, nb, sizeof(nb));
                    memcpy(rows[n].id, e.id, 16);
                    rows[n].norm = strdup(nb);
                    rows[n].type = strdup(e.entity_type ? e.entity_type : "");
                    rows[n].obs  = e.observation_count;
                    n++;
                }
                mnemon_entity_free(&e);
                rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
            }
            mdb_cursor_close(cur);
        }
    }
    mnemon_graph_txn_abort(txn);

    size_t removed = 0;
    char consumed_pad = 0; (void)consumed_pad;
    bool *consumed = calloc(n ? n : 1, sizeof(bool));
    if (!consumed) { for (size_t i=0;i<n;i++){free(rows[i].norm);free(rows[i].type);} free(rows); return MNEMON_ERR_OOM; }

    for (size_t i = 0; i < n; i++) {
        if (consumed[i]) continue;
        /* Canonical for this group = member with most observations. */
        size_t canon = i;
        for (size_t j = i + 1; j < n; j++) {
            if (consumed[j]) continue;
            if (strcmp(rows[i].norm, rows[j].norm) == 0 &&
                strcmp(rows[i].type, rows[j].type) == 0 &&
                rows[j].obs > rows[canon].obs)
                canon = j;
        }
        for (size_t j = i; j < n; j++) {
            if (j == canon || consumed[j]) continue;
            if (strcmp(rows[i].norm, rows[j].norm) != 0 ||
                strcmp(rows[i].type, rows[j].type) != 0)
                continue;

            /* Merge rows[j] (dup) into rows[canon]. */
            mnemon_entity_t c = {0}, d = {0};
            if (mnemon_get_entity(s, rows[canon].id, &c) == MNEMON_OK &&
                mnemon_get_entity(s, rows[j].id, &d) == MNEMON_OK) {
                /* Append dup observations not already present. */
                for (uint32_t oi = 0; oi < d.observation_count; oi++) {
                    bool dupobs = false;
                    for (uint32_t ci = 0; ci < c.observation_count; ci++)
                        if (c.observations[ci] && d.observations[oi] &&
                            strcmp(c.observations[ci], d.observations[oi]) == 0)
                            { dupobs = true; break; }
                    if (dupobs) continue;
                    char **no = realloc(c.observations,
                        (c.observation_count + 1) * sizeof(char *));
                    if (!no) break;
                    c.observations = no;
                    c.observations[c.observation_count++] =
                        strdup(d.observations[oi]);
                }
                c.updated_at = mnemon_time_ms();

                repoint_edges(s, rows[j].id, rows[canon].id);
                mnemon_delete_entity(s, rows[j].id);   /* cascades dup edges */
                mnemon_store_entity(s, &c);            /* new version + index */
                removed++;
                consumed[j] = true;
            }
            mnemon_entity_free(&c);
            mnemon_entity_free(&d);
        }
        consumed[i] = true;
    }

    for (size_t i = 0; i < n; i++) { free(rows[i].norm); free(rows[i].type); }
    free(rows);
    free(consumed);
    if (merged) *merged = removed;
    mnemon_log(MNEMON_LOG_INFO, "resolve_entities: merged %zu duplicates", removed);
    return MNEMON_OK;
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

    /* Cascade: remove edges touching this entity so no dangling edges remain. */
    mnemon_graph_del_edges_for_entity(s->graph, txn, id, NULL);

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
    if (rebuild_vec) {
        mnemon_vector_clear(s->vector, false);  /* memory + chunk vectors */
        mnemon_vector_clear(s->vector, true);   /* entity vectors */
        /* Chunk metadata is regenerated from scratch below, so drop the old
         * rows; otherwise they would point at vectors we just cleared. */
        MDB_txn *ctxn;
        if (mnemon_graph_txn_begin(s->graph, 0, &ctxn) == MNEMON_OK) {
            mnemon_graph_clear_chunks(s->graph, ctxn);
            mnemon_graph_txn_commit(ctxn);
        }
    }

    /* IDs of memories whose content is long enough to be chunked.  Chunk
     * re-indexing happens after the scan txn closes, because store_chunks()
     * opens its own write transactions and LMDB forbids a write txn while a
     * read txn is held on the same thread. */
    uint8_t (*chunk_parents)[16] = NULL;
    size_t chunk_parent_count = 0, chunk_parent_cap = 0;

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

                /* Note long memories for chunk re-indexing after the scan. */
                if (rebuild_vec && mem.content &&
                    strlen(mem.content) >= CHUNK_MIN_CONTENT) {
                    if (chunk_parent_count >= chunk_parent_cap) {
                        size_t nc = chunk_parent_cap ? chunk_parent_cap * 2 : 64;
                        uint8_t (*p)[16] = realloc(chunk_parents, nc * 16);
                        if (p) { chunk_parents = p; chunk_parent_cap = nc; }
                    }
                    if (chunk_parent_count < chunk_parent_cap)
                        memcpy(chunk_parents[chunk_parent_count++], mem.id, 16);
                }

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
            int ent_count = 0, ent_backfilled = 0;
            rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
            while (rc == 0) {
                mnemon_entity_t ent = {0};
                mnemon_graph_get_entity(s->graph, txn, key.mv_data, &ent);

                if (rebuild_fts)
                    mnemon_fts_index_entity(s->fts, &ent);

                if (rebuild_vec && ent.embedding) {
                    mnemon_vector_add(s->vector, ent.id, ent.embedding,
                                      s->dimensions, true);
                } else if (rebuild_vec && s->embed &&
                           mnemon_embed_available(s->embed) &&
                           ent.name && ent.name[0]) {
                    /* Backfill: entities created before embed-on-create have no
                     * stored embedding, leaving entity vector search empty.
                     * Embed the name (as create_entity does) and index it. */
                    int dims = mnemon_embed_dimensions(s->embed);
                    float *emb = malloc((size_t)dims * sizeof(float));
                    if (emb) {
                        if (mnemon_embed_text(s->embed, ent.name,
                                strlen(ent.name), emb, dims, false) == MNEMON_OK) {
                            mnemon_vector_add(s->vector, ent.id, emb, dims, true);
                            ent_backfilled++;
                        }
                        free(emb);
                    }
                }

                ent_count++;
                mnemon_entity_free(&ent);
                rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
            }
            mdb_cursor_close(cur);
            mnemon_log(MNEMON_LOG_INFO,
                       "rebuild: re-indexed %d entities (%d embeddings backfilled)",
                       ent_count, ent_backfilled);
        }
    }

    mnemon_graph_txn_abort(txn);

    /* Re-generate chunk vectors + metadata for long memories.  Done outside
     * the scan txn because store_chunks() opens its own write transactions. */
    if (rebuild_vec && chunk_parents) {
        for (size_t i = 0; i < chunk_parent_count; i++) {
            MDB_txn *mtxn;
            if (mnemon_graph_txn_begin(s->graph, MDB_RDONLY, &mtxn) != MNEMON_OK)
                continue;
            mnemon_memory_t mem = {0};
            mnemon_err_t ge = mnemon_graph_get_memory(s->graph, mtxn,
                                                      chunk_parents[i], &mem);
            mnemon_graph_txn_abort(mtxn);
            if (ge == MNEMON_OK) {
                store_chunks(s, &mem);
                mnemon_memory_free(&mem);
            }
        }
        mnemon_log(MNEMON_LOG_INFO,
                   "rebuild: re-chunked %zu long memories", chunk_parent_count);
    }
    free(chunk_parents);

    /* Prune dangling edges whose endpoints no longer exist -- cleans up edges
     * left behind by deletes that predate the entity-delete cascade. */
    {
        MDB_txn *etxn;
        if (mnemon_graph_txn_begin(s->graph, 0, &etxn) == MNEMON_OK) {
            size_t pruned = 0;
            mnemon_graph_prune_orphan_edges(s->graph, etxn, &pruned);
            mnemon_graph_txn_commit(etxn);
            if (pruned)
                mnemon_log(MNEMON_LOG_INFO,
                           "rebuild: pruned %zu orphan edges", pruned);
        }
    }

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

mnemon_honeypot_t *mnemon_storage_honeypot(mnemon_storage_t *s)
{
    return s ? s->honeypot : NULL;
}

void mnemon_storage_set_honeypot(mnemon_storage_t *s, mnemon_honeypot_t *hp)
{
    if (s) s->honeypot = hp;
}

mnemon_reader_pool_t *mnemon_storage_reader_pool(mnemon_storage_t *s)
{
    return s ? s->reader_pool : NULL;
}

void mnemon_storage_set_reader_pool(mnemon_storage_t *s,
                                    mnemon_reader_pool_t *pool)
{
    if (s) s->reader_pool = pool;
}
