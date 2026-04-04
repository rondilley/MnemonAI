/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * vector.c -- usearch HNSW vector index wrapper
 *
 * Manages two indexes: one for memory embeddings, one for entity embeddings.
 * Uses pthread_rwlock_t for concurrent read/write access.
 * UUID-to-uint64 mapping via lower 8 bytes of UUIDv7.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include <usearch.h>

#include "vector.h"
#include "log.h"

/* Key-UUID mapping for reverse lookups from search results */
typedef struct {
    uint64_t key;
    uint8_t  uuid[16];
} keymap_entry_t;

typedef struct {
    keymap_entry_t *entries;
    size_t          count;
    size_t          capacity;
    bool            sorted;
} keymap_t;

struct mnemon_vector {
    usearch_index_t  mem_idx;
    usearch_index_t  ent_idx;
    keymap_t         mem_map;
    keymap_t         ent_map;
    char            *dir;
    int              dimensions;
    pthread_rwlock_t rwlock;
};

static uint64_t uuid_to_key(const uint8_t id[16])
{
    uint64_t k = 0;
    for (int i = 8; i < 16; i++)
        k = (k << 8) | id[i];
    return k;
}

static void keymap_init(keymap_t *m)
{
    memset(m, 0, sizeof(*m));
}

static void keymap_free(keymap_t *m)
{
    free(m->entries);
    memset(m, 0, sizeof(*m));
}

static int keymap_add(keymap_t *m, uint64_t key, const uint8_t uuid[16])
{
    if (m->count >= m->capacity) {
        size_t nc = m->capacity ? m->capacity * 2 : 256;
        keymap_entry_t *p = realloc(m->entries, nc * sizeof(*p));
        if (!p) return -1;
        m->entries = p;
        m->capacity = nc;
    }
    m->entries[m->count].key = key;
    memcpy(m->entries[m->count].uuid, uuid, 16);
    m->count++;
    m->sorted = false;
    return 0;
}

static void keymap_remove(keymap_t *m, uint64_t key)
{
    for (size_t i = 0; i < m->count; i++) {
        if (m->entries[i].key == key) {
            m->entries[i] = m->entries[m->count - 1];
            m->count--;
            return;
        }
    }
}

static int keymap_entry_cmp(const void *a, const void *b)
{
    const keymap_entry_t *ea = (const keymap_entry_t *)a;
    const keymap_entry_t *eb = (const keymap_entry_t *)b;
    if (ea->key < eb->key) return -1;
    if (ea->key > eb->key) return 1;
    return 0;
}

static const uint8_t *keymap_find(keymap_t *m, uint64_t key)
{
    if (m->count > 32) {
        /* Lazy sort for O(log n) binary search */
        if (!m->sorted) {
            qsort(m->entries, m->count, sizeof(keymap_entry_t),
                  keymap_entry_cmp);
            m->sorted = true;
        }
        keymap_entry_t needle;
        needle.key = key;
        keymap_entry_t *found = bsearch(&needle, m->entries, m->count,
                                         sizeof(keymap_entry_t),
                                         keymap_entry_cmp);
        if (found) return found->uuid;
        return NULL;
    }
    /* Small maps: linear scan is faster due to cache locality */
    for (size_t i = 0; i < m->count; i++) {
        if (m->entries[i].key == key)
            return m->entries[i].uuid;
    }
    return NULL;
}

static void keymap_save(const keymap_t *m, const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    uint32_t n = (uint32_t)m->count;
    fwrite(&n, sizeof(n), 1, fp);
    fwrite(m->entries, sizeof(keymap_entry_t), m->count, fp);
    fclose(fp);
}

static void keymap_load(keymap_t *m, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    uint32_t n;
    if (fread(&n, sizeof(n), 1, fp) != 1) { fclose(fp); return; }
    m->entries = calloc(n, sizeof(keymap_entry_t));
    if (!m->entries) { fclose(fp); return; }
    m->count = fread(m->entries, sizeof(keymap_entry_t), n, fp);
    m->capacity = n;
    fclose(fp);
}

static usearch_index_t create_index(int dimensions, usearch_error_t *err)
{
    usearch_init_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.metric_kind = usearch_metric_cos_k;
    opts.quantization = usearch_scalar_f32_k;
    opts.dimensions = (size_t)dimensions;
    opts.connectivity = 32;
    opts.expansion_add = 128;
    opts.expansion_search = 64;
    opts.multi = false;
    return usearch_init(&opts, err);
}

mnemon_err_t mnemon_vector_open(mnemon_vector_t **out, const char *dir,
                                int dimensions)
{
    mnemon_vector_t *v;
    usearch_error_t err = NULL;
    char path[4096];

    if (!out || !dir) return MNEMON_ERR_INVALID_INPUT;

    v = calloc(1, sizeof(*v));
    if (!v) return MNEMON_ERR_OOM;

    v->dir = strdup(dir);
    v->dimensions = dimensions;
    pthread_rwlock_init(&v->rwlock, NULL);
    keymap_init(&v->mem_map);
    keymap_init(&v->ent_map);

    mkdir(dir, 0700);

    /* Create indexes */
    v->mem_idx = create_index(dimensions, &err);
    if (err) {
        mnemon_err_set(MNEMON_ERR_USEARCH, 0, "usearch mem init: %s", err);
        free(v->dir); free(v);
        return MNEMON_ERR_USEARCH;
    }

    err = NULL;
    v->ent_idx = create_index(dimensions, &err);
    if (err) {
        mnemon_err_set(MNEMON_ERR_USEARCH, 0, "usearch ent init: %s", err);
        usearch_free(v->mem_idx, &err);
        free(v->dir); free(v);
        return MNEMON_ERR_USEARCH;
    }

    /* Try loading existing indexes */
    snprintf(path, sizeof(path), "%s/vectors_memories.usearch", dir);
    err = NULL;
    usearch_load(v->mem_idx, path, &err);
    if (err) {
        mnemon_log(MNEMON_LOG_DEBUG, "no existing memory index: %s", err);
        err = NULL;
    }

    snprintf(path, sizeof(path), "%s/vectors_entities.usearch", dir);
    usearch_load(v->ent_idx, path, &err);
    if (err) {
        mnemon_log(MNEMON_LOG_DEBUG, "no existing entity index: %s", err);
        err = NULL;
    }

    /* Load key maps */
    snprintf(path, sizeof(path), "%s/mem_keymap.bin", dir);
    keymap_load(&v->mem_map, path);
    snprintf(path, sizeof(path), "%s/ent_keymap.bin", dir);
    keymap_load(&v->ent_map, path);

    *out = v;
    return MNEMON_OK;
}

void mnemon_vector_close(mnemon_vector_t *v)
{
    usearch_error_t err = NULL;
    if (!v) return;
    if (v->mem_idx) usearch_free(v->mem_idx, &err);
    if (v->ent_idx) usearch_free(v->ent_idx, &err);
    keymap_free(&v->mem_map);
    keymap_free(&v->ent_map);
    pthread_rwlock_destroy(&v->rwlock);
    free(v->dir);
    free(v);
}

mnemon_err_t mnemon_vector_add(mnemon_vector_t *v, const uint8_t id[16],
                               const float *embedding, int dimensions,
                               bool is_entity)
{
    usearch_error_t err = NULL;
    uint64_t key;
    usearch_index_t idx;
    keymap_t *map;

    if (!v || !id || !embedding) return MNEMON_ERR_INVALID_INPUT;
    if (dimensions != v->dimensions) return MNEMON_ERR_INVALID_INPUT;

    key = uuid_to_key(id);
    idx = is_entity ? v->ent_idx : v->mem_idx;
    map = is_entity ? &v->ent_map : &v->mem_map;

    pthread_rwlock_wrlock(&v->rwlock);

    /* Reserve capacity if needed */
    size_t current = usearch_size(idx, &err);
    size_t cap = usearch_capacity(idx, &err);
    if (current + 1 >= cap) {
        usearch_reserve(idx, cap + 1024, &err);
        if (err) {
            pthread_rwlock_unlock(&v->rwlock);
            mnemon_err_set(MNEMON_ERR_USEARCH, 0, "reserve: %s", err);
            return MNEMON_ERR_USEARCH;
        }
    }

    err = NULL;
    usearch_add(idx, key, embedding, usearch_scalar_f32_k, &err);
    if (err) {
        pthread_rwlock_unlock(&v->rwlock);
        mnemon_err_set(MNEMON_ERR_USEARCH, 0, "add: %s", err);
        return MNEMON_ERR_USEARCH;
    }

    keymap_add(map, key, id);
    pthread_rwlock_unlock(&v->rwlock);
    return MNEMON_OK;
}

mnemon_err_t mnemon_vector_remove(mnemon_vector_t *v, const uint8_t id[16],
                                  bool is_entity)
{
    usearch_error_t err = NULL;
    uint64_t key;
    usearch_index_t idx;
    keymap_t *map;

    if (!v || !id) return MNEMON_ERR_INVALID_INPUT;

    key = uuid_to_key(id);
    idx = is_entity ? v->ent_idx : v->mem_idx;
    map = is_entity ? &v->ent_map : &v->mem_map;

    pthread_rwlock_wrlock(&v->rwlock);
    usearch_remove(idx, key, &err);
    keymap_remove(map, key);
    pthread_rwlock_unlock(&v->rwlock);

    return MNEMON_OK;
}

mnemon_err_t mnemon_vector_search(mnemon_vector_t *v, const float *query,
                                  int dimensions, int top_k,
                                  bool search_entities,
                                  mnemon_vector_results_t *out)
{
    usearch_error_t err = NULL;
    usearch_index_t idx;
    const keymap_t *map;

    if (!v || !query || !out) return MNEMON_ERR_INVALID_INPUT;
    if (dimensions != v->dimensions) return MNEMON_ERR_INVALID_INPUT;

    memset(out, 0, sizeof(*out));
    idx = search_entities ? v->ent_idx : v->mem_idx;
    map = search_entities ? &v->ent_map : &v->mem_map;

    usearch_key_t *keys = calloc((size_t)top_k, sizeof(usearch_key_t));
    usearch_distance_t *dists = calloc((size_t)top_k, sizeof(usearch_distance_t));
    if (!keys || !dists) {
        free(keys); free(dists);
        return MNEMON_ERR_OOM;
    }

    pthread_rwlock_rdlock(&v->rwlock);
    size_t found = usearch_search(idx, query, usearch_scalar_f32_k,
                                  (size_t)top_k, keys, dists, &err);
    pthread_rwlock_unlock(&v->rwlock);

    if (err) {
        free(keys); free(dists);
        mnemon_err_set(MNEMON_ERR_USEARCH, 0, "search: %s", err);
        return MNEMON_ERR_USEARCH;
    }

    out->results = calloc(found, sizeof(mnemon_vector_result_t));
    if (!out->results) {
        free(keys); free(dists);
        return MNEMON_ERR_OOM;
    }

    int count = 0;
    for (size_t i = 0; i < found; i++) {
        const uint8_t *uuid = keymap_find(map, keys[i]);
        if (uuid) {
            memcpy(out->results[count].id, uuid, 16);
            out->results[count].distance = dists[i];
            count++;
        }
    }
    out->count = count;

    free(keys);
    free(dists);
    return MNEMON_OK;
}

void mnemon_vector_results_free(mnemon_vector_results_t *r)
{
    if (!r) return;
    free(r->results);
    memset(r, 0, sizeof(*r));
}

mnemon_err_t mnemon_vector_save(mnemon_vector_t *v)
{
    usearch_error_t err = NULL;
    char path[4096], tmp[4096];

    if (!v) return MNEMON_ERR_INVALID_INPUT;

    pthread_rwlock_rdlock(&v->rwlock);

    /* Atomic save: write to tmp, then rename */
    snprintf(tmp, sizeof(tmp), "%s/vectors_memories.usearch.tmp", v->dir);
    snprintf(path, sizeof(path), "%s/vectors_memories.usearch", v->dir);
    usearch_save(v->mem_idx, tmp, &err);
    if (!err) rename(tmp, path);
    else mnemon_log(MNEMON_LOG_ERROR, "save mem index: %s", err);

    err = NULL;
    snprintf(tmp, sizeof(tmp), "%s/vectors_entities.usearch.tmp", v->dir);
    snprintf(path, sizeof(path), "%s/vectors_entities.usearch", v->dir);
    usearch_save(v->ent_idx, tmp, &err);
    if (!err) rename(tmp, path);
    else mnemon_log(MNEMON_LOG_ERROR, "save ent index: %s", err);

    /* Save key maps */
    snprintf(path, sizeof(path), "%s/mem_keymap.bin", v->dir);
    keymap_save(&v->mem_map, path);
    snprintf(path, sizeof(path), "%s/ent_keymap.bin", v->dir);
    keymap_save(&v->ent_map, path);

    pthread_rwlock_unlock(&v->rwlock);
    return MNEMON_OK;
}

size_t mnemon_vector_count(mnemon_vector_t *v, bool entities)
{
    usearch_error_t err = NULL;
    if (!v) return 0;
    return usearch_size(entities ? v->ent_idx : v->mem_idx, &err);
}

void mnemon_vector_read_lock(mnemon_vector_t *v)
{
    if (v) pthread_rwlock_rdlock(&v->rwlock);
}

void mnemon_vector_read_unlock(mnemon_vector_t *v)
{
    if (v) pthread_rwlock_unlock(&v->rwlock);
}

void mnemon_vector_write_lock(mnemon_vector_t *v)
{
    if (v) pthread_rwlock_wrlock(&v->rwlock);
}

void mnemon_vector_write_unlock(mnemon_vector_t *v)
{
    if (v) pthread_rwlock_unlock(&v->rwlock);
}
