/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * graph.c -- LMDB knowledge graph with hand-rolled MessagePack serialization
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>

#include "graph.h"
#include "log.h"

#define SCHEMA_VERSION "1"
#define EMBED_DIM 768

/* LMDB's MDB_val.mv_data is void* but LMDB never modifies key/value
 * data passed for reads. This macro avoids -Wcast-qual warnings. */
#define CONST_CAST(p) ((void *)(uintptr_t)(const void *)(p))

struct mnemon_graph {
    MDB_env *env;
    MDB_dbi  dbi_entities;
    MDB_dbi  dbi_edges;
    MDB_dbi  dbi_edges_rev;
    MDB_dbi  dbi_memories;
    MDB_dbi  dbi_chunks;
    MDB_dbi  dbi_temporal;
    MDB_dbi  dbi_intents;
    MDB_dbi  dbi_meta;
};

/* ================================================================== */
/* Minimal MessagePack writer                                          */
/* ================================================================== */

typedef struct { uint8_t *buf; size_t len, cap; } mpk_t;

static void mpk_init(mpk_t *m) { m->buf = NULL; m->len = m->cap = 0; }
static void mpk_free(mpk_t *m) { free(m->buf); m->buf = NULL; m->len = m->cap = 0; }

static int mpk_ensure(mpk_t *m, size_t need)
{
    if (m->len + need <= m->cap) return 0;
    size_t nc = m->cap ? m->cap * 2 : 256;
    while (nc < m->len + need) nc *= 2;
    uint8_t *p = realloc(m->buf, nc);
    if (!p) return -1;
    m->buf = p; m->cap = nc;
    return 0;
}

static void mpk_u8(mpk_t *m, uint8_t v) { if (mpk_ensure(m,1)==0) m->buf[m->len++]=v; }
static void mpk_u16be(mpk_t *m, uint16_t v) { mpk_u8(m,v>>8); mpk_u8(m,v&0xff); }
static void mpk_u32be(mpk_t *m, uint32_t v) { mpk_u16be(m,v>>16); mpk_u16be(m,v&0xffff); }
static void mpk_u64be(mpk_t *m, uint64_t v) { mpk_u32be(m,(uint32_t)(v>>32)); mpk_u32be(m,(uint32_t)v); }

static void mpk_raw(mpk_t *m, const void *d, size_t n)
{
    if (mpk_ensure(m,n)==0) { memcpy(m->buf+m->len, d, n); m->len+=n; }
}

static void mpk_map(mpk_t *m, uint32_t n)
{
    if (n < 16) mpk_u8(m, 0x80 | (uint8_t)n);
    else { mpk_u8(m, 0xde); mpk_u16be(m, (uint16_t)n); }
}

static void mpk_array(mpk_t *m, uint32_t n)
{
    if (n < 16) mpk_u8(m, 0x90 | (uint8_t)n);
    else { mpk_u8(m, 0xdc); mpk_u16be(m, (uint16_t)n); }
}

static void mpk_str(mpk_t *m, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (!s) { mpk_u8(m, 0xc0); return; }
    if (n < 32) mpk_u8(m, 0xa0 | (uint8_t)n);
    else if (n < 256) { mpk_u8(m, 0xd9); mpk_u8(m, (uint8_t)n); }
    else { mpk_u8(m, 0xda); mpk_u16be(m, (uint16_t)n); }
    mpk_raw(m, s, n);
}

static void mpk_bin(mpk_t *m, const void *d, size_t n)
{
    if (n < 256) { mpk_u8(m, 0xc4); mpk_u8(m, (uint8_t)n); }
    else { mpk_u8(m, 0xc5); mpk_u16be(m, (uint16_t)n); }
    mpk_raw(m, d, n);
}

static void mpk_int64(mpk_t *m, int64_t v)
{
    mpk_u8(m, 0xd3); mpk_u64be(m, (uint64_t)v);
}

static void mpk_uint32(mpk_t *m, uint32_t v)
{
    mpk_u8(m, 0xce); mpk_u32be(m, v);
}

static void mpk_float(mpk_t *m, float v)
{
    uint32_t bits; memcpy(&bits, &v, 4);
    mpk_u8(m, 0xca); mpk_u32be(m, bits);
}

static void mpk_bool(mpk_t *m, bool v) { mpk_u8(m, v ? 0xc3 : 0xc2); }

/* ================================================================== */
/* Minimal MessagePack reader                                          */
/* ================================================================== */

typedef struct { const uint8_t *buf; size_t len, pos; } mpr_t;

static uint8_t mpr_u8(mpr_t *r) { return r->pos < r->len ? r->buf[r->pos++] : 0; }
static uint16_t mpr_u16be(mpr_t *r) { uint16_t v = (uint16_t)mpr_u8(r)<<8; v|=mpr_u8(r); return v; }
static uint32_t mpr_u32be(mpr_t *r) { uint32_t v = (uint32_t)mpr_u16be(r)<<16; v|=mpr_u16be(r); return v; }
static uint64_t mpr_u64be(mpr_t *r) { uint64_t v = (uint64_t)mpr_u32be(r)<<32; v|=mpr_u32be(r); return v; }

#define MPR_MAX_COLLECTION 10000  /* defense-in-depth cap on map/array count */

static uint32_t mpr_map(mpr_t *r)
{
    uint8_t b = mpr_u8(r);
    uint32_t n = 0;
    if ((b & 0xf0) == 0x80) n = b & 0x0f;
    else if (b == 0xde) n = mpr_u16be(r);
    return n > MPR_MAX_COLLECTION ? 0 : n;
}

static uint32_t mpr_array(mpr_t *r)
{
    uint8_t b = mpr_u8(r);
    uint32_t n = 0;
    if ((b & 0xf0) == 0x90) n = b & 0x0f;
    else if (b == 0xdc) n = mpr_u16be(r);
    return n > MPR_MAX_COLLECTION ? 0 : n;
}

static char *mpr_str(mpr_t *r)
{
    uint8_t b = mpr_u8(r);
    size_t n = 0;
    if (b == 0xc0) return NULL; /* nil */
    if ((b & 0xe0) == 0xa0) n = b & 0x1f;
    else if (b == 0xd9) n = mpr_u8(r);
    else if (b == 0xda) n = mpr_u16be(r);
    else return NULL;
    if (r->pos + n > r->len) return NULL;
    char *s = malloc(n + 1);
    if (s) { memcpy(s, r->buf + r->pos, n); s[n] = '\0'; }
    r->pos += n;
    return s;
}

static size_t mpr_bin(mpr_t *r, const uint8_t **out)
{
    uint8_t b = mpr_u8(r);
    size_t n = 0;
    if (b == 0xc4) n = mpr_u8(r);
    else if (b == 0xc5) n = mpr_u16be(r);
    else return 0;
    if (r->pos + n > r->len) { *out = NULL; return 0; }
    *out = r->buf + r->pos;
    r->pos += n;
    return n;
}

static int64_t mpr_int64(mpr_t *r)
{
    uint8_t b = mpr_u8(r);
    if (b == 0xd3) return (int64_t)mpr_u64be(r);
    if (b == 0xd2) return (int32_t)mpr_u32be(r);
    if (b <= 0x7f) return b; /* positive fixint */
    return 0;
}

static uint32_t mpr_uint32(mpr_t *r)
{
    uint8_t b = mpr_u8(r);
    if (b == 0xce) return mpr_u32be(r);
    if (b <= 0x7f) return b;
    return 0;
}

static float mpr_float(mpr_t *r)
{
    uint8_t b = mpr_u8(r);
    if (b == 0xca) { uint32_t bits = mpr_u32be(r); float v; memcpy(&v,&bits,4); return v; }
    return 0.0f;
}

static bool mpr_bool(mpr_t *r)
{
    uint8_t b = mpr_u8(r);
    return b == 0xc3;
}

/* Skip one msgpack value */
static void mpr_skip(mpr_t *r)
{
    if (r->pos >= r->len) return;
    uint8_t b = r->buf[r->pos];

    /* fixint, nil, bool */
    if (b <= 0x7f || b == 0xc0 || b == 0xc2 || b == 0xc3 ||
        (b >= 0xe0)) { r->pos++; return; }

    r->pos++;
    if (b == 0xcc) { r->pos += 1; return; }
    if (b == 0xcd || b == 0xd1) { r->pos += 2; return; }
    if (b == 0xce || b == 0xd2 || b == 0xca) { r->pos += 4; return; }
    if (b == 0xcf || b == 0xd3 || b == 0xcb) { r->pos += 8; return; }

    /* str */
    if ((b & 0xe0) == 0xa0) { r->pos += (b & 0x1f); return; }
    if (b == 0xd9) { size_t n = r->buf[r->pos++]; r->pos += n; return; }
    if (b == 0xda) { size_t n = (r->buf[r->pos]<<8)|r->buf[r->pos+1]; r->pos += 2 + n; return; }

    /* bin */
    if (b == 0xc4) { size_t n = r->buf[r->pos++]; r->pos += n; return; }
    if (b == 0xc5) { size_t n = (r->buf[r->pos]<<8)|r->buf[r->pos+1]; r->pos += 2 + n; return; }

    /* array */
    uint32_t cnt = 0;
    if ((b & 0xf0) == 0x90) cnt = b & 0x0f;
    else if (b == 0xdc) { cnt = (r->buf[r->pos]<<8)|r->buf[r->pos+1]; r->pos += 2; }
    if (cnt > 0) { for (uint32_t i = 0; i < cnt; i++) mpr_skip(r); return; }

    /* map */
    if ((b & 0xf0) == 0x80) cnt = b & 0x0f;
    else if (b == 0xde) { cnt = (r->buf[r->pos]<<8)|r->buf[r->pos+1]; r->pos += 2; }
    if (cnt > 0) { for (uint32_t i = 0; i < cnt * 2; i++) mpr_skip(r); return; }
}

/* ================================================================== */
/* FNV-1a hash for edge type keys                                      */
/* ================================================================== */

static uint64_t fnv1a_64(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (; s && *s; s++) { h ^= (uint8_t)*s; h *= 0x100000001b3ULL; }
    return h;
}

/* ================================================================== */
/* Serialization                                                       */
/* ================================================================== */

static void pack_entity(mpk_t *m, const mnemon_entity_t *e)
{
    uint32_t nfields = 12;
    mpk_map(m, nfields);
    mpk_str(m, "v");    mpk_uint32(m, 2);  /* v2: added event_date */
    mpk_str(m, "id");   mpk_bin(m, e->id, 16);
    mpk_str(m, "name"); mpk_str(m, e->name);
    mpk_str(m, "type"); mpk_str(m, e->entity_type);
    mpk_str(m, "obs");
    mpk_array(m, e->observation_count);
    for (uint32_t i = 0; i < e->observation_count; i++)
        mpk_str(m, e->observations[i]);
    mpk_str(m, "emb");
    if (e->embedding) {
        mpk_bin(m, e->embedding, EMBED_DIM * sizeof(float));
    } else {
        mpk_u8(m, 0xc0); /* nil */
    }
    mpk_str(m, "cat"); mpk_int64(m, e->created_at);
    mpk_str(m, "uat"); mpk_int64(m, e->updated_at);
    mpk_str(m, "edt"); mpk_int64(m, e->event_date);
    mpk_str(m, "imp"); mpk_float(m, e->importance);
    mpk_str(m, "acc"); mpk_uint32(m, e->access_count);
    mpk_str(m, "lac"); mpk_int64(m, e->last_accessed);
}

static void unpack_entity(mpr_t *r, mnemon_entity_t *e)
{
    uint32_t n = mpr_map(r);
    memset(e, 0, sizeof(*e));
    for (uint32_t i = 0; i < n; i++) {
        char *key = mpr_str(r);
        if (!key) { mpr_skip(r); continue; }
        if (strcmp(key, "v") == 0) { mpr_skip(r); }
        else if (strcmp(key, "id") == 0) { const uint8_t *d; size_t l = mpr_bin(r, &d); if (l==16) memcpy(e->id,d,16); }
        else if (strcmp(key, "name") == 0) { e->name = mpr_str(r); }
        else if (strcmp(key, "type") == 0) { e->entity_type = mpr_str(r); }
        else if (strcmp(key, "obs") == 0) {
            uint32_t cnt = mpr_array(r);
            e->observations = calloc(cnt, sizeof(char*));
            e->observation_count = cnt;
            for (uint32_t j = 0; j < cnt; j++)
                e->observations[j] = mpr_str(r);
        }
        else if (strcmp(key, "emb") == 0) {
            /* Handle both bin (embedding data) and nil (no embedding) */
            if (r->pos < r->len && r->buf[r->pos] == 0xc0) {
                r->pos++; /* consume nil byte */
            } else {
                const uint8_t *d; size_t l = mpr_bin(r, &d);
                if (d && l == EMBED_DIM * sizeof(float)) {
                    e->embedding = malloc(l);
                    if (e->embedding) memcpy(e->embedding, d, l);
                }
            }
        }
        else if (strcmp(key, "cat") == 0) { e->created_at = mpr_int64(r); }
        else if (strcmp(key, "uat") == 0) { e->updated_at = mpr_int64(r); }
        else if (strcmp(key, "edt") == 0) { e->event_date = mpr_int64(r); }
        else if (strcmp(key, "imp") == 0) { e->importance = mpr_float(r); }
        else if (strcmp(key, "acc") == 0) { e->access_count = mpr_uint32(r); }
        else if (strcmp(key, "lac") == 0) { e->last_accessed = mpr_int64(r); }
        else { mpr_skip(r); }
        free(key);
    }
}

static void pack_edge(mpk_t *m, const mnemon_edge_t *e)
{
    mpk_map(m, 11);
    mpk_str(m,"v");    mpk_uint32(m,1);
    mpk_str(m,"id");   mpk_bin(m, e->id, 16);
    mpk_str(m,"src");  mpk_bin(m, e->source_id, 16);
    mpk_str(m,"tgt");  mpk_bin(m, e->target_id, 16);
    mpk_str(m,"type"); mpk_str(m, e->edge_type);
    mpk_str(m,"desc"); mpk_str(m, e->description);
    mpk_str(m,"wt");   mpk_float(m, e->weight);
    mpk_str(m,"vf");   mpk_int64(m, e->valid_from);
    mpk_str(m,"vt");   mpk_int64(m, e->valid_to);
    mpk_str(m,"cat");  mpk_int64(m, e->created_at);
    mpk_str(m,"exp");  mpk_int64(m, e->expired_at);
}

static void unpack_edge(mpr_t *r, mnemon_edge_t *e)
{
    uint32_t n = mpr_map(r);
    memset(e, 0, sizeof(*e));
    for (uint32_t i = 0; i < n; i++) {
        char *key = mpr_str(r);
        if (!key) { mpr_skip(r); continue; }
        if (strcmp(key,"v")==0) mpr_skip(r);
        else if (strcmp(key,"id")==0) { const uint8_t *d; size_t l=mpr_bin(r,&d); if(l==16) memcpy(e->id,d,16); }
        else if (strcmp(key,"src")==0) { const uint8_t *d; size_t l=mpr_bin(r,&d); if(l==16) memcpy(e->source_id,d,16); }
        else if (strcmp(key,"tgt")==0) { const uint8_t *d; size_t l=mpr_bin(r,&d); if(l==16) memcpy(e->target_id,d,16); }
        else if (strcmp(key,"type")==0) e->edge_type = mpr_str(r);
        else if (strcmp(key,"desc")==0) e->description = mpr_str(r);
        else if (strcmp(key,"wt")==0) e->weight = mpr_float(r);
        else if (strcmp(key,"vf")==0) e->valid_from = mpr_int64(r);
        else if (strcmp(key,"vt")==0) e->valid_to = mpr_int64(r);
        else if (strcmp(key,"cat")==0) e->created_at = mpr_int64(r);
        else if (strcmp(key,"exp")==0) e->expired_at = mpr_int64(r);
        else mpr_skip(r);
        free(key);
    }
}

static void pack_memory(mpk_t *m, const mnemon_memory_t *mem)
{
    mpk_map(m, 16);
    mpk_str(m,"v");    mpk_uint32(m,1);
    mpk_str(m,"id");   mpk_bin(m, mem->id, 16);
    mpk_str(m,"tier"); mpk_uint32(m, (uint32_t)mem->tier);
    mpk_str(m,"cnt");  mpk_str(m, mem->content);
    mpk_str(m,"st");   mpk_str(m, mem->source_type);
    mpk_str(m,"sid");  mpk_str(m, mem->source_id);
    mpk_str(m,"sa");   mpk_str(m, mem->source_author);
    mpk_str(m,"sts");  mpk_int64(m, mem->source_timestamp);
    mpk_str(m,"tags");
    mpk_array(m, mem->tag_count);
    for (uint32_t i = 0; i < mem->tag_count; i++)
        mpk_str(m, mem->tags[i]);
    mpk_str(m,"emb");
    if (mem->embedding)
        mpk_bin(m, mem->embedding, EMBED_DIM * sizeof(float));
    else
        mpk_u8(m, 0xc0);
    mpk_str(m,"imp"); mpk_float(m, mem->importance);
    mpk_str(m,"acc"); mpk_uint32(m, mem->access_count);
    mpk_str(m,"cat"); mpk_int64(m, mem->created_at);
    mpk_str(m,"lac"); mpk_int64(m, mem->last_accessed);
    mpk_str(m,"eids");
    mpk_array(m, mem->entity_id_count);
    for (uint32_t i = 0; i < mem->entity_id_count; i++)
        mpk_bin(m, mem->entity_ids[i], 16);
    mpk_str(m,"con"); mpk_bool(m, mem->consolidated);
}

static void unpack_memory(mpr_t *r, mnemon_memory_t *mem)
{
    uint32_t n = mpr_map(r);
    memset(mem, 0, sizeof(*mem));
    for (uint32_t i = 0; i < n; i++) {
        char *key = mpr_str(r);
        if (!key) { mpr_skip(r); continue; }
        if (strcmp(key,"v")==0) mpr_skip(r);
        else if (strcmp(key,"id")==0) { const uint8_t *d; size_t l=mpr_bin(r,&d); if(l==16) memcpy(mem->id,d,16); }
        else if (strcmp(key,"tier")==0) mem->tier = (mnemon_memory_tier_t)mpr_uint32(r);
        else if (strcmp(key,"cnt")==0) mem->content = mpr_str(r);
        else if (strcmp(key,"st")==0) mem->source_type = mpr_str(r);
        else if (strcmp(key,"sid")==0) mem->source_id = mpr_str(r);
        else if (strcmp(key,"sa")==0) mem->source_author = mpr_str(r);
        else if (strcmp(key,"sts")==0) mem->source_timestamp = mpr_int64(r);
        else if (strcmp(key,"tags")==0) {
            uint32_t cnt = mpr_array(r);
            mem->tags = calloc(cnt, sizeof(char*));
            mem->tag_count = cnt;
            for (uint32_t j=0; j<cnt; j++) mem->tags[j] = mpr_str(r);
        }
        else if (strcmp(key,"emb")==0) {
            if (r->pos < r->len && r->buf[r->pos] == 0xc0) {
                r->pos++;
            } else {
                const uint8_t *d; size_t l=mpr_bin(r,&d);
                if (d && l==EMBED_DIM*sizeof(float)) {
                    mem->embedding = malloc(l);
                    if (mem->embedding) memcpy(mem->embedding, d, l);
                }
            }
        }
        else if (strcmp(key,"imp")==0) mem->importance = mpr_float(r);
        else if (strcmp(key,"acc")==0) mem->access_count = mpr_uint32(r);
        else if (strcmp(key,"cat")==0) mem->created_at = mpr_int64(r);
        else if (strcmp(key,"lac")==0) mem->last_accessed = mpr_int64(r);
        else if (strcmp(key,"eids")==0) {
            uint32_t cnt = mpr_array(r);
            mem->entity_ids = calloc(cnt, sizeof(uint8_t*));
            mem->entity_id_count = cnt;
            for (uint32_t j=0; j<cnt; j++) {
                const uint8_t *d; size_t l=mpr_bin(r,&d);
                if (d && l==16) { mem->entity_ids[j]=malloc(16); if(mem->entity_ids[j]) memcpy(mem->entity_ids[j],d,16); }
            }
        }
        else if (strcmp(key,"con")==0) mem->consolidated = mpr_bool(r);
        else mpr_skip(r);
        free(key);
    }
}

/* ================================================================== */
/* LMDB operations                                                     */
/* ================================================================== */

mnemon_err_t mnemon_graph_open(mnemon_graph_t **out, const char *path,
                               int map_size_gb, int max_readers)
{
    mnemon_graph_t *g;
    MDB_txn *txn;
    int rc;

    if (!out || !path) return MNEMON_ERR_INVALID_INPUT;

    g = calloc(1, sizeof(*g));
    if (!g) return MNEMON_ERR_OOM;

    /* Ensure directory exists */
    mkdir(path, 0700);

    rc = mdb_env_create(&g->env);
    if (rc) goto fail_lmdb;

    mdb_env_set_maxdbs(g->env, 8);
    mdb_env_set_mapsize(g->env, (size_t)map_size_gb * 1073741824ULL);
    mdb_env_set_maxreaders(g->env, max_readers);

    rc = mdb_env_open(g->env, path, 0, 0600);
    if (rc) goto fail_env;

    /* Open named databases in a write txn */
    rc = mdb_txn_begin(g->env, NULL, 0, &txn);
    if (rc) goto fail_env;

    mdb_dbi_open(txn, "entities", MDB_CREATE, &g->dbi_entities);
    mdb_dbi_open(txn, "edges", MDB_CREATE | MDB_DUPSORT, &g->dbi_edges);
    mdb_dbi_open(txn, "edges_rev", MDB_CREATE | MDB_DUPSORT, &g->dbi_edges_rev);
    mdb_dbi_open(txn, "memories", MDB_CREATE, &g->dbi_memories);
    mdb_dbi_open(txn, "chunks", MDB_CREATE, &g->dbi_chunks);
    mdb_dbi_open(txn, "temporal", MDB_CREATE | MDB_DUPSORT, &g->dbi_temporal);
    mdb_dbi_open(txn, "intents", MDB_CREATE, &g->dbi_intents);
    mdb_dbi_open(txn, "meta", MDB_CREATE, &g->dbi_meta);

    /* Check/set schema version */
    MDB_val mk = {strlen("schema_version"), CONST_CAST("schema_version")};
    MDB_val mv;
    rc = mdb_get(txn, g->dbi_meta, &mk, &mv);
    if (rc == MDB_NOTFOUND) {
        mv.mv_data = CONST_CAST(SCHEMA_VERSION);
        mv.mv_size = strlen(SCHEMA_VERSION);
        mdb_put(txn, g->dbi_meta, &mk, &mv, 0);
    }

    rc = mdb_txn_commit(txn);
    if (rc) goto fail_env;

    *out = g;
    return MNEMON_OK;

fail_lmdb:
    mnemon_err_set(MNEMON_ERR_LMDB, rc, "LMDB error: %s", mdb_strerror(rc));
    free(g);
    return MNEMON_ERR_LMDB;
fail_env:
    mnemon_err_set(MNEMON_ERR_LMDB, rc, "LMDB error: %s", mdb_strerror(rc));
    mdb_env_close(g->env);
    free(g);
    return MNEMON_ERR_LMDB;
}

void mnemon_graph_close(mnemon_graph_t *g)
{
    if (!g) return;
    mdb_env_close(g->env);
    free(g);
}

MDB_env *mnemon_graph_env(mnemon_graph_t *g) { return g ? g->env : NULL; }

/* ------------------------------------------------------------------ */
/* Chunk metadata                                                      */
/* ------------------------------------------------------------------ */

/* Chunk record format: 16 bytes parent_id + 4 bytes seq + 4 bytes offset + 4 bytes length = 28 bytes */

mnemon_err_t mnemon_graph_put_chunk(mnemon_graph_t *g, MDB_txn *txn,
                                    const mnemon_chunk_meta_t *c)
{
    if (!g || !txn || !c) return MNEMON_ERR_INVALID_INPUT;

    uint8_t val[28];
    memcpy(val, c->parent_id, 16);
    uint32_t seq = c->sequence;
    uint32_t off = c->byte_offset;
    uint32_t len = c->byte_length;
    memcpy(val + 16, &seq, 4);
    memcpy(val + 20, &off, 4);
    memcpy(val + 24, &len, 4);

    MDB_val k = {16, CONST_CAST(c->id)};
    MDB_val v = {28, val};
    int rc = mdb_put(txn, g->dbi_chunks, &k, &v, 0);
    if (rc) {
        mnemon_err_set(MNEMON_ERR_LMDB, rc, "%s", mdb_strerror(rc));
        return MNEMON_ERR_LMDB;
    }
    return MNEMON_OK;
}

mnemon_err_t mnemon_graph_get_chunk(mnemon_graph_t *g, MDB_txn *txn,
                                    const uint8_t chunk_id[16],
                                    mnemon_chunk_meta_t *out)
{
    if (!g || !txn || !chunk_id || !out) return MNEMON_ERR_INVALID_INPUT;

    MDB_val k = {16, CONST_CAST(chunk_id)};
    MDB_val v;
    int rc = mdb_get(txn, g->dbi_chunks, &k, &v);
    if (rc == MDB_NOTFOUND) return MNEMON_ERR_NOT_FOUND;
    if (rc) {
        mnemon_err_set(MNEMON_ERR_LMDB, rc, "%s", mdb_strerror(rc));
        return MNEMON_ERR_LMDB;
    }
    if (v.mv_size < 28) return MNEMON_ERR_NOT_FOUND;

    memcpy(out->id, chunk_id, 16);
    memcpy(out->parent_id, v.mv_data, 16);
    memcpy(&out->sequence, (uint8_t *)v.mv_data + 16, 4);
    memcpy(&out->byte_offset, (uint8_t *)v.mv_data + 20, 4);
    memcpy(&out->byte_length, (uint8_t *)v.mv_data + 24, 4);
    return MNEMON_OK;
}

mnemon_err_t mnemon_graph_txn_begin(mnemon_graph_t *g, unsigned int flags, MDB_txn **txn)
{
    int rc = mdb_txn_begin(g->env, NULL, flags, txn);
    if (rc) { mnemon_err_set(MNEMON_ERR_LMDB, rc, "%s", mdb_strerror(rc)); return MNEMON_ERR_LMDB; }
    return MNEMON_OK;
}

mnemon_err_t mnemon_graph_txn_commit(MDB_txn *txn)
{
    int rc = mdb_txn_commit(txn);
    if (rc) { mnemon_err_set(MNEMON_ERR_LMDB, rc, "%s", mdb_strerror(rc)); return MNEMON_ERR_LMDB; }
    return MNEMON_OK;
}

void mnemon_graph_txn_abort(MDB_txn *txn) { mdb_txn_abort(txn); }

/* Entity CRUD */
mnemon_err_t mnemon_graph_put_entity(mnemon_graph_t *g, MDB_txn *txn, const mnemon_entity_t *e)
{
    mpk_t m; mpk_init(&m);
    pack_entity(&m, e);
    MDB_val key = {16, CONST_CAST(e->id)};
    MDB_val val = {m.len, m.buf};
    int rc = mdb_put(txn, g->dbi_entities, &key, &val, 0);
    mpk_free(&m);
    if (rc) { mnemon_err_set(MNEMON_ERR_LMDB, rc, "%s", mdb_strerror(rc)); return MNEMON_ERR_LMDB; }
    return MNEMON_OK;
}

mnemon_err_t mnemon_graph_get_entity(mnemon_graph_t *g, MDB_txn *txn, const uint8_t id[16], mnemon_entity_t *out)
{
    MDB_val key = {16, CONST_CAST(id)};
    MDB_val val;
    int rc = mdb_get(txn, g->dbi_entities, &key, &val);
    if (rc == MDB_NOTFOUND) return MNEMON_ERR_NOT_FOUND;
    if (rc) { mnemon_err_set(MNEMON_ERR_LMDB, rc, "%s", mdb_strerror(rc)); return MNEMON_ERR_LMDB; }
    mpr_t r = {val.mv_data, val.mv_size, 0};
    unpack_entity(&r, out);
    return MNEMON_OK;
}

mnemon_err_t mnemon_graph_del_entity(mnemon_graph_t *g, MDB_txn *txn, const uint8_t id[16])
{
    MDB_val key = {16, CONST_CAST(id)};
    int rc = mdb_del(txn, g->dbi_entities, &key, NULL);
    if (rc == MDB_NOTFOUND) return MNEMON_ERR_NOT_FOUND;
    if (rc) { mnemon_err_set(MNEMON_ERR_LMDB, rc, "%s", mdb_strerror(rc)); return MNEMON_ERR_LMDB; }
    return MNEMON_OK;
}

/* Edge CRUD */
static void build_edge_key(uint8_t out[40], const uint8_t a[16], const char *type, const uint8_t b[16])
{
    uint64_t h = fnv1a_64(type);
    memcpy(out, a, 16);
    out[16]=(uint8_t)(h>>56); out[17]=(uint8_t)(h>>48);
    out[18]=(uint8_t)(h>>40); out[19]=(uint8_t)(h>>32);
    out[20]=(uint8_t)(h>>24); out[21]=(uint8_t)(h>>16);
    out[22]=(uint8_t)(h>>8);  out[23]=(uint8_t)h;
    memcpy(out+24, b, 16);
}

mnemon_err_t mnemon_graph_put_edge(mnemon_graph_t *g, MDB_txn *txn, const mnemon_edge_t *e)
{
    mpk_t m; mpk_init(&m);
    pack_edge(&m, e);

    /* Forward index: source | type_hash | target */
    uint8_t fwd[40]; build_edge_key(fwd, e->source_id, e->edge_type, e->target_id);
    MDB_val fk = {40, fwd}, fv = {m.len, m.buf};
    int rc = mdb_put(txn, g->dbi_edges, &fk, &fv, 0);

    /* Reverse index: target | type_hash | source -> edge_id */
    uint8_t rev[40]; build_edge_key(rev, e->target_id, e->edge_type, e->source_id);
    MDB_val rk = {40, rev}, rv = {16, CONST_CAST(e->id)};
    if (!rc) rc = mdb_put(txn, g->dbi_edges_rev, &rk, &rv, 0);

    mpk_free(&m);
    if (rc) { mnemon_err_set(MNEMON_ERR_LMDB, rc, "%s", mdb_strerror(rc)); return MNEMON_ERR_LMDB; }
    return MNEMON_OK;
}

mnemon_err_t mnemon_graph_get_edges_from(mnemon_graph_t *g, MDB_txn *txn,
                                         const uint8_t source_id[16],
                                         const char *edge_type,
                                         mnemon_edge_list_t *out)
{
    MDB_cursor *cur;
    int rc;

    memset(out, 0, sizeof(*out));
    rc = mdb_cursor_open(txn, g->dbi_edges, &cur);
    if (rc) return MNEMON_ERR_LMDB;

    /* Position at source_id prefix */
    MDB_val key = {16, CONST_CAST(source_id)};
    MDB_val val;

    rc = mdb_cursor_get(cur, &key, &val, MDB_SET_RANGE);

    size_t cap = 16;
    out->edges = calloc(cap, sizeof(mnemon_edge_t));
    if (!out->edges) { mdb_cursor_close(cur); return MNEMON_ERR_OOM; }

    while (rc == 0) {
        if (key.mv_size != 40 || memcmp(key.mv_data, source_id, 16) != 0)
            break;

        mpr_t r = {val.mv_data, val.mv_size, 0};
        mnemon_edge_t e;
        unpack_edge(&r, &e);

        /* Filter by edge type if specified */
        if (edge_type == NULL || (e.edge_type && strcmp(e.edge_type, edge_type) == 0)) {
            if (out->count >= cap) {
                cap *= 2;
                mnemon_edge_t *p = realloc(out->edges, cap * sizeof(mnemon_edge_t));
                if (!p) { mnemon_edge_free(&e); break; }
                out->edges = p;
            }
            out->edges[out->count++] = e;
        } else {
            mnemon_edge_free(&e);
        }

        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
    }

    mdb_cursor_close(cur);
    return MNEMON_OK;
}

mnemon_err_t mnemon_graph_get_edges_to(mnemon_graph_t *g, MDB_txn *txn,
                                       const uint8_t target_id[16],
                                       const char *edge_type,
                                       mnemon_edge_list_t *out)
{
    /* Similar to get_edges_from but using reverse index */
    (void)edge_type;
    memset(out, 0, sizeof(*out));

    MDB_cursor *cur;
    int rc = mdb_cursor_open(txn, g->dbi_edges_rev, &cur);
    if (rc) return MNEMON_ERR_LMDB;

    MDB_val key = {16, CONST_CAST(target_id)};
    MDB_val val;
    rc = mdb_cursor_get(cur, &key, &val, MDB_SET_RANGE);

    size_t cap = 16;
    out->edges = calloc(cap, sizeof(mnemon_edge_t));

    while (rc == 0) {
        if (key.mv_size != 40 || memcmp(key.mv_data, target_id, 16) != 0)
            break;

        /* val contains the edge_id; look up the full edge from forward index */
        /* For Phase 1, return a partial edge with source/target IDs from the key */
        if (out->count >= cap) {
            cap *= 2;
            mnemon_edge_t *p = realloc(out->edges, cap * sizeof(mnemon_edge_t));
            if (!p) break;
            out->edges = p;
        }
        mnemon_edge_t *e = &out->edges[out->count];
        memset(e, 0, sizeof(*e));
        if (val.mv_size == 16) memcpy(e->id, val.mv_data, 16);
        memcpy(e->target_id, target_id, 16);
        /* source_id is at key bytes 24-39 */
        if (key.mv_size == 40) memcpy(e->source_id, (uint8_t*)key.mv_data + 24, 16);
        out->count++;

        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
    }

    mdb_cursor_close(cur);
    return MNEMON_OK;
}

/* Memory CRUD */
mnemon_err_t mnemon_graph_put_memory(mnemon_graph_t *g, MDB_txn *txn, const mnemon_memory_t *mem)
{
    mpk_t m; mpk_init(&m);
    pack_memory(&m, mem);
    MDB_val key = {16, CONST_CAST(mem->id)};
    MDB_val val = {m.len, m.buf};
    int rc = mdb_put(txn, g->dbi_memories, &key, &val, 0);
    mpk_free(&m);
    if (rc) { mnemon_err_set(MNEMON_ERR_LMDB, rc, "%s", mdb_strerror(rc)); return MNEMON_ERR_LMDB; }
    return MNEMON_OK;
}

mnemon_err_t mnemon_graph_get_memory(mnemon_graph_t *g, MDB_txn *txn, const uint8_t id[16], mnemon_memory_t *out)
{
    MDB_val key = {16, CONST_CAST(id)};
    MDB_val val;
    int rc = mdb_get(txn, g->dbi_memories, &key, &val);
    if (rc == MDB_NOTFOUND) return MNEMON_ERR_NOT_FOUND;
    if (rc) { mnemon_err_set(MNEMON_ERR_LMDB, rc, "%s", mdb_strerror(rc)); return MNEMON_ERR_LMDB; }
    mpr_t r = {val.mv_data, val.mv_size, 0};
    unpack_memory(&r, out);
    return MNEMON_OK;
}

mnemon_err_t mnemon_graph_del_memory(mnemon_graph_t *g, MDB_txn *txn, const uint8_t id[16])
{
    MDB_val key = {16, CONST_CAST(id)};
    int rc = mdb_del(txn, g->dbi_memories, &key, NULL);
    if (rc == MDB_NOTFOUND) return MNEMON_ERR_NOT_FOUND;
    if (rc) { mnemon_err_set(MNEMON_ERR_LMDB, rc, "%s", mdb_strerror(rc)); return MNEMON_ERR_LMDB; }
    return MNEMON_OK;
}

/* BFS visited-set: open-addressing hash table for O(1) lookup */
#define BFS_MAX 1000
#define BFS_HASH_SIZE 2048  /* power of 2, > 2 * BFS_MAX */

static uint32_t bfs_hash_id(const uint8_t id[16])
{
    /* FNV-1a over 16 bytes */
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 16; i++) { h ^= id[i]; h *= 0x01000193u; }
    return h;
}

typedef struct {
    uint8_t ids[BFS_HASH_SIZE][16];
    bool    occupied[BFS_HASH_SIZE];
} bfs_visited_t;

static bool bfs_visited_check(const bfs_visited_t *v, const uint8_t id[16])
{
    uint32_t idx = bfs_hash_id(id) & (BFS_HASH_SIZE - 1);
    for (int probe = 0; probe < 32; probe++) {
        uint32_t slot = (idx + (uint32_t)probe) & (BFS_HASH_SIZE - 1);
        if (!v->occupied[slot]) return false;
        if (memcmp(v->ids[slot], id, 16) == 0) return true;
    }
    return false;  /* probe limit reached, assume not seen */
}

static void bfs_visited_insert(bfs_visited_t *v, const uint8_t id[16])
{
    uint32_t idx = bfs_hash_id(id) & (BFS_HASH_SIZE - 1);
    for (int probe = 0; probe < 32; probe++) {
        uint32_t slot = (idx + (uint32_t)probe) & (BFS_HASH_SIZE - 1);
        if (!v->occupied[slot]) {
            memcpy(v->ids[slot], id, 16);
            v->occupied[slot] = true;
            return;
        }
    }
}

/* BFS */
mnemon_err_t mnemon_graph_bfs(mnemon_graph_t *g, MDB_txn *txn,
                              const uint8_t start_id[16], int max_depth,
                              mnemon_visit_fn visit, void *user_ctx)
{
    /* Heap-allocate BFS state */
    uint8_t (*queue)[16] = malloc(BFS_MAX * 16);
    int *depths = malloc(BFS_MAX * sizeof(int));
    bfs_visited_t *visited = calloc(1, sizeof(bfs_visited_t));
    if (!queue || !depths || !visited) {
        free(queue); free(depths); free(visited);
        return MNEMON_ERR_OOM;
    }
    int head = 0, tail = 0;
    int node_count = 0;

    memcpy(queue[tail], start_id, 16);
    depths[tail] = 0;
    tail++;
    bfs_visited_insert(visited, start_id);
    node_count++;

    while (head < tail && tail < BFS_MAX) {
        uint8_t current[16];
        memcpy(current, queue[head], 16);
        int depth = depths[head];
        head++;

        if (depth > max_depth) continue;

        mnemon_entity_t ent;
        memset(&ent, 0, sizeof(ent));
        mnemon_graph_get_entity(g, txn, current, &ent);

        if (visit) {
            int rc = visit(&ent, NULL, depth, user_ctx);
            mnemon_entity_free(&ent);
            if (rc != 0) break;
        } else {
            mnemon_entity_free(&ent);
        }

        if (depth >= max_depth) continue;

        mnemon_edge_list_t edges;
        memset(&edges, 0, sizeof(edges));
        mnemon_graph_get_edges_from(g, txn, current, NULL, &edges);

        for (uint32_t i = 0; i < edges.count && tail < BFS_MAX; i++) {
            if (!bfs_visited_check(visited, edges.edges[i].target_id) &&
                node_count < BFS_MAX) {
                memcpy(queue[tail], edges.edges[i].target_id, 16);
                depths[tail] = depth + 1;
                tail++;
                bfs_visited_insert(visited, edges.edges[i].target_id);
                node_count++;
            }
        }
        mnemon_edge_list_free(&edges);
    }

    free(queue);
    free(depths);
    free(visited);
    return MNEMON_OK;
    #undef BFS_MAX
}

/* Intent log */
mnemon_err_t mnemon_graph_put_intent(mnemon_graph_t *g, MDB_txn *txn,
                                     const uint8_t id[16], uint8_t op_type,
                                     uint8_t steps_done,
                                     const uint8_t *payload, size_t payload_len)
{
    mpk_t m; mpk_init(&m);
    mpk_map(&m, 4);
    mpk_str(&m,"op"); mpk_uint32(&m, op_type);
    mpk_str(&m,"sd"); mpk_uint32(&m, steps_done);
    mpk_str(&m,"pl"); if (payload) mpk_bin(&m, payload, payload_len); else mpk_u8(&m, 0xc0);
    mpk_str(&m,"v");  mpk_uint32(&m, 1);

    MDB_val key = {16, CONST_CAST(id)};
    MDB_val val = {m.len, m.buf};
    int rc = mdb_put(txn, g->dbi_intents, &key, &val, 0);
    mpk_free(&m);
    if (rc) return MNEMON_ERR_LMDB;
    return MNEMON_OK;
}

mnemon_err_t mnemon_graph_update_intent(mnemon_graph_t *g, MDB_txn *txn,
                                        const uint8_t id[16], uint8_t steps_done)
{
    return mnemon_graph_put_intent(g, txn, id, 0, steps_done, NULL, 0);
}

mnemon_err_t mnemon_graph_del_intent(mnemon_graph_t *g, MDB_txn *txn, const uint8_t id[16])
{
    MDB_val key = {16, CONST_CAST(id)};
    int rc = mdb_del(txn, g->dbi_intents, &key, NULL);
    if (rc && rc != MDB_NOTFOUND) return MNEMON_ERR_LMDB;
    return MNEMON_OK;
}

/* Meta */
mnemon_err_t mnemon_graph_get_meta(mnemon_graph_t *g, MDB_txn *txn, const char *key, char *buf, size_t buf_len)
{
    MDB_val mk = {strlen(key), CONST_CAST(key)};
    MDB_val mv;
    int rc = mdb_get(txn, g->dbi_meta, &mk, &mv);
    if (rc == MDB_NOTFOUND) return MNEMON_ERR_NOT_FOUND;
    if (rc) return MNEMON_ERR_LMDB;
    size_t n = mv.mv_size < buf_len - 1 ? mv.mv_size : buf_len - 1;
    memcpy(buf, mv.mv_data, n);
    buf[n] = '\0';
    return MNEMON_OK;
}

mnemon_err_t mnemon_graph_put_meta(mnemon_graph_t *g, MDB_txn *txn, const char *key, const char *value)
{
    MDB_val mk = {strlen(key), CONST_CAST(key)};
    MDB_val mv = {strlen(value), CONST_CAST(value)};
    int rc = mdb_put(txn, g->dbi_meta, &mk, &mv, 0);
    if (rc) return MNEMON_ERR_LMDB;
    return MNEMON_OK;
}

/* Stats */
mnemon_err_t mnemon_graph_count(mnemon_graph_t *g, MDB_txn *txn,
                                size_t *entities, size_t *edges, size_t *memories)
{
    MDB_stat stat;
    if (entities) { mdb_stat(txn, g->dbi_entities, &stat); *entities = stat.ms_entries; }
    if (edges) { mdb_stat(txn, g->dbi_edges, &stat); *edges = stat.ms_entries; }
    if (memories) { mdb_stat(txn, g->dbi_memories, &stat); *memories = stat.ms_entries; }
    return MNEMON_OK;
}
