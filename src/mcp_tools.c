/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mcp_tools.c -- MCP tool handler implementations (Phase 1: 16 tools)
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>

#include "mcp_tools.h"
#include "storage.h"
#include "search.h"
#include "honeypot.h"
#include "temporal.h"
#include "embed.h"
#include "secret.h"
#include "id.h"
#include "memory.h"
#include "consolidate.h"
#include "import.h"
#include "fts.h"
#include "graph.h"
#include "hardware.h"
#include "log.h"

/* ---- Helpers ---- */

static void uuid_to_json_str(const uint8_t id[16], char *buf)
{
    mnemon_uuid_t u;
    memcpy(u.bytes, id, 16);
    mnemon_uuid_to_string(&u, buf, 37);
}

static int uuid_from_json(const cJSON *item, uint8_t out[16])
{
    if (!cJSON_IsString(item)) return -1;
    mnemon_uuid_t u;
    if (mnemon_uuid_from_string(item->valuestring, &u) != MNEMON_OK)
        return -1;
    memcpy(out, u.bytes, 16);
    return 0;
}

static const char *json_str(const cJSON *obj, const char *key, const char *def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item)) return item->valuestring;
    return def;
}

static int json_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) return item->valueint;
    return def;
}

static bool json_bool(const cJSON *obj, const char *key, bool def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    return def;
}

static cJSON *result_set_to_json(mnemon_result_set_t *rs)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < rs->count; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "id", rs->results[i].id);
        if (rs->results[i].content)
            cJSON_AddStringToObject(r, "content", rs->results[i].content);
        cJSON_AddNumberToObject(r, "score", rs->results[i].score);
        cJSON_AddNumberToObject(r, "graph_score", rs->results[i].graph_score);
        cJSON_AddNumberToObject(r, "vector_score", rs->results[i].vector_score);
        cJSON_AddNumberToObject(r, "keyword_score", rs->results[i].keyword_score);
        if (rs->results[i].tier)
            cJSON_AddStringToObject(r, "tier", rs->results[i].tier);
        cJSON_AddItemToArray(arr, r);
    }
    cJSON_AddItemToObject(obj, "results", arr);
    cJSON_AddBoolToObject(obj, "truncated", rs->truncated);
    return obj;
}

/* ---- Tool Handlers ---- */

static cJSON *tool_store_memory(mnemon_storage_t *s, const cJSON *params)
{
    const char *content = json_str(params, "content", NULL);
    if (!content) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "content is required");
        return e;
    }

    /* Content size limit (max_memory_size_kb, default 64KB) */
    if (strlen(content) > 64 * 1024) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error",
            "content exceeds maximum size (64KB)");
        return e;
    }

    /* Secret check */
    bool skip_secret = json_bool(params, "skip_secret_check", false);
    if (!skip_secret && mnemon_secret_detected(content, strlen(content))) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "secret detected in content");
        return e;
    }

    /* Injection scanning via honeypot module */
    mnemon_honeypot_t *hp = mnemon_storage_honeypot(s);
    if (hp) {
        float injection_score = mnemon_honeypot_scan_injection(
            hp, content, strlen(content));
        if (injection_score >= 7.0f) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "error",
                "content blocked: high prompt injection score");
            return e;
        }
    }

    mnemon_memory_t mem;
    memset(&mem, 0, sizeof(mem));

    mnemon_uuid_t uuid;
    mnemon_uuid_generate(&uuid);
    memcpy(mem.id, uuid.bytes, 16);

    mem.content = strdup(content);
    mem.source_type = strdup(json_str(params, "source_type", "mcp"));
    mem.source_id = strdup(json_str(params, "source_id", ""));
    const char *sa = json_str(params, "source_author", NULL);
    mem.source_author = sa ? strdup(sa) : NULL;
    mem.importance = 0.5f;

    /* Optional created_at override (ISO8601); defaults to now */
    const char *ca_str = json_str(params, "created_at", NULL);
    if (ca_str) {
        int64_t ca = mnemon_parse_iso8601(ca_str);
        mem.created_at = (ca > 0) ? ca : mnemon_time_ms();
    } else {
        mem.created_at = mnemon_time_ms();
    }
    mem.last_accessed = mem.created_at;

    /* Tier */
    const char *tier_str = json_str(params, "tier", "episodic");
    if (strcmp(tier_str, "semantic") == 0) mem.tier = MNEMON_TIER_SEMANTIC;
    else if (strcmp(tier_str, "procedural") == 0) mem.tier = MNEMON_TIER_PROCEDURAL;
    else mem.tier = MNEMON_TIER_EPISODIC;

    /* Tags */
    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(params, "tags");
    if (cJSON_IsArray(tags)) {
        mem.tag_count = (uint32_t)cJSON_GetArraySize(tags);
        if (mem.tag_count > 1000) mem.tag_count = 1000; /* cap tag array size */
        mem.tags = calloc(mem.tag_count, sizeof(char *));
        if (!mem.tags) { mnemon_memory_free(&mem); cJSON *e = cJSON_CreateObject(); cJSON_AddStringToObject(e, "error", "out of memory"); return e; }
        for (uint32_t i = 0; i < mem.tag_count; i++) {
            cJSON *t = cJSON_GetArrayItem(tags, (int)i);
            mem.tags[i] = cJSON_IsString(t) ? strdup(t->valuestring) : strdup("");
        }
    }

    /* Embedding */
    mnemon_embed_t *embed = mnemon_storage_embed(s);
    if (embed && mnemon_embed_available(embed)) {
        int dims = mnemon_embed_dimensions(embed);
        mem.embedding = malloc((size_t)dims * sizeof(float));
        if (mem.embedding) {
            mnemon_embed_text(embed, content, strlen(content),
                              mem.embedding, dims, false);
        }
    }

    mnemon_err_t err = mnemon_store_memory(s, &mem);

    cJSON *result = cJSON_CreateObject();
    if (err == MNEMON_OK) {
        char id_str[37];
        uuid_to_json_str(mem.id, id_str);
        cJSON_AddStringToObject(result, "id", id_str);
        cJSON_AddStringToObject(result, "tier", tier_str);
        cJSON_AddStringToObject(result, "source_type", mem.source_type);
        char ts[32];
        mnemon_format_iso8601(mem.created_at, ts, sizeof(ts));
        cJSON_AddStringToObject(result, "created_at", ts);
    } else {
        cJSON_AddStringToObject(result, "error", mnemon_err_msg());
    }

    mnemon_memory_free(&mem);
    return result;
}

static cJSON *tool_retrieve_memory(mnemon_storage_t *s, const cJSON *params)
{
    uint8_t id[16];
    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (uuid_from_json(id_item, id) != 0) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "invalid id");
        return e;
    }

    mnemon_memory_t mem;
    memset(&mem, 0, sizeof(mem));
    mnemon_err_t err = mnemon_get_memory(s, id, &mem);
    if (err != MNEMON_OK) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", mnemon_strerror(err));
        return e;
    }

    cJSON *result = cJSON_CreateObject();
    char id_str[37]; uuid_to_json_str(mem.id, id_str);
    cJSON_AddStringToObject(result, "id", id_str);
    cJSON_AddStringToObject(result, "content", mem.content ? mem.content : "");
    cJSON_AddStringToObject(result, "source_type", mem.source_type ? mem.source_type : "");
    cJSON_AddNumberToObject(result, "importance", mem.importance);
    cJSON_AddNumberToObject(result, "access_count", mem.access_count);
    char ts[32];
    mnemon_format_iso8601(mem.created_at, ts, sizeof(ts));
    cJSON_AddStringToObject(result, "created_at", ts);

    mnemon_memory_free(&mem);
    return result;
}

static cJSON *tool_update_memory(mnemon_storage_t *s, const cJSON *params)
{
    uint8_t id[16];
    if (uuid_from_json(cJSON_GetObjectItemCaseSensitive(params, "id"), id) != 0) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "invalid id");
        return e;
    }

    const char *content = json_str(params, "content", NULL);
    float *new_emb = NULL;
    int dims = 0;

    if (content) {
        mnemon_embed_t *embed = mnemon_storage_embed(s);
        if (embed && mnemon_embed_available(embed)) {
            dims = mnemon_embed_dimensions(embed);
            new_emb = malloc((size_t)dims * sizeof(float));
            if (new_emb)
                mnemon_embed_text(embed, content, strlen(content), new_emb, dims, false);
        }
    }

    mnemon_err_t err = mnemon_update_memory(s, id, content, new_emb);
    free(new_emb);

    cJSON *result = cJSON_CreateObject();
    if (err == MNEMON_OK) {
        char id_str[37]; uuid_to_json_str(id, id_str);
        cJSON_AddStringToObject(result, "id", id_str);
        cJSON_AddBoolToObject(result, "updated", true);
    } else {
        cJSON_AddStringToObject(result, "error", mnemon_err_msg());
    }
    return result;
}

static cJSON *tool_delete_memory(mnemon_storage_t *s, const cJSON *params)
{
    uint8_t id[16];
    if (uuid_from_json(cJSON_GetObjectItemCaseSensitive(params, "id"), id) != 0) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "invalid id");
        return e;
    }

    mnemon_err_t err = mnemon_delete_memory(s, id);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "deleted", err == MNEMON_OK);
    if (err != MNEMON_OK)
        cJSON_AddStringToObject(result, "error", mnemon_strerror(err));
    return result;
}

static cJSON *tool_delete_entity(mnemon_storage_t *s, const cJSON *params)
{
    uint8_t id[16];
    if (uuid_from_json(cJSON_GetObjectItemCaseSensitive(params, "id"), id) != 0) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "invalid id");
        return e;
    }

    mnemon_err_t err = mnemon_delete_entity(s, id);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "deleted", err == MNEMON_OK);
    if (err != MNEMON_OK)
        cJSON_AddStringToObject(result, "error", mnemon_strerror(err));
    return result;
}

static cJSON *tool_search_hybrid(mnemon_storage_t *s, const cJSON *params)
{
    mnemon_query_t q;
    memset(&q, 0, sizeof(q));
    q.query_text = json_str(params, "query", "");
    q.top_k = json_int(params, "top_k", 10);

    mnemon_result_set_t rs;
    memset(&rs, 0, sizeof(rs));
    mnemon_search_hybrid(s, &q, &rs);
    cJSON *result = result_set_to_json(&rs);
    mnemon_result_set_free(&rs);
    return result;
}

static cJSON *tool_search_semantic(mnemon_storage_t *s, const cJSON *params)
{
    mnemon_query_t q;
    memset(&q, 0, sizeof(q));
    q.query_text = json_str(params, "query", "");
    q.top_k = json_int(params, "top_k", 10);

    mnemon_result_set_t rs;
    memset(&rs, 0, sizeof(rs));
    mnemon_search_semantic(s, &q, &rs);
    cJSON *result = result_set_to_json(&rs);
    mnemon_result_set_free(&rs);
    return result;
}

static cJSON *tool_search_keyword(mnemon_storage_t *s, const cJSON *params)
{
    mnemon_query_t q;
    memset(&q, 0, sizeof(q));
    q.query_text = json_str(params, "query", "");
    q.top_k = json_int(params, "top_k", 10);

    mnemon_result_set_t rs;
    memset(&rs, 0, sizeof(rs));
    mnemon_search_keyword(s, &q, &rs);
    cJSON *result = result_set_to_json(&rs);
    mnemon_result_set_free(&rs);
    return result;
}

/* True when the string is a bare calendar date ("2026-04-01") with no time
 * component.  An upper bound expressed that way should include the whole day,
 * so callers extend it to 23:59:59.999 rather than stopping at midnight. */
static bool ts_is_date_only(const char *s)
{
    return s && !strchr(s, 'T') && !strchr(s, ' ') && !strchr(s, ':');
}

/* Milliseconds from the start of a day to its final instant. */
#define MS_PER_DAY 86400000LL

/* Current UTC year, used to default natural-language dates that omit a year
 * (e.g. "January 10").  Derived at runtime so it never goes stale, unlike the
 * hardcoded constant it replaced. */
static int current_year(void)
{
    char buf[32];
    mnemon_format_iso8601(mnemon_time_ms(), buf, sizeof(buf));
    return (int)strtol(buf, NULL, 10);  /* leading "YYYY" before the '-' */
}

static cJSON *tool_search_temporal(mnemon_storage_t *s, const cJSON *params)
{
    int64_t since = 0, until = 0;
    const char *since_str = json_str(params, "since", NULL);
    const char *until_str = json_str(params, "until", NULL);
    if (since_str) since = mnemon_parse_iso8601(since_str);
    if (until_str) until = mnemon_parse_iso8601(until_str);
    /* Date-only upper bound covers the entire day. */
    if (until > 0 && ts_is_date_only(until_str)) until += MS_PER_DAY - 1;
    int top_k = json_int(params, "top_k", 10);

    mnemon_result_set_t rs;
    memset(&rs, 0, sizeof(rs));
    mnemon_search_temporal(s, NULL, since, until, top_k, &rs);
    cJSON *result = result_set_to_json(&rs);
    mnemon_result_set_free(&rs);
    return result;
}

static cJSON *tool_create_entity(mnemon_storage_t *s, const cJSON *params)
{
    const char *name = json_str(params, "name", NULL);
    const char *etype = json_str(params, "entity_type", NULL);
    if (!name || !etype) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "name and entity_type required");
        return e;
    }

    mnemon_entity_t ent;
    memset(&ent, 0, sizeof(ent));
    mnemon_uuid_t uuid;
    mnemon_uuid_generate(&uuid);
    memcpy(ent.id, uuid.bytes, 16);
    ent.name = strdup(name);
    ent.entity_type = strdup(etype);
    ent.importance = 0.5f;
    ent.created_at = mnemon_time_ms();
    ent.updated_at = ent.created_at;

    /* Observations */
    const cJSON *obs = cJSON_GetObjectItemCaseSensitive(params, "observations");
    if (cJSON_IsArray(obs)) {
        ent.observation_count = (uint32_t)cJSON_GetArraySize(obs);
        ent.observations = calloc(ent.observation_count, sizeof(char *));
        for (uint32_t i = 0; i < ent.observation_count; i++) {
            cJSON *o = cJSON_GetArrayItem(obs, (int)i);
            ent.observations[i] = cJSON_IsString(o) ? strdup(o->valuestring) : strdup("");
        }
    }

    /* Embedding */
    mnemon_embed_t *embed = mnemon_storage_embed(s);
    if (embed && mnemon_embed_available(embed)) {
        int dims = mnemon_embed_dimensions(embed);
        ent.embedding = malloc((size_t)dims * sizeof(float));
        if (ent.embedding)
            mnemon_embed_text(embed, name, strlen(name), ent.embedding, dims, false);
    }

    mnemon_err_t err = mnemon_store_entity(s, &ent);

    cJSON *result = cJSON_CreateObject();
    if (err == MNEMON_OK) {
        char id_str[37]; uuid_to_json_str(ent.id, id_str);
        cJSON_AddStringToObject(result, "id", id_str);
        cJSON_AddStringToObject(result, "name", name);
        cJSON_AddStringToObject(result, "entity_type", etype);
    } else {
        cJSON_AddStringToObject(result, "error", mnemon_err_msg());
    }

    mnemon_entity_free(&ent);
    return result;
}

static cJSON *tool_add_observation(mnemon_storage_t *s, const cJSON *params)
{
    uint8_t id[16];
    if (uuid_from_json(cJSON_GetObjectItemCaseSensitive(params, "entity_id"), id) != 0) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "invalid entity_id");
        return e;
    }

    const char *obs = json_str(params, "observation", NULL);
    if (!obs) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "observation required");
        return e;
    }

    /* Get existing entity, add observation, re-store */
    mnemon_entity_t ent;
    memset(&ent, 0, sizeof(ent));
    mnemon_err_t err = mnemon_get_entity(s, id, &ent);
    if (err != MNEMON_OK) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", mnemon_strerror(err));
        return e;
    }

    /* Append observation */
    uint32_t new_count = ent.observation_count + 1;
    char **new_obs = realloc(ent.observations, new_count * sizeof(char *));
    if (!new_obs) {
        mnemon_entity_free(&ent);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "out of memory");
        return e;
    }
    new_obs[ent.observation_count] = strdup(obs);
    ent.observations = new_obs;
    ent.observation_count = new_count;
    ent.updated_at = mnemon_time_ms();

    err = mnemon_store_entity(s, &ent);

    cJSON *result = cJSON_CreateObject();
    char id_str[37]; uuid_to_json_str(id, id_str);
    cJSON_AddStringToObject(result, "id", id_str);
    cJSON_AddNumberToObject(result, "observation_count", ent.observation_count);

    mnemon_entity_free(&ent);
    return result;
}

static cJSON *tool_create_relation(mnemon_storage_t *s, const cJSON *params)
{
    mnemon_edge_t edge;
    memset(&edge, 0, sizeof(edge));

    if (uuid_from_json(cJSON_GetObjectItemCaseSensitive(params, "source_id"),
                       edge.source_id) != 0 ||
        uuid_from_json(cJSON_GetObjectItemCaseSensitive(params, "target_id"),
                       edge.target_id) != 0) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "invalid source_id or target_id");
        return e;
    }

    const char *etype = json_str(params, "edge_type", NULL);
    if (!etype) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "edge_type required");
        return e;
    }

    mnemon_uuid_t uuid;
    mnemon_uuid_generate(&uuid);
    memcpy(edge.id, uuid.bytes, 16);
    edge.edge_type = strdup(etype);
    const char *desc = json_str(params, "description", NULL);
    edge.description = desc ? strdup(desc) : NULL;
    edge.weight = 1.0f;
    edge.valid_from = mnemon_time_ms();
    edge.created_at = edge.valid_from;

    mnemon_err_t err = mnemon_store_edge(s, &edge);

    cJSON *result = cJSON_CreateObject();
    if (err == MNEMON_OK) {
        char id_str[37]; uuid_to_json_str(edge.id, id_str);
        cJSON_AddStringToObject(result, "id", id_str);
        cJSON_AddStringToObject(result, "edge_type", etype);
    } else {
        cJSON_AddStringToObject(result, "error", mnemon_err_msg());
    }

    mnemon_edge_free(&edge);
    return result;
}

static cJSON *tool_get_memory_stats(mnemon_storage_t *s, const cJSON *params)
{
    (void)params;
    mnemon_stats_t st;
    memset(&st, 0, sizeof(st));
    mnemon_get_stats(s, &st);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "total_memories", (double)st.total_memories);
    cJSON_AddNumberToObject(result, "total_entities", (double)st.total_entities);
    cJSON_AddNumberToObject(result, "total_edges", (double)st.total_edges);
    cJSON_AddNumberToObject(result, "memory_vectors", (double)st.memory_vectors);
    cJSON_AddNumberToObject(result, "entity_vectors", (double)st.entity_vectors);
    cJSON_AddNumberToObject(result, "fts_indexed", (double)st.fts_indexed);
    return result;
}

static cJSON *tool_health_check(mnemon_storage_t *s, const cJSON *params)
{
    (void)params;
    mnemon_embed_t *embed = mnemon_storage_embed(s);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddBoolToObject(result, "embedding_model_loaded",
                          embed && mnemon_embed_available(embed));
    char version_str[64];
    snprintf(version_str, sizeof(version_str), "v%s", PACKAGE_VERSION);
    cJSON_AddStringToObject(result, "version", version_str);
    cJSON_AddBoolToObject(result, "storage_ok", true);
    return result;
}

static cJSON *tool_consolidate(mnemon_storage_t *s, const cJSON *params)
{
    const char *topic = json_str(params, "topic", NULL);
    bool dry_run = json_bool(params, "dry_run", false);

    mnemon_consolidation_result_t cr;
    mnemon_consolidate(s, topic, NULL, dry_run, &cr);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "consolidated_count", cr.consolidated_count);
    cJSON_AddNumberToObject(result, "new_entities", cr.new_entities);
    cJSON_AddNumberToObject(result, "new_relations", cr.new_relations);
    cJSON_AddNumberToObject(result, "duration_ms", (double)cr.duration_ms);
    return result;
}

static cJSON *tool_list_memories(mnemon_storage_t *s, const cJSON *params)
{
    const char *source_filter = json_str(params, "source_type", NULL);
    const char *tier_filter = json_str(params, "tier", NULL);
    int offset = json_int(params, "offset", 0);
    int limit = json_int(params, "limit", 50);
    if (limit > 200) limit = 200;
    if (offset < 0) offset = 0;

    mnemon_graph_t *graph = mnemon_storage_graph(s);
    MDB_txn *txn;
    if (mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn) != MNEMON_OK) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "failed to open transaction");
        return e;
    }

    MDB_dbi dbi;
    int rc = mdb_dbi_open(txn, "memories", 0, &dbi);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        cJSON *result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "memories", cJSON_CreateArray());
        cJSON_AddNumberToObject(result, "total_count", 0);
        cJSON_AddBoolToObject(result, "truncated", false);
        return result;
    }

    MDB_cursor *cur;
    mdb_cursor_open(txn, dbi, &cur);

    cJSON *result = cJSON_CreateObject();
    cJSON *memories = cJSON_CreateArray();
    int total = 0, skipped = 0, added = 0;

    MDB_val key, val;
    rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
    while (rc == 0) {
        mnemon_memory_t mem = {0};
        mnemon_graph_get_memory(graph, txn, key.mv_data, &mem);

        /* Apply filters */
        bool matches = true;
        if (source_filter && mem.source_type &&
            strcmp(mem.source_type, source_filter) != 0) matches = false;
        if (tier_filter) {
            const char *tier_name = "episodic";
            if (mem.tier == MNEMON_TIER_SEMANTIC) tier_name = "semantic";
            else if (mem.tier == MNEMON_TIER_PROCEDURAL) tier_name = "procedural";
            if (strcmp(tier_name, tier_filter) != 0) matches = false;
        }

        if (matches) {
            total++;
            if (skipped < offset) {
                skipped++;
            } else if (added < limit) {
                cJSON *m = cJSON_CreateObject();
                char id_str[37]; uuid_to_json_str(mem.id, id_str);
                cJSON_AddStringToObject(m, "id", id_str);

                /* Content preview (200 chars) */
                if (mem.content) {
                    char preview[201];
                    strncpy(preview, mem.content, 200);
                    preview[200] = '\0';
                    cJSON_AddStringToObject(m, "content_preview", preview);
                }

                cJSON_AddStringToObject(m, "source_type",
                    mem.source_type ? mem.source_type : "");
                const char *tier_name = "episodic";
                if (mem.tier == MNEMON_TIER_SEMANTIC) tier_name = "semantic";
                else if (mem.tier == MNEMON_TIER_PROCEDURAL) tier_name = "procedural";
                cJSON_AddStringToObject(m, "tier", tier_name);
                cJSON_AddNumberToObject(m, "importance", mem.importance);

                char ts[32];
                mnemon_format_iso8601(mem.created_at, ts, sizeof(ts));
                cJSON_AddStringToObject(m, "created_at", ts);

                cJSON_AddItemToArray(memories, m);
                added++;
            }
        }

        mnemon_memory_free(&mem);
        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
    }

    mdb_cursor_close(cur);
    mnemon_graph_txn_abort(txn);

    cJSON_AddItemToObject(result, "memories", memories);
    cJSON_AddNumberToObject(result, "total_count", total);
    cJSON_AddBoolToObject(result, "truncated", (total > offset + limit));
    return result;
}

/* ---- Phase 1 completion: 6 additional tools ---- */

static cJSON *tool_search_entities(mnemon_storage_t *s, const cJSON *params)
{
    const char *query_text = json_str(params, "query", "");
    const char *etype_filter = json_str(params, "entity_type", NULL);
    int top_k = json_int(params, "top_k", 10);
    if (top_k > 50) top_k = 50;

    /* Search FTS5 for entities only (source_type=1) so memory documents can't
     * crowd entities out of the candidate window. */
    mnemon_fts_t *fts = mnemon_storage_fts(s);
    mnemon_fts_results_t fr = {0};
    mnemon_fts_search_typed(fts, query_text, 1, top_k * 2, &fr);

    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    int count = 0;

    for (int i = 0; i < fr.count && count < top_k; i++) {
        if (fr.results[i].source_type != 1) continue; /* entities only */

        mnemon_entity_t ent = {0};
        mnemon_err_t err = mnemon_get_entity(s, fr.results[i].id, &ent);
        if (err != MNEMON_OK) continue;

        if (etype_filter && ent.entity_type &&
            strcmp(ent.entity_type, etype_filter) != 0) {
            mnemon_entity_free(&ent);
            continue;
        }

        cJSON *obj = cJSON_CreateObject();
        char id_str[37]; uuid_to_json_str(ent.id, id_str);
        cJSON_AddStringToObject(obj, "id", id_str);
        cJSON_AddStringToObject(obj, "name", ent.name ? ent.name : "");
        cJSON_AddStringToObject(obj, "entity_type", ent.entity_type ? ent.entity_type : "");
        cJSON_AddNumberToObject(obj, "score", fr.results[i].score);
        cJSON_AddNumberToObject(obj, "observation_count", ent.observation_count);
        cJSON_AddItemToArray(arr, obj);
        count++;

        mnemon_entity_free(&ent);
    }

    cJSON_AddItemToObject(result, "entities", arr);
    cJSON_AddNumberToObject(result, "count", count);
    mnemon_fts_results_free(&fr);
    return result;
}

/* BFS visitor context for tool_get_entity_graph */
typedef struct {
    cJSON       *nodes;     /* array of entity objects */
    int          count;     /* entities collected */
    int          max_nodes; /* cap to prevent unbounded growth */
} graph_visit_ctx_t;

static int graph_visit_cb(const mnemon_entity_t *entity,
                          const mnemon_edge_t *edge,
                          int depth, void *user_ctx)
{
    (void)edge;
    graph_visit_ctx_t *ctx = (graph_visit_ctx_t *)user_ctx;
    if (ctx->count >= ctx->max_nodes) return 1; /* stop */

    cJSON *n = cJSON_CreateObject();
    char id_str[37]; uuid_to_json_str(entity->id, id_str);
    cJSON_AddStringToObject(n, "id", id_str);
    cJSON_AddStringToObject(n, "name", entity->name ? entity->name : "");
    cJSON_AddStringToObject(n, "entity_type",
                            entity->entity_type ? entity->entity_type : "");
    cJSON_AddNumberToObject(n, "observation_count", entity->observation_count);
    cJSON_AddNumberToObject(n, "depth", depth);
    cJSON_AddItemToArray(ctx->nodes, n);
    ctx->count++;
    return 0;
}

static cJSON *tool_get_entity_graph(mnemon_storage_t *s, const cJSON *params)
{
    uint8_t id[16];
    if (uuid_from_json(cJSON_GetObjectItemCaseSensitive(params, "entity_id"), id) != 0) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "invalid entity_id");
        return e;
    }

    int depth = json_int(params, "depth", 2);
    if (depth < 1) depth = 1;
    if (depth > 5) depth = 5;

    /* BFS traversal up to requested depth */
    mnemon_graph_t *graph = mnemon_storage_graph(s);
    MDB_txn *txn;
    mnemon_err_t err = mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn);
    if (err != MNEMON_OK) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", mnemon_strerror(err));
        return e;
    }

    graph_visit_ctx_t ctx = {
        .nodes     = cJSON_CreateArray(),
        .count     = 0,
        .max_nodes = 200
    };
    err = mnemon_graph_bfs(graph, txn, id, depth, graph_visit_cb, &ctx);
    if (err != MNEMON_OK) {
        mnemon_graph_txn_abort(txn);
        cJSON_Delete(ctx.nodes);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", mnemon_strerror(err));
        return e;
    }

    /* Collect edges for all visited entities */
    cJSON *edges_arr = cJSON_CreateArray();
    int edge_count = 0;
    cJSON *node;
    cJSON_ArrayForEach(node, ctx.nodes) {
        const cJSON *nid = cJSON_GetObjectItemCaseSensitive(node, "id");
        if (!cJSON_IsString(nid)) continue;

        uint8_t node_id[16];
        if (uuid_from_json(nid, node_id) != 0) continue;

        mnemon_edge_list_t edges = {0};
        mnemon_graph_get_edges_from(graph, txn, node_id, NULL, &edges);
        for (uint32_t i = 0; i < edges.count && edge_count < 500; i++) {
            cJSON *e = cJSON_CreateObject();
            char eid[37]; uuid_to_json_str(edges.edges[i].id, eid);
            cJSON_AddStringToObject(e, "id", eid);
            char sid[37]; uuid_to_json_str(edges.edges[i].source_id, sid);
            cJSON_AddStringToObject(e, "source_id", sid);
            char tid[37]; uuid_to_json_str(edges.edges[i].target_id, tid);
            cJSON_AddStringToObject(e, "target_id", tid);
            if (edges.edges[i].edge_type)
                cJSON_AddStringToObject(e, "edge_type", edges.edges[i].edge_type);
            cJSON_AddItemToArray(edges_arr, e);
            edge_count++;
        }
        mnemon_edge_list_free(&edges);
    }

    mnemon_graph_txn_abort(txn);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "depth", depth);
    cJSON_AddItemToObject(result, "nodes", ctx.nodes);
    cJSON_AddItemToObject(result, "edges", edges_arr);
    return result;
}

static cJSON *tool_import_batch(mnemon_storage_t *s, const cJSON *params)
{
    const cJSON *memories = cJSON_GetObjectItemCaseSensitive(params, "memories");
    if (!cJSON_IsArray(memories)) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "memories array required");
        return e;
    }

    int imported = 0, skipped = 0, errors = 0;
    int count = cJSON_GetArraySize(memories);
    if (count > 1000) count = 1000;
    int64_t start = mnemon_time_ms();

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(memories, i);
        const char *content = json_str(item, "content", NULL);
        if (!content) { skipped++; continue; }
        if (strlen(content) > 64 * 1024) { skipped++; continue; }
        if (mnemon_secret_detected(content, strlen(content))) { skipped++; continue; }

        mnemon_memory_t mem = {0};
        mnemon_uuid_t u; mnemon_uuid_generate(&u);
        memcpy(mem.id, u.bytes, 16);
        mem.content = strdup(content);
        mem.source_type = strdup(json_str(item, "source_type", "import"));
        mem.source_id = strdup(json_str(item, "source_id", ""));
        const char *sa = json_str(item, "source_author", NULL);
        mem.source_author = sa ? strdup(sa) : NULL;
        mem.importance = 0.5f;

        const char *ca_str = json_str(item, "created_at", NULL);
        if (ca_str) {
            int64_t ca = mnemon_parse_iso8601(ca_str);
            mem.created_at = (ca > 0) ? ca : mnemon_time_ms();
        } else {
            mem.created_at = mnemon_time_ms();
        }
        mem.last_accessed = mem.created_at;

        const char *tier_str = json_str(item, "tier", "episodic");
        if (strcmp(tier_str, "semantic") == 0) mem.tier = MNEMON_TIER_SEMANTIC;
        else if (strcmp(tier_str, "procedural") == 0) mem.tier = MNEMON_TIER_PROCEDURAL;

        mnemon_embed_t *embed = mnemon_storage_embed(s);
        if (embed && mnemon_embed_available(embed)) {
            int dims = mnemon_embed_dimensions(embed);
            mem.embedding = malloc((size_t)dims * sizeof(float));
            if (mem.embedding)
                mnemon_embed_text(embed, content, strlen(content), mem.embedding, dims, false);
        }

        if (mnemon_store_memory(s, &mem) == MNEMON_OK)
            imported++;
        else
            errors++;

        mnemon_memory_free(&mem);
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "imported", imported);
    cJSON_AddNumberToObject(result, "skipped", skipped);
    cJSON_AddNumberToObject(result, "errors", errors);
    cJSON_AddNumberToObject(result, "duration_ms", (double)(mnemon_time_ms() - start));
    return result;
}

static cJSON *tool_import_file(mnemon_storage_t *s, const cJSON *params)
{
    const char *path = json_str(params, "path", NULL);
    if (!path) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "path is required");
        return e;
    }

    /* Path validation */
    if (!mnemon_import_path_allowed(path, "~")) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "path not in allowed directories");
        return e;
    }

    const char *format = json_str(params, "format", "auto");
    mnemon_import_opts_t opts = {0};
    opts.source_type = json_str(params, "source_type", "import");
    opts.chunking = json_str(params, "chunking", "paragraph");
    opts.max_chunk_size = json_int(params, "max_chunk_size", 4096);
    opts.preserve_timestamps = json_bool(params, "preserve_timestamps", false);

    mnemon_import_result_t result_data = {0};
    mnemon_err_t err = mnemon_import_file(s, path, format, &opts, &result_data);

    cJSON *result = cJSON_CreateObject();
    if (err == MNEMON_OK) {
        cJSON_AddNumberToObject(result, "imported", result_data.imported);
        cJSON_AddNumberToObject(result, "skipped", result_data.skipped);
        cJSON_AddNumberToObject(result, "errors", result_data.errors);
        cJSON_AddNumberToObject(result, "chunks_created", result_data.chunks_created);
        cJSON_AddNumberToObject(result, "duration_ms", (double)result_data.duration_ms);
    } else {
        cJSON_AddStringToObject(result, "error", mnemon_err_msg());
    }
    return result;
}

/* Forward declaration */
#define IMPORT_MAX_DEPTH 16
static void import_dir_walk(mnemon_storage_t *s, const char *dirpath,
                            const char *pattern, const char *format,
                            const mnemon_import_opts_t *opts,
                            bool recursive, int depth,
                            int *files_processed, int *files_skipped,
                            int *total_imported);

/* ---- Background import job tracking ---- */

#define MAX_IMPORT_JOBS 16

typedef enum {
    IMPORT_JOB_RUNNING,
    IMPORT_JOB_COMPLETE,
    IMPORT_JOB_FAILED
} import_job_status_t;

typedef struct {
    char               id[37];
    char               path[4096];
    import_job_status_t status;
    int                files_processed;
    int                files_skipped;
    int                total_imported;
    int64_t            start_time;
    int64_t            end_time;
    pthread_t          thread;
    bool               active;
    /* Thread args (owned by job, freed after thread completes) */
    mnemon_storage_t  *storage;
    char               pattern[256];
    char               format[64];
    mnemon_import_opts_t opts;
    bool               recursive;
    /* Owned strings for opts fields */
    char               opts_source_type[128];
    char               opts_chunking[64];
} import_job_t;

static import_job_t  g_import_jobs[MAX_IMPORT_JOBS];
static pthread_mutex_t g_import_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *import_job_thread(void *arg)
{
    import_job_t *job = (import_job_t *)arg;
    import_dir_walk(job->storage, job->path, job->pattern, job->format,
                    &job->opts, job->recursive, 0,
                    &job->files_processed, &job->files_skipped,
                    &job->total_imported);
    job->end_time = mnemon_time_ms();
    job->status = IMPORT_JOB_COMPLETE;
    return NULL;
}

/* Match filename against simple glob pattern ("*" or "*.ext") */
static bool glob_match(const char *pattern, const char *name)
{
    if (strcmp(pattern, "*") == 0) return true;
    const char *star = strchr(pattern, '*');
    if (star && star == pattern) {
        const char *suffix = star + 1;
        size_t slen = strlen(suffix);
        size_t nlen = strlen(name);
        return (nlen >= slen && strcmp(name + nlen - slen, suffix) == 0);
    }
    return strcmp(pattern, name) == 0;
}

/* Recursive directory walker */
static void import_dir_walk(mnemon_storage_t *s, const char *dirpath,
                            const char *pattern, const char *format,
                            const mnemon_import_opts_t *opts,
                            bool recursive, int depth,
                            int *files_processed, int *files_skipped,
                            int *total_imported)
{
    if (depth > IMPORT_MAX_DEPTH) return;

    DIR *dir = opendir(dirpath);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char filepath[4096];
        snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, entry->d_name);

        struct stat st;
        if (stat(filepath, &st) != 0) {
            (*files_skipped)++;
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (recursive) {
                import_dir_walk(s, filepath, pattern, format, opts,
                                true, depth + 1,
                                files_processed, files_skipped,
                                total_imported);
            }
            continue;
        }

        if (!S_ISREG(st.st_mode)) {
            (*files_skipped)++;
            continue;
        }

        if (!glob_match(pattern, entry->d_name)) {
            (*files_skipped)++;
            continue;
        }

        mnemon_import_result_t r = {0};
        if (mnemon_import_file(s, filepath, format, opts, &r) == MNEMON_OK) {
            *total_imported += r.imported;
            (*files_processed)++;
        } else {
            (*files_skipped)++;
        }
    }

    closedir(dir);
}

static cJSON *tool_import_directory(mnemon_storage_t *s, const cJSON *params)
{
    const char *path = json_str(params, "path", NULL);
    if (!path) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "path is required");
        return e;
    }

    if (!mnemon_import_path_allowed(path, "~")) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "path not in allowed directories");
        return e;
    }

    /* Verify directory exists */
    DIR *dir = opendir(path);
    if (!dir) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "cannot open directory");
        return e;
    }
    closedir(dir);

    const char *pattern = json_str(params, "pattern", "*");
    const char *format = json_str(params, "format", "auto");
    bool recursive = json_bool(params, "recursive", false);
    bool async = json_bool(params, "async", false);

    mnemon_import_opts_t opts = {0};
    const char *source_type = json_str(params, "source_type", "document");
    const char *chunking = json_str(params, "chunking", "paragraph");
    opts.source_type = source_type;
    opts.chunking = chunking;
    opts.max_chunk_size = json_int(params, "max_chunk_size", 4096);
    opts.preserve_timestamps = json_bool(params, "preserve_timestamps", false);

    if (async) {
        /* Launch background import job */
        pthread_mutex_lock(&g_import_mutex);
        import_job_t *job = NULL;
        for (int i = 0; i < MAX_IMPORT_JOBS; i++) {
            if (!g_import_jobs[i].active) {
                job = &g_import_jobs[i];
                break;
            }
        }
        if (!job) {
            pthread_mutex_unlock(&g_import_mutex);
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "error",
                "max concurrent import jobs reached");
            return e;
        }

        memset(job, 0, sizeof(*job));
        job->active = true;
        job->status = IMPORT_JOB_RUNNING;
        job->storage = s;
        job->recursive = recursive;
        job->start_time = mnemon_time_ms();
        snprintf(job->path, sizeof(job->path), "%s", path);
        snprintf(job->pattern, sizeof(job->pattern), "%s", pattern);
        snprintf(job->format, sizeof(job->format), "%s", format);
        snprintf(job->opts_source_type, sizeof(job->opts_source_type),
                 "%s", source_type);
        snprintf(job->opts_chunking, sizeof(job->opts_chunking),
                 "%s", chunking);
        job->opts.source_type = job->opts_source_type;
        job->opts.chunking = job->opts_chunking;
        job->opts.max_chunk_size = opts.max_chunk_size;

        /* Generate job ID */
        mnemon_uuid_t uuid;
        mnemon_uuid_generate(&uuid);
        mnemon_uuid_to_string(&uuid, job->id, sizeof(job->id));

        if (pthread_create(&job->thread, NULL, import_job_thread, job) != 0) {
            job->active = false;
            pthread_mutex_unlock(&g_import_mutex);
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "error", "failed to start import thread");
            return e;
        }
        pthread_detach(job->thread);
        pthread_mutex_unlock(&g_import_mutex);

        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "job_id", job->id);
        cJSON_AddStringToObject(result, "status", "running");
        cJSON_AddStringToObject(result, "path", path);
        return result;
    }

    /* Synchronous import */
    int files_processed = 0, files_skipped = 0, total_imported = 0;
    int64_t start = mnemon_time_ms();

    import_dir_walk(s, path, pattern, format, &opts, recursive, 0,
                    &files_processed, &files_skipped, &total_imported);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "files_processed", files_processed);
    cJSON_AddNumberToObject(result, "files_skipped", files_skipped);
    cJSON_AddNumberToObject(result, "memories_imported", total_imported);
    cJSON_AddNumberToObject(result, "duration_ms", (double)(mnemon_time_ms() - start));
    return result;
}

static cJSON *tool_get_import_status(mnemon_storage_t *s, const cJSON *params)
{
    (void)s;
    const char *job_id = json_str(params, "job_id", NULL);

    cJSON *result = cJSON_CreateObject();
    cJSON *jobs = cJSON_CreateArray();

    pthread_mutex_lock(&g_import_mutex);
    for (int i = 0; i < MAX_IMPORT_JOBS; i++) {
        import_job_t *job = &g_import_jobs[i];
        if (!job->active) continue;

        /* If filtering by job_id, skip non-matching */
        if (job_id && strcmp(job->id, job_id) != 0) continue;

        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "job_id", job->id);
        cJSON_AddStringToObject(j, "path", job->path);
        switch (job->status) {
        case IMPORT_JOB_RUNNING:  cJSON_AddStringToObject(j, "status", "running"); break;
        case IMPORT_JOB_COMPLETE: cJSON_AddStringToObject(j, "status", "complete"); break;
        case IMPORT_JOB_FAILED:   cJSON_AddStringToObject(j, "status", "failed"); break;
        }
        cJSON_AddNumberToObject(j, "files_processed", job->files_processed);
        cJSON_AddNumberToObject(j, "files_skipped", job->files_skipped);
        cJSON_AddNumberToObject(j, "memories_imported", job->total_imported);
        if (job->end_time > 0)
            cJSON_AddNumberToObject(j, "duration_ms",
                (double)(job->end_time - job->start_time));
        cJSON_AddItemToArray(jobs, j);

        /* Garbage collect completed jobs after reporting */
        if (job->status != IMPORT_JOB_RUNNING && job_id)
            job->active = false;
    }
    pthread_mutex_unlock(&g_import_mutex);

    cJSON_AddItemToObject(result, "jobs", jobs);
    return result;
}

static cJSON *tool_get_history(mnemon_storage_t *s, const cJSON *params)
{
    uint8_t id[16];
    if (uuid_from_json(cJSON_GetObjectItemCaseSensitive(params, "entity_id"), id) != 0) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "invalid entity_id");
        return e;
    }

    int64_t since = 0, until = 0;
    const char *since_str = json_str(params, "since", NULL);
    const char *until_str = json_str(params, "until", NULL);
    if (since_str) since = mnemon_parse_iso8601(since_str);
    if (until_str) until = mnemon_parse_iso8601(until_str);

    mnemon_version_list_t vl = {0};
    mnemon_err_t err = mnemon_get_history(s, id, since, until, &vl);
    if (err != MNEMON_OK) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", mnemon_strerror(err));
        return e;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *versions = cJSON_CreateArray();
    for (uint32_t i = 0; i < vl.count; i++) {
        cJSON *v = cJSON_CreateObject();
        char vid[37]; uuid_to_json_str(vl.versions[i].id, vid);
        cJSON_AddStringToObject(v, "id", vid);
        cJSON_AddStringToObject(v, "name", vl.versions[i].name ? vl.versions[i].name : "");
        char ts[32];
        mnemon_format_iso8601(vl.versions[i].created_at, ts, sizeof(ts));
        cJSON_AddStringToObject(v, "created_at", ts);
        mnemon_format_iso8601(vl.versions[i].updated_at, ts, sizeof(ts));
        cJSON_AddStringToObject(v, "updated_at", ts);
        cJSON_AddItemToArray(versions, v);
    }
    cJSON_AddItemToObject(result, "versions", versions);
    cJSON_AddNumberToObject(result, "count", vl.count);

    mnemon_version_list_free(&vl);
    return result;
}

static cJSON *tool_get_state_at_time(mnemon_storage_t *s, const cJSON *params)
{
    uint8_t id[16];
    if (uuid_from_json(cJSON_GetObjectItemCaseSensitive(params, "entity_id"), id) != 0) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "invalid entity_id");
        return e;
    }

    const char *ts_str = json_str(params, "timestamp", NULL);
    if (!ts_str) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "timestamp required");
        return e;
    }
    int64_t timestamp = mnemon_parse_iso8601(ts_str);

    mnemon_entity_t ent = {0};
    mnemon_err_t err = mnemon_get_state_at_time(s, id, timestamp, &ent);
    if (err != MNEMON_OK) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", mnemon_strerror(err));
        return e;
    }

    cJSON *result = cJSON_CreateObject();
    char id_str[37]; uuid_to_json_str(ent.id, id_str);
    cJSON_AddStringToObject(result, "id", id_str);
    cJSON_AddStringToObject(result, "name", ent.name ? ent.name : "");
    cJSON_AddStringToObject(result, "entity_type", ent.entity_type ? ent.entity_type : "");
    cJSON_AddNumberToObject(result, "observation_count", ent.observation_count);
    char ts[32];
    mnemon_format_iso8601(ent.created_at, ts, sizeof(ts));
    cJSON_AddStringToObject(result, "created_at", ts);

    mnemon_entity_free(&ent);
    return result;
}

static cJSON *tool_get_changes_since(mnemon_storage_t *s, const cJSON *params)
{
    const char *since_str = json_str(params, "since", NULL);
    if (!since_str) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "since required (ISO 8601)");
        return e;
    }
    int64_t since = mnemon_parse_iso8601(since_str);
    const char *entity_type = json_str(params, "entity_type", NULL);
    int top_k = json_int(params, "top_k", 50);

    mnemon_result_set_t rs = {0};
    mnemon_get_changes_since(s, since, entity_type, top_k, &rs);
    cJSON *result = result_set_to_json(&rs);
    mnemon_result_set_free(&rs);
    return result;
}

static cJSON *tool_prune_stale(mnemon_storage_t *s, const cJSON *params)
{
    int min_age_days = json_int(params, "min_age_days", 90);
    float min_importance_threshold = 0.1f;
    const cJSON *mi = cJSON_GetObjectItemCaseSensitive(params, "min_importance");
    if (cJSON_IsNumber(mi)) min_importance_threshold = (float)mi->valuedouble;
    bool dry_run = json_bool(params, "dry_run", true);

    int64_t now = mnemon_time_ms();
    int64_t age_cutoff = now - (int64_t)min_age_days * 86400000LL;

    /* Scan memories from LMDB */
    mnemon_graph_t *graph = mnemon_storage_graph(s);
    MDB_txn *txn;
    if (mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn) != MNEMON_OK) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "failed to open read transaction");
        return e;
    }

    MDB_dbi dbi;
    int rc = mdb_dbi_open(txn, "memories", 0, &dbi);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        cJSON *result = cJSON_CreateObject();
        cJSON *arr = cJSON_CreateArray();
        cJSON_AddItemToObject(result, "candidates", arr);
        return result;
    }

    MDB_cursor *cur;
    mdb_cursor_open(txn, dbi, &cur);

    cJSON *result = cJSON_CreateObject();
    cJSON *candidates = cJSON_CreateArray();
    int candidate_count = 0;

    MDB_val key, val;
    rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
    while (rc == 0 && candidate_count < 200) {
        mnemon_memory_t mem = {0};
        mnemon_graph_get_memory(graph, txn, key.mv_data, &mem);

        if (mem.created_at < age_cutoff) {
            float decayed = mnemon_importance_decay(mem.importance,
                                                    mem.last_accessed, now, 90);
            if (decayed < min_importance_threshold) {
                cJSON *c = cJSON_CreateObject();
                char id_str[37]; uuid_to_json_str(mem.id, id_str);
                cJSON_AddStringToObject(c, "id", id_str);
                /* Content preview: first 200 chars */
                if (mem.content) {
                    char preview[201];
                    strncpy(preview, mem.content, 200);
                    preview[200] = '\0';
                    cJSON_AddStringToObject(c, "content_preview", preview);
                }
                cJSON_AddNumberToObject(c, "importance", decayed);
                cJSON_AddNumberToObject(c, "age_days",
                    (double)(now - mem.created_at) / 86400000.0);
                char ts[32];
                mnemon_format_iso8601(mem.last_accessed, ts, sizeof(ts));
                cJSON_AddStringToObject(c, "last_accessed", ts);
                cJSON_AddItemToArray(candidates, c);
                candidate_count++;

                /* Actually delete if not dry_run */
                if (!dry_run)
                    mnemon_delete_memory(s, mem.id);
            }
        }

        mnemon_memory_free(&mem);
        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
    }

    mdb_cursor_close(cur);
    mnemon_graph_txn_abort(txn);

    cJSON_AddItemToObject(result, "candidates", candidates);
    cJSON_AddNumberToObject(result, "count", candidate_count);
    cJSON_AddBoolToObject(result, "dry_run", dry_run);
    return result;
}

static cJSON *tool_get_hardware_info(mnemon_storage_t *s, const cJSON *params)
{
    (void)s;
    (void)params;
    mnemon_hardware_t hw;
    memset(&hw, 0, sizeof(hw));
    mnemon_hardware_detect(&hw);

    cJSON *result = cJSON_CreateObject();

    cJSON *cpu = cJSON_CreateObject();
    cJSON_AddStringToObject(cpu, "model", hw.cpu_model);
    cJSON_AddNumberToObject(cpu, "cores", hw.cpu_cores);
    cJSON *simd = cJSON_CreateArray();
    if (hw.has_avx2) cJSON_AddItemToArray(simd, cJSON_CreateString("avx2"));
    if (hw.has_avx512f) cJSON_AddItemToArray(simd, cJSON_CreateString("avx512f"));
    if (hw.has_avx512bw) cJSON_AddItemToArray(simd, cJSON_CreateString("avx512bw"));
    cJSON_AddItemToObject(cpu, "simd_caps", simd);
    cJSON_AddItemToObject(result, "cpu", cpu);

    cJSON *gpu = cJSON_CreateObject();
    if (hw.gpu_vendor != MNEMON_GPU_NONE) {
        cJSON_AddStringToObject(gpu, "model", hw.gpu_model);
        const char *vendor_str = "unknown";
        if (hw.gpu_vendor == MNEMON_GPU_AMD) vendor_str = "AMD";
        else if (hw.gpu_vendor == MNEMON_GPU_NVIDIA) vendor_str = "NVIDIA";
        else if (hw.gpu_vendor == MNEMON_GPU_INTEL) vendor_str = "Intel";
        cJSON_AddStringToObject(gpu, "vendor", vendor_str);
        cJSON_AddNumberToObject(gpu, "vram_mb", (double)hw.gpu_vram_bytes / (1024*1024));
        if (hw.gpu_gtt_bytes > 0)
            cJSON_AddNumberToObject(gpu, "gtt_mb", (double)hw.gpu_gtt_bytes / (1024*1024));
        cJSON_AddBoolToObject(gpu, "rocm", hw.has_rocm);
    } else {
        cJSON_AddStringToObject(gpu, "model", "none");
    }
    if (hw.has_npu) {
        cJSON *npu = cJSON_CreateObject();
        cJSON_AddStringToObject(npu, "model", hw.npu_model);
        cJSON_AddItemToObject(result, "npu", npu);
    }
    cJSON_AddItemToObject(result, "gpu", gpu);

    cJSON_AddNumberToObject(result, "ram_mb", (double)hw.ram_total_bytes / (1024*1024));
    cJSON_AddNumberToObject(result, "numa_nodes", hw.numa_nodes);
    cJSON_AddBoolToObject(result, "has_nvme", hw.has_nvme);
    cJSON_AddStringToObject(result, "simd_dispatch", g_simd_ops.name);

    return result;
}

static cJSON *tool_get_index_stats(mnemon_storage_t *s, const cJSON *params)
{
    (void)params;
    mnemon_stats_t st = {0};
    mnemon_get_stats(s, &st);

    cJSON *result = cJSON_CreateObject();

    cJSON *lmdb = cJSON_CreateObject();
    cJSON_AddNumberToObject(lmdb, "entity_count", (double)st.total_entities);
    cJSON_AddNumberToObject(lmdb, "edge_count", (double)st.total_edges);
    cJSON_AddNumberToObject(lmdb, "memory_count", (double)st.total_memories);
    cJSON_AddItemToObject(result, "lmdb", lmdb);

    cJSON *fts5 = cJSON_CreateObject();
    cJSON_AddNumberToObject(fts5, "indexed_docs", (double)st.fts_indexed);
    cJSON_AddItemToObject(result, "fts5", fts5);

    cJSON *vector = cJSON_CreateObject();
    cJSON_AddNumberToObject(vector, "memory_vectors", (double)st.memory_vectors);
    cJSON_AddNumberToObject(vector, "entity_vectors", (double)st.entity_vectors);
    cJSON_AddNumberToObject(vector, "dimensions", 768);
    cJSON_AddItemToObject(result, "vector", vector);

    return result;
}

static cJSON *tool_rebuild_indexes(mnemon_storage_t *s, const cJSON *params)
{
    const char *target = json_str(params, "target", "all");
    int64_t start = mnemon_time_ms();
    mnemon_err_t err = mnemon_rebuild_indexes(s, target);
    cJSON *result = cJSON_CreateObject();
    if (err == MNEMON_OK) {
        cJSON *rebuilt = cJSON_CreateArray();
        if (strcmp(target, "all") == 0 || strcmp(target, "fts") == 0)
            cJSON_AddItemToArray(rebuilt, cJSON_CreateString("fts"));
        if (strcmp(target, "all") == 0 || strcmp(target, "vector") == 0)
            cJSON_AddItemToArray(rebuilt, cJSON_CreateString("vector"));
        cJSON_AddItemToObject(result, "rebuilt", rebuilt);
        cJSON_AddNumberToObject(result, "duration_ms", (double)(mnemon_time_ms() - start));
    } else {
        cJSON_AddStringToObject(result, "error", mnemon_err_msg());
    }
    return result;
}

/* ---- Decoy (honeypot) tool handlers ---- */

static cJSON *tool_decoy_admin(mnemon_storage_t *s, const cJSON *params)
{
    (void)s; (void)params;
    mnemon_log(MNEMON_LOG_WARNING, "HONEYPOT: decoy tool admin_reset_auth invoked");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", "insufficient privileges");
    return r;
}

static cJSON *tool_decoy_export(mnemon_storage_t *s, const cJSON *params)
{
    (void)s; (void)params;
    mnemon_log(MNEMON_LOG_WARNING, "HONEYPOT: decoy tool export_all_memories invoked");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", "insufficient privileges");
    return r;
}

static cJSON *tool_decoy_debug(mnemon_storage_t *s, const cJSON *params)
{
    (void)s; (void)params;
    mnemon_log(MNEMON_LOG_WARNING, "HONEYPOT: decoy tool debug_raw_query invoked");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", "debug mode not enabled");
    return r;
}

static cJSON *tool_decoy_config(mnemon_storage_t *s, const cJSON *params)
{
    (void)s; (void)params;
    mnemon_log(MNEMON_LOG_WARNING, "HONEYPOT: decoy tool set_system_config invoked");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", "insufficient privileges");
    return r;
}

/* ---- Temporal Event Tools ---- */

static cJSON *tool_extract_events(mnemon_storage_t *s, const cJSON *params)
{
    const char *content = json_str(params, "content", NULL);
    if (!content) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "content required");
        return e;
    }

    int context_year = json_int(params, "context_year", current_year());
    bool create_entities = json_bool(params, "create_entities", true);

    /* Optional memory_id: links extracted entities back to source memory */
    uint8_t mem_id[16] = {0};
    bool has_mem_id = false;
    const cJSON *mid_item = cJSON_GetObjectItemCaseSensitive(params,
                                                              "memory_id");
    if (mid_item && cJSON_IsString(mid_item)) {
        if (uuid_from_json(mid_item, mem_id) == 0)
            has_mem_id = true;
    }

    /* Embedding support for entity vectors */
    mnemon_embed_t *embed = mnemon_storage_embed(s);
    bool can_embed = embed && mnemon_embed_available(embed);
    int dims = can_embed ? mnemon_embed_dimensions(embed) : 0;

    mnemon_event_list_t events = {0};
    mnemon_extract_events(content, context_year, &events);

    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(result, "events");
    int created = 0;
    int edges_created = 0;

    for (int i = 0; i < events.count; i++) {
        cJSON *evt = cJSON_CreateObject();
        cJSON_AddStringToObject(evt, "description",
                                events.events[i].description);
        char ts[32];
        mnemon_format_iso8601(events.events[i].event_date, ts, sizeof(ts));
        cJSON_AddStringToObject(evt, "event_date", ts);
        cJSON_AddNumberToObject(evt, "days_epoch",
            (double)(events.events[i].event_date / (86400LL * 1000)));

        /* Optionally create entity for each event */
        if (create_entities && events.events[i].description) {
            mnemon_entity_t ent = {0};
            mnemon_uuid_t u;
            mnemon_uuid_generate(&u);
            memcpy(ent.id, u.bytes, 16);

            /* Use first ~60 chars of description as name */
            char name[64];
            snprintf(name, sizeof(name), "%.60s",
                     events.events[i].description);
            /* Trim at word boundary */
            char *sp = strrchr(name, ' ');
            if (sp && sp - name > 30) *sp = '\0';

            ent.name = strdup(name);
            ent.entity_type = strdup("event");
            ent.event_date = events.events[i].event_date;
            ent.importance = 0.5f;
            ent.created_at = mnemon_time_ms();
            ent.updated_at = ent.created_at;
            ent.observations = calloc(1, sizeof(char *));
            if (ent.observations) {
                ent.observations[0] = strdup(events.events[i].description);
                ent.observation_count = 1;
            }

            /* Embed entity description for vector search */
            if (can_embed) {
                const char *desc = events.events[i].description;
                ent.embedding = malloc((size_t)dims * sizeof(float));
                if (ent.embedding) {
                    if (mnemon_embed_text(embed, desc, strlen(desc),
                                          ent.embedding, dims, false) != MNEMON_OK) {
                        free(ent.embedding);
                        ent.embedding = NULL;
                    }
                }
            }

            if (mnemon_store_entity(s, &ent) == MNEMON_OK) {
                char eid[37];
                mnemon_uuid_to_string(&u, eid, sizeof(eid));
                cJSON_AddStringToObject(evt, "entity_id", eid);
                created++;

                /* Create edge: entity -> source memory */
                if (has_mem_id) {
                    mnemon_edge_t edge = {0};
                    mnemon_uuid_t eu;
                    mnemon_uuid_generate(&eu);
                    memcpy(edge.id, eu.bytes, 16);
                    memcpy(edge.source_id, ent.id, 16);
                    memcpy(edge.target_id, mem_id, 16);
                    edge.edge_type = strdup("extracted_from");
                    edge.weight = 1.0f;
                    edge.created_at = mnemon_time_ms();
                    if (mnemon_store_edge(s, &edge) == MNEMON_OK)
                        edges_created++;
                    mnemon_edge_free(&edge);
                }
            }
            mnemon_entity_free(&ent);
        }

        cJSON_AddItemToArray(arr, evt);
    }

    cJSON_AddNumberToObject(result, "extracted", events.count);
    cJSON_AddNumberToObject(result, "entities_created", created);
    cJSON_AddNumberToObject(result, "edges_created", edges_created);

    mnemon_event_list_free(&events);
    return result;
}

static cJSON *tool_search_events(mnemon_storage_t *s, const cJSON *params)
{
    int64_t since = 0, until = 0;
    const char *since_str = json_str(params, "since", NULL);
    const char *until_str = json_str(params, "until", NULL);
    const char *name = json_str(params, "name", NULL);
    int top_k = json_int(params, "top_k", 20);

    if (since_str) since = mnemon_parse_iso8601(since_str);
    if (until_str) until = mnemon_parse_iso8601(until_str);

    /* Also try natural date parsing for convenience */
    if (since_str && since == 0)
        since = mnemon_parse_natural_date(since_str, current_year());
    if (until_str && until == 0)
        until = mnemon_parse_natural_date(until_str, current_year());

    /* Date-only upper bound covers the entire day. */
    if (until > 0 && ts_is_date_only(until_str)) until += MS_PER_DAY - 1;

    mnemon_result_set_t rs = {0};
    mnemon_search_events(s, since, until, name, top_k, &rs);

    cJSON *result = result_set_to_json(&rs);
    mnemon_result_set_free(&rs);
    return result;
}

static cJSON *tool_calculate_duration(mnemon_storage_t *s, const cJSON *params)
{
    (void)s;

    const char *from_str = json_str(params, "from", NULL);
    const char *to_str = json_str(params, "to", NULL);

    if (!from_str || !to_str) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error",
            "both 'from' and 'to' dates are required");
        return e;
    }

    int context_year = json_int(params, "context_year", current_year());

    /* Parse dates -- try ISO8601 first, then natural language */
    int64_t from_ms = mnemon_parse_iso8601(from_str);
    if (from_ms == 0) from_ms = mnemon_parse_natural_date(from_str, context_year);
    int64_t to_ms = mnemon_parse_iso8601(to_str);
    if (to_ms == 0) to_ms = mnemon_parse_natural_date(to_str, context_year);

    if (from_ms <= 0 || to_ms <= 0) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "could not parse one or both dates");
        if (from_ms <= 0) cJSON_AddStringToObject(e, "from_raw", from_str);
        if (to_ms <= 0) cJSON_AddStringToObject(e, "to_raw", to_str);
        return e;
    }

    int days = mnemon_duration_days(from_ms, to_ms);
    bool from_is_earlier = (from_ms <= to_ms);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "days", days);
    cJSON_AddNumberToObject(result, "days_inclusive", days + 1);

    char buf[32];
    mnemon_format_iso8601(from_ms, buf, sizeof(buf));
    cJSON_AddStringToObject(result, "from", buf);
    mnemon_format_iso8601(to_ms, buf, sizeof(buf));
    cJSON_AddStringToObject(result, "to", buf);
    cJSON_AddBoolToObject(result, "from_is_earlier", from_is_earlier);

    if (days >= 7) {
        cJSON_AddNumberToObject(result, "weeks", days / 7);
        cJSON_AddNumberToObject(result, "remaining_days", days % 7);
    }

    return result;
}

/* ---- Tool Definition Table ---- */

#define SCHEMA(s) s

static const mnemon_tool_def_t tool_defs[] = {
    {"store_memory", tool_store_memory,
     "Store a new memory with embedding and optional entity extraction",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\",\"description\":\"Text content to store\"},\"source_type\":{\"type\":\"string\"},\"source_id\":{\"type\":\"string\"},\"source_author\":{\"type\":\"string\"},\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"tier\":{\"type\":\"string\",\"enum\":[\"episodic\",\"semantic\",\"procedural\"]},\"skip_secret_check\":{\"type\":\"boolean\"},\"created_at\":{\"type\":\"string\",\"description\":\"ISO8601 timestamp to assign (default: now)\"}},\"required\":[\"content\"]}")},

    {"retrieve_memory", tool_retrieve_memory,
     "Retrieve a memory by UUID",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"Memory UUID\"}},\"required\":[\"id\"]}")},

    {"update_memory", tool_update_memory,
     "Update a memory's content (re-embeds if changed)",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"id\"]}")},

    {"delete_memory", tool_delete_memory,
     "Soft-delete a memory",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}")},

    {"delete_entity", tool_delete_entity,
     "Delete an entity (removes it from the graph, FTS, and vector indexes)",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}")},

    {"search_hybrid", tool_search_hybrid,
     "Hybrid search: graph + vector + keyword with RRF fusion",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"top_k\":{\"type\":\"integer\"}},\"required\":[\"query\"]}")},

    {"search_semantic", tool_search_semantic,
     "Vector similarity search only",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"top_k\":{\"type\":\"integer\"}},\"required\":[\"query\"]}")},

    {"search_keyword", tool_search_keyword,
     "FTS5 BM25 keyword search only",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"top_k\":{\"type\":\"integer\"}},\"required\":[\"query\"]}")},

    {"search_temporal", tool_search_temporal,
     "Time-filtered search",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"since\":{\"type\":\"string\"},\"until\":{\"type\":\"string\"},\"top_k\":{\"type\":\"integer\"}}}")},

    {"create_entity", tool_create_entity,
     "Create a new entity in the knowledge graph",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"entity_type\":{\"type\":\"string\"},\"observations\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"name\",\"entity_type\"]}")},

    {"add_observation", tool_add_observation,
     "Add an observation to an existing entity",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"entity_id\":{\"type\":\"string\"},\"observation\":{\"type\":\"string\"}},\"required\":[\"entity_id\",\"observation\"]}")},

    {"create_relation", tool_create_relation,
     "Create a relation (edge) between two entities",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"source_id\":{\"type\":\"string\"},\"target_id\":{\"type\":\"string\"},\"edge_type\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"}},\"required\":[\"source_id\",\"target_id\",\"edge_type\"]}")},

    {"get_memory_stats", tool_get_memory_stats,
     "Get statistics about stored memories, entities, and edges",
     SCHEMA("{\"type\":\"object\",\"properties\":{}}")},

    {"health_check", tool_health_check,
     "Check daemon health and component status",
     SCHEMA("{\"type\":\"object\",\"properties\":{}}")},

    {"consolidate_memories", tool_consolidate,
     "Trigger episodic-to-semantic memory consolidation",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"topic\":{\"type\":\"string\"},\"dry_run\":{\"type\":\"boolean\"}}}")},

    {"list_memories", tool_list_memories,
     "List stored memories with optional filtering",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"source_type\":{\"type\":\"string\"},\"tier\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"}}}")},

    {"search_entities", tool_search_entities,
     "Search entities by name, type, or observations",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"entity_type\":{\"type\":\"string\"},\"top_k\":{\"type\":\"integer\"}},\"required\":[\"query\"]}")},

    {"get_entity_graph", tool_get_entity_graph,
     "Get an entity with its edges and related entities",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"entity_id\":{\"type\":\"string\"},\"depth\":{\"type\":\"integer\",\"default\":2,\"maximum\":5}},\"required\":[\"entity_id\"]}")},

    {"import_batch", tool_import_batch,
     "Import an array of memories in a single call (max 1000)",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"memories\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\"},\"source_type\":{\"type\":\"string\"},\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"tier\":{\"type\":\"string\"},\"created_at\":{\"type\":\"string\",\"description\":\"ISO8601 timestamp\"}}}}},\"required\":[\"memories\"]}")},

    {"import_file", tool_import_file,
     "Import memories from a local file (jsonl, csv, mbox, text, markdown)",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"format\":{\"type\":\"string\",\"enum\":[\"auto\",\"jsonl\",\"csv\",\"mbox\",\"text\",\"markdown\"]},\"source_type\":{\"type\":\"string\"},\"chunking\":{\"type\":\"string\",\"enum\":[\"paragraph\",\"line\",\"page\",\"none\"]},\"max_chunk_size\":{\"type\":\"integer\"},\"preserve_timestamps\":{\"type\":\"boolean\",\"description\":\"Use source timestamps (mbox Date header, JSONL created_at field) for created_at\"}},\"required\":[\"path\"]}")},

    {"import_directory", tool_import_directory,
     "Import all matching files from a directory",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"pattern\":{\"type\":\"string\",\"default\":\"*\"},\"format\":{\"type\":\"string\"},\"recursive\":{\"type\":\"boolean\"},\"source_type\":{\"type\":\"string\"}},\"required\":[\"path\"]}")},

    {"get_import_status", tool_get_import_status,
     "Check status of import operations",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\"}}}")},

    {"rebuild_indexes", tool_rebuild_indexes,
     "Rebuild FTS5 and/or usearch indexes from LMDB source of truth",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\",\"enum\":[\"fts\",\"vector\",\"all\"],\"default\":\"all\"}}}")},

    {"get_history", tool_get_history,
     "Get version history for an entity",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"entity_id\":{\"type\":\"string\"},\"since\":{\"type\":\"string\"},\"until\":{\"type\":\"string\"}},\"required\":[\"entity_id\"]}")},

    {"get_state_at_time", tool_get_state_at_time,
     "Get entity state as it existed at a specific point in time",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"entity_id\":{\"type\":\"string\"},\"timestamp\":{\"type\":\"string\"}},\"required\":[\"entity_id\",\"timestamp\"]}")},

    {"get_changes_since", tool_get_changes_since,
     "Get change feed of entities modified since a given time",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"since\":{\"type\":\"string\"},\"entity_type\":{\"type\":\"string\"},\"top_k\":{\"type\":\"integer\"}},\"required\":[\"since\"]}")},

    {"prune_stale", tool_prune_stale,
     "Find memories eligible for pruning based on age and importance decay",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"min_age_days\":{\"type\":\"integer\",\"default\":90},\"min_importance\":{\"type\":\"number\",\"default\":0.1},\"dry_run\":{\"type\":\"boolean\",\"default\":true}}}")},

    {"get_hardware_info", tool_get_hardware_info,
     "Get CPU, GPU, SIMD, RAM, NUMA, and storage detection results",
     SCHEMA("{\"type\":\"object\",\"properties\":{}}")},

    {"get_index_stats", tool_get_index_stats,
     "Get detailed per-engine statistics (LMDB, FTS5, vector)",
     SCHEMA("{\"type\":\"object\",\"properties\":{}}")},

    /* ---- Decoy tools (honeypot) ---- */
    /* These look like real admin tools but only log security alerts.
     * Interspersed among real tools to appear natural in tools/list. */

    {"admin_reset_auth", tool_decoy_admin,
     "Reset authentication tokens for all sessions (admin only)",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"admin_key\":{\"type\":\"string\"}},\"required\":[\"admin_key\"]}")},

    {"export_all_memories", tool_decoy_export,
     "Export complete memory database as JSONL (admin only)",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"format\":{\"type\":\"string\"},\"include_embeddings\":{\"type\":\"boolean\"}}}")},

    {"debug_raw_query", tool_decoy_debug,
     "Execute raw LMDB query (debug mode only)",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"database\":{\"type\":\"string\"}}}")},

    {"set_system_config", tool_decoy_config,
     "Update runtime configuration values",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"key\",\"value\"]}")},

    {"extract_events", tool_extract_events,
     "Extract dated events from text and create event entities with timestamps. Entities are embedded for vector search and linked to source memory via edges when memory_id is provided.",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\",\"description\":\"Text to extract events from\"},\"context_year\":{\"type\":\"integer\",\"description\":\"Default year for dates without explicit year\"},\"create_entities\":{\"type\":\"boolean\",\"description\":\"Create event entities in knowledge graph (default true)\"},\"memory_id\":{\"type\":\"string\",\"description\":\"UUID of source memory to link entities back to via edges\"}},\"required\":[\"content\"]}")},

    {"search_events", tool_search_events,
     "Search for dated events by event date range. Returns events sorted chronologically. Unlike search_temporal (which filters by storage time), this searches by the actual event dates extracted from content.",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"since\":{\"type\":\"string\",\"description\":\"Start date (ISO8601 or natural like 'January 10')\"},\"until\":{\"type\":\"string\",\"description\":\"End date\"},\"name\":{\"type\":\"string\",\"description\":\"Filter by event name substring\"},\"top_k\":{\"type\":\"integer\"}}}")},

    {"calculate_duration", tool_calculate_duration,
     "Calculate the number of days between two dates. Accepts ISO8601 or natural language dates like 'January 10' or 'March 15, 2023'. Returns days, days_inclusive (counting both endpoints), and weeks.",
     SCHEMA("{\"type\":\"object\",\"properties\":{\"from\":{\"type\":\"string\",\"description\":\"Start date\"},\"to\":{\"type\":\"string\",\"description\":\"End date\"},\"context_year\":{\"type\":\"integer\",\"description\":\"Default year for dates without explicit year\"}},\"required\":[\"from\",\"to\"]}")},
};

#define TOOL_COUNT (sizeof(tool_defs) / sizeof(tool_defs[0]))

int mnemon_get_tool_defs(const mnemon_tool_def_t **out)
{
    if (out) *out = tool_defs;
    return (int)TOOL_COUNT;
}
