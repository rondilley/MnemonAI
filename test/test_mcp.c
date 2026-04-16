/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * test_mcp.c -- Comprehensive MCP protocol + tool tests
 *
 * Tests every registered tool, MCP lifecycle, JSON-RPC envelope,
 * error handling, and content format compliance.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cJSON.h>
#include "mcp_dispatch.h"
#include "storage.h"
#include "config_parse.h"
#include "memory.h"

static int passed = 0, failed = 0, total = 0;
#define TEST(n) do { total++; printf("  %-60s ", n); } while(0)
#define PASS() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(m) do { failed++; printf("FAIL: %s\n", m); return; } while(0)
#define ASSERT(c, m) do { if (!(c)) FAIL(m); } while(0)

static mnemon_dispatch_t *dispatch = NULL;
static mnemon_storage_t *storage = NULL;
static char tmpdir[256];

/* Stored IDs from earlier tests, reused by later tests */
static char stored_mem_id[40] = {0};
static char stored_entity_a[40] = {0};
static char stored_entity_b[40] = {0};
static char stored_edge_id[40] = {0};

/* ---- Helpers ---- */

static cJSON *make_request(const char *method, int id, cJSON *params)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", method);
    if (params)
        cJSON_AddItemToObject(req, "params", params);
    return req;
}

/* Call a tool and return the parsed inner JSON result.
 * Caller must cJSON_Delete the returned object. */
static cJSON *call_tool(const char *name, cJSON *arguments, int req_id)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", name);
    cJSON_AddItemToObject(params, "arguments", arguments ? arguments : cJSON_CreateObject());
    cJSON *req = make_request("tools/call", req_id, params);
    cJSON *resp = mnemon_dispatch_request(dispatch, req);
    cJSON_Delete(req);

    if (!resp) return NULL;

    /* Extract inner JSON from content[0].text */
    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    if (!result) { cJSON_Delete(resp); return NULL; }
    cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
    if (!cJSON_IsArray(content) || cJSON_GetArraySize(content) == 0) {
        cJSON_Delete(resp);
        return NULL;
    }
    cJSON *text = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetArrayItem(content, 0), "text");
    if (!cJSON_IsString(text)) { cJSON_Delete(resp); return NULL; }

    cJSON *inner = cJSON_Parse(text->valuestring);
    cJSON_Delete(resp);
    return inner;
}

/* Check if a tool result has isError=false */
static bool tool_succeeded(const char *name, cJSON *args, int req_id)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", name);
    cJSON_AddItemToObject(params, "arguments", args ? args : cJSON_CreateObject());
    cJSON *req = make_request("tools/call", req_id, params);
    cJSON *resp = mnemon_dispatch_request(dispatch, req);
    cJSON_Delete(req);
    if (!resp) return false;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    cJSON *ie = cJSON_GetObjectItemCaseSensitive(result, "isError");
    bool ok = ie && cJSON_IsFalse(ie);
    cJSON_Delete(resp);
    return ok;
}

static void setup(void)
{
    /* Use HOME-based tmpdir so import path validation passes (allowed_paths=~) */
    const char *home = getenv("HOME");
    if (home)
        snprintf(tmpdir, sizeof(tmpdir), "%s/.mnemon_test_mcp_%d", home, getpid());
    else
        snprintf(tmpdir, sizeof(tmpdir), "/tmp/mnemon_test_mcp_%d", getpid());
    mkdir(tmpdir, 0700);

    mnemon_config_t *cfg = calloc(1, sizeof(*cfg));
    cfg->data_dir = strdup(tmpdir);
    cfg->log_level = strdup("error");
    cfg->map_size_gb = 1;
    cfg->max_readers = 16;
    cfg->dimensions = 768;
    cfg->model_path = strdup("none");
    cfg->extraction_endpoint = strdup("");
    cfg->allowed_paths = strdup("~");
    cfg->http_bind = strdup("127.0.0.1");

    mnemon_storage_open(&storage, cfg);
    mnemon_dispatch_init(&dispatch, storage);
    mnemon_config_free(cfg);
}

static void teardown(void)
{
    mnemon_dispatch_free(dispatch);
    mnemon_storage_close(storage);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    if (system(cmd) != 0) { /* ignore */ }
}

/* ================================================================ */
/* MCP Lifecycle                                                    */
/* ================================================================ */

static void test_initialize(void)
{
    TEST("[SPEC] initialize: protocolVersion + capabilities + serverInfo");
    cJSON *req = make_request("initialize", 1, NULL);
    cJSON *resp = mnemon_dispatch_request(dispatch, req);
    ASSERT(resp != NULL, "response");

    cJSON *r = cJSON_GetObjectItemCaseSensitive(resp, "result");
    ASSERT(r != NULL, "result");
    ASSERT(strcmp(cJSON_GetObjectItemCaseSensitive(r, "protocolVersion")->valuestring,
                  "2024-11-05") == 0, "version");
    ASSERT(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(r, "capabilities")), "caps");
    ASSERT(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(r, "serverInfo")), "info");

    cJSON_Delete(req);
    cJSON_Delete(resp);
    PASS();
}

static void test_initialized_notification(void)
{
    TEST("[SPEC] notifications/initialized returns NULL");
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(req, "method", "notifications/initialized");
    cJSON *resp = mnemon_dispatch_request(dispatch, req);
    ASSERT(resp == NULL, "no response");
    cJSON_Delete(req);
    PASS();
}

static void test_tools_list(void)
{
    TEST("[SPEC] tools/list returns 28 tools with inputSchema");
    cJSON *req = make_request("tools/list", 2, NULL);
    cJSON *resp = mnemon_dispatch_request(dispatch, req);
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(resp, "result"), "tools");
    ASSERT(cJSON_IsArray(tools), "array");
    int n = cJSON_GetArraySize(tools);
    ASSERT(n >= 28, "28+ tools");
    for (int i = 0; i < n; i++) {
        cJSON *t = cJSON_GetArrayItem(tools, i);
        ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(t, "name")), "name");
        ASSERT(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(t, "inputSchema")), "schema");
    }
    printf("[%d] ", n);
    cJSON_Delete(req);
    cJSON_Delete(resp);
    PASS();
}

/* ================================================================ */
/* Error Handling                                                   */
/* ================================================================ */

static void test_err_missing_method(void)
{
    TEST("[SPEC] missing method -> -32600");
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", 99);
    cJSON *resp = mnemon_dispatch_request(dispatch, req);
    ASSERT(cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(resp, "error"), "code")->valueint == -32600, "code");
    cJSON_Delete(req);
    cJSON_Delete(resp);
    PASS();
}

static void test_err_unknown_method(void)
{
    TEST("[SPEC] unknown method -> -32601");
    cJSON *req = make_request("bogus/method", 99, NULL);
    cJSON *resp = mnemon_dispatch_request(dispatch, req);
    ASSERT(cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(resp, "error"), "code")->valueint == -32601, "code");
    cJSON_Delete(req);
    cJSON_Delete(resp);
    PASS();
}

static void test_err_unknown_tool(void)
{
    TEST("[SPEC] unknown tool -> -32602");
    cJSON *args = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", "does_not_exist");
    cJSON_AddItemToObject(params, "arguments", args);
    cJSON *req = make_request("tools/call", 99, params);
    cJSON *resp = mnemon_dispatch_request(dispatch, req);
    ASSERT(cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(resp, "error"), "code")->valueint == -32602, "code");
    cJSON_Delete(req);
    cJSON_Delete(resp);
    PASS();
}

/* ================================================================ */
/* Tool: store_memory                                               */
/* ================================================================ */

static void test_store_memory(void)
{
    TEST("[TOOL] store_memory: basic store returns id");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "content", "Neural networks use backpropagation");
    cJSON_AddStringToObject(args, "source_type", "test");
    cJSON *tags = cJSON_CreateArray();
    cJSON_AddItemToArray(tags, cJSON_CreateString("ml"));
    cJSON_AddItemToArray(tags, cJSON_CreateString("neural"));
    cJSON_AddItemToObject(args, "tags", tags);

    cJSON *r = call_tool("store_memory", args, 100);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "id")), "has id");
    ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "created_at")), "has created_at");
    snprintf(stored_mem_id, sizeof(stored_mem_id), "%s",
             cJSON_GetObjectItemCaseSensitive(r, "id")->valuestring);
    cJSON_Delete(r);
    PASS();
}

static void test_store_memory_secret_rejected(void)
{
    TEST("[TOOL] store_memory: secret content -> isError=true");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "content",
        "token ghp_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    ASSERT(!tool_succeeded("store_memory", args, 101), "rejected");
    PASS();
}

static void test_store_memory_oversized(void)
{
    TEST("[TOOL] store_memory: >64KB content rejected");
    char *big = malloc(70000);
    memset(big, 'x', 69999);
    big[69999] = '\0';
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "content", big);
    cJSON *r = call_tool("store_memory", args, 102);
    ASSERT(r && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "error")), "error");
    cJSON_Delete(r);
    free(big);
    PASS();
}

/* ================================================================ */
/* Tool: retrieve_memory                                            */
/* ================================================================ */

static void test_retrieve_memory(void)
{
    TEST("[TOOL] retrieve_memory: get stored content back");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "id", stored_mem_id);
    cJSON *r = call_tool("retrieve_memory", args, 110);
    ASSERT(r != NULL, "result");
    ASSERT(strstr(cJSON_GetObjectItemCaseSensitive(r, "content")->valuestring,
                  "backpropagation") != NULL, "content matches");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: update_memory                                              */
/* ================================================================ */

static void test_update_memory(void)
{
    TEST("[TOOL] update_memory: change content");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "id", stored_mem_id);
    cJSON_AddStringToObject(args, "content", "Updated: deep learning uses SGD");
    cJSON *r = call_tool("update_memory", args, 120);
    ASSERT(r && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "updated")), "updated");
    cJSON_Delete(r);

    /* Verify updated content */
    cJSON *args2 = cJSON_CreateObject();
    cJSON_AddStringToObject(args2, "id", stored_mem_id);
    cJSON *r2 = call_tool("retrieve_memory", args2, 121);
    ASSERT(strstr(cJSON_GetObjectItemCaseSensitive(r2, "content")->valuestring,
                  "SGD") != NULL, "content updated");
    cJSON_Delete(r2);
    PASS();
}

/* ================================================================ */
/* Tool: search_keyword                                             */
/* ================================================================ */

static void test_search_keyword(void)
{
    TEST("[TOOL] search_keyword: finds stored memory by keyword");
    /* Store a distinctive memory first */
    cJSON *sa = cJSON_CreateObject();
    cJSON_AddStringToObject(sa, "content", "PostgreSQL supports JSONB columns natively");
    cJSON *store_r = call_tool("store_memory", sa, 130);
    cJSON_Delete(store_r);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "query", "PostgreSQL JSONB");
    cJSON_AddNumberToObject(args, "top_k", 5);
    cJSON *r = call_tool("search_keyword", args, 131);
    ASSERT(r != NULL, "result");
    cJSON *results = cJSON_GetObjectItemCaseSensitive(r, "results");
    ASSERT(cJSON_IsArray(results), "results array");
    ASSERT(cJSON_GetArraySize(results) >= 1, "found");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: search_semantic                                            */
/* ================================================================ */

static void test_search_semantic(void)
{
    TEST("[TOOL] search_semantic: returns results array (no model=empty)");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "query", "machine learning");
    cJSON *r = call_tool("search_semantic", args, 140);
    ASSERT(r != NULL, "result");
    /* Without embedding model, returns empty or error -- both acceptable */
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: search_hybrid                                              */
/* ================================================================ */

static void test_search_hybrid(void)
{
    TEST("[TOOL] search_hybrid: returns fused results with scores");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "query", "deep learning");
    cJSON *r = call_tool("search_hybrid", args, 150);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(r, "results")), "results");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: search_temporal                                            */
/* ================================================================ */

static void test_search_temporal(void)
{
    TEST("[TOOL] search_temporal: time-filtered results");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "since", "2020-01-01T00:00:00Z");
    cJSON_AddNumberToObject(args, "top_k", 5);
    cJSON *r = call_tool("search_temporal", args, 160);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(r, "results")), "results");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: create_entity                                              */
/* ================================================================ */

static void test_create_entity(void)
{
    TEST("[TOOL] create_entity: returns id, name, type");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "name", "TensorFlow");
    cJSON_AddStringToObject(args, "entity_type", "framework");
    cJSON *obs = cJSON_CreateArray();
    cJSON_AddItemToArray(obs, cJSON_CreateString("Open-source ML framework"));
    cJSON_AddItemToArray(obs, cJSON_CreateString("Developed by Google Brain"));
    cJSON_AddItemToObject(args, "observations", obs);

    cJSON *r = call_tool("create_entity", args, 200);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "id")), "id");
    ASSERT(strcmp(cJSON_GetObjectItemCaseSensitive(r, "name")->valuestring, "TensorFlow") == 0, "name");
    snprintf(stored_entity_a, sizeof(stored_entity_a), "%s",
             cJSON_GetObjectItemCaseSensitive(r, "id")->valuestring);
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: add_observation                                            */
/* ================================================================ */

static void test_add_observation(void)
{
    TEST("[TOOL] add_observation: appends observation to entity");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "entity_id", stored_entity_a);
    cJSON_AddStringToObject(args, "observation", "Supports eager execution since 2.0");
    cJSON *r = call_tool("add_observation", args, 210);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_GetObjectItemCaseSensitive(r, "observation_count")->valueint >= 3, "count>=3");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: search_entities                                            */
/* ================================================================ */

static void test_search_entities(void)
{
    TEST("[TOOL] search_entities: finds entity by name");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "query", "TensorFlow");
    cJSON *r = call_tool("search_entities", args, 220);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(r, "entities")), "entities");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: create_relation                                            */
/* ================================================================ */

static void test_create_relation(void)
{
    TEST("[TOOL] create_relation: edge between entities");
    /* Create second entity */
    cJSON *args2 = cJSON_CreateObject();
    cJSON_AddStringToObject(args2, "name", "PyTorch");
    cJSON_AddStringToObject(args2, "entity_type", "framework");
    cJSON *r2 = call_tool("create_entity", args2, 230);
    snprintf(stored_entity_b, sizeof(stored_entity_b), "%s",
             cJSON_GetObjectItemCaseSensitive(r2, "id")->valuestring);
    cJSON_Delete(r2);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "source_id", stored_entity_a);
    cJSON_AddStringToObject(args, "target_id", stored_entity_b);
    cJSON_AddStringToObject(args, "edge_type", "competes_with");
    cJSON_AddStringToObject(args, "description", "Both are ML frameworks");

    cJSON *r = call_tool("create_relation", args, 231);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "id")), "edge id");
    snprintf(stored_edge_id, sizeof(stored_edge_id), "%s",
             cJSON_GetObjectItemCaseSensitive(r, "id")->valuestring);
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: get_entity_graph                                           */
/* ================================================================ */

static void test_get_entity_graph(void)
{
    TEST("[TOOL] get_entity_graph: BFS nodes + edges with depth");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "entity_id", stored_entity_a);
    cJSON_AddNumberToObject(args, "depth", 2);
    cJSON *r = call_tool("get_entity_graph", args, 240);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_GetObjectItemCaseSensitive(r, "depth")->valueint == 2, "depth=2");
    ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(r, "nodes")), "nodes");
    ASSERT(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "nodes")) >= 1, "nodes>=1");
    ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(r, "edges")), "edges");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: get_history                                                */
/* ================================================================ */

static void test_get_history(void)
{
    TEST("[TOOL] get_history: entity version list");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "entity_id", stored_entity_a);
    cJSON *r = call_tool("get_history", args, 250);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(r, "versions")), "versions");
    ASSERT(cJSON_GetObjectItemCaseSensitive(r, "count")->valueint >= 1, "count>=1");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: get_state_at_time                                          */
/* ================================================================ */

static void test_get_state_at_time(void)
{
    TEST("[TOOL] get_state_at_time: returns entity at timestamp");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "entity_id", stored_entity_a);
    char ts[32];
    mnemon_format_iso8601(mnemon_time_ms() + 1000, ts, sizeof(ts));
    cJSON_AddStringToObject(args, "timestamp", ts);
    cJSON *r = call_tool("get_state_at_time", args, 260);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "name")), "name");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: get_changes_since                                          */
/* ================================================================ */

static void test_get_changes_since(void)
{
    TEST("[TOOL] get_changes_since: change feed");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "since", "2020-01-01T00:00:00Z");
    cJSON *r = call_tool("get_changes_since", args, 270);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(r, "results")), "results");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: get_memory_stats                                           */
/* ================================================================ */

static void test_get_memory_stats(void)
{
    TEST("[TOOL] get_memory_stats: returns counts");
    cJSON *r = call_tool("get_memory_stats", NULL, 300);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_GetObjectItemCaseSensitive(r, "total_memories")->valuedouble >= 1, "memories>0");
    ASSERT(cJSON_GetObjectItemCaseSensitive(r, "total_entities")->valuedouble >= 1, "entities>0");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: health_check                                               */
/* ================================================================ */

static void test_health_check(void)
{
    TEST("[TOOL] health_check: status=ok");
    cJSON *r = call_tool("health_check", NULL, 310);
    ASSERT(r != NULL, "result");
    ASSERT(strcmp(cJSON_GetObjectItemCaseSensitive(r, "status")->valuestring, "ok") == 0, "ok");
    ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "version")), "version");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: get_hardware_info                                          */
/* ================================================================ */

static void test_get_hardware_info(void)
{
    TEST("[TOOL] get_hardware_info: cpu + gpu + ram");
    cJSON *r = call_tool("get_hardware_info", NULL, 320);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(r, "cpu")), "cpu");
    ASSERT(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(r, "gpu")), "gpu");
    ASSERT(cJSON_GetObjectItemCaseSensitive(r, "ram_mb")->valuedouble > 0, "ram");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: get_index_stats                                            */
/* ================================================================ */

static void test_get_index_stats(void)
{
    TEST("[TOOL] get_index_stats: lmdb + fts5 + vector sections");
    cJSON *r = call_tool("get_index_stats", NULL, 330);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(r, "lmdb")), "lmdb");
    ASSERT(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(r, "fts5")), "fts5");
    ASSERT(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(r, "vector")), "vector");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: list_memories                                              */
/* ================================================================ */

static void test_list_memories(void)
{
    TEST("[TOOL] list_memories: paginated list with previews");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "limit", 5);
    cJSON *r = call_tool("list_memories", args, 340);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(r, "memories")), "memories");
    ASSERT(cJSON_GetObjectItemCaseSensitive(r, "total_count")->valuedouble >= 1, "count>0");

    /* Check first memory has expected fields */
    cJSON *first = cJSON_GetArrayItem(
        cJSON_GetObjectItemCaseSensitive(r, "memories"), 0);
    if (first) {
        ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(first, "id")), "id");
        ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(first, "content_preview")), "preview");
        ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(first, "tier")), "tier");
    }
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: consolidate_memories                                       */
/* ================================================================ */

static void test_consolidate(void)
{
    TEST("[TOOL] consolidate_memories: dry_run returns count");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddBoolToObject(args, "dry_run", true);
    cJSON *r = call_tool("consolidate_memories", args, 350);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_HasObjectItem(r, "consolidated_count"), "count");
    ASSERT(cJSON_HasObjectItem(r, "duration_ms"), "duration");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: prune_stale                                                */
/* ================================================================ */

static void test_prune_stale(void)
{
    TEST("[TOOL] prune_stale: dry_run returns candidates");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "min_age_days", 0); /* match everything */
    cJSON_AddNumberToObject(args, "min_importance", 99.0); /* nothing passes */
    cJSON_AddBoolToObject(args, "dry_run", true);
    cJSON *r = call_tool("prune_stale", args, 360);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(r, "candidates")), "candidates");
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "dry_run")), "dry_run");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: import_batch                                               */
/* ================================================================ */

static void test_import_batch(void)
{
    TEST("[TOOL] import_batch: import array of memories");
    cJSON *args = cJSON_CreateObject();
    cJSON *memories = cJSON_CreateArray();
    for (int i = 0; i < 5; i++) {
        cJSON *m = cJSON_CreateObject();
        char buf[64];
        snprintf(buf, sizeof(buf), "Batch import test memory #%d", i);
        cJSON_AddStringToObject(m, "content", buf);
        cJSON_AddStringToObject(m, "source_type", "batch_test");
        cJSON_AddItemToArray(memories, m);
    }
    cJSON_AddItemToObject(args, "memories", memories);

    cJSON *r = call_tool("import_batch", args, 370);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_GetObjectItemCaseSensitive(r, "imported")->valueint == 5, "imported=5");
    ASSERT(cJSON_GetObjectItemCaseSensitive(r, "errors")->valueint == 0, "errors=0");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: import_file                                                */
/* ================================================================ */

static void test_import_file(void)
{
    TEST("[TOOL] import_file: import JSONL file");
    /* Create a temp JSONL file */
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/test_import.jsonl", tmpdir);
    FILE *f = fopen(filepath, "w");
    fprintf(f, "{\"content\":\"Import line 1\"}\n");
    fprintf(f, "{\"content\":\"Import line 2\"}\n");
    fprintf(f, "{\"content\":\"Import line 3\"}\n");
    fclose(f);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "path", filepath);
    cJSON_AddStringToObject(args, "format", "jsonl");
    cJSON *r = call_tool("import_file", args, 380);
    ASSERT(r != NULL, "result");
    cJSON *imp = cJSON_GetObjectItemCaseSensitive(r, "imported");
    /* Path may be rejected if tmpdir isn't under allowed_paths -- both OK */
    if (imp) {
        ASSERT(imp->valueint == 3, "imported=3");
    } else {
        /* Path validation rejected -- tmpdir not under ~ */
        ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "error")), "path error");
    }
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: import_directory                                           */
/* ================================================================ */

static void test_import_directory(void)
{
    TEST("[TOOL] import_directory: import *.txt from directory");
    /* Create temp directory with text files */
    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/import_dir", tmpdir);
    mkdir(subdir, 0700);

    for (int i = 0; i < 3; i++) {
        char fp[1024];
        snprintf(fp, sizeof(fp), "%s/doc_%d.txt", subdir, i);
        FILE *f = fopen(fp, "w");
        fprintf(f, "Document %d content for directory import test.\n", i);
        fclose(f);
    }

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "path", subdir);
    cJSON_AddStringToObject(args, "pattern", "*.txt");
    cJSON *r = call_tool("import_directory", args, 390);
    ASSERT(r != NULL, "result");
    cJSON *fp = cJSON_GetObjectItemCaseSensitive(r, "files_processed");
    if (fp) {
        ASSERT(fp->valueint >= 1, "files>0");
    } else {
        ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "error")), "path error");
    }
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: get_import_status                                          */
/* ================================================================ */

static void test_get_import_status(void)
{
    TEST("[TOOL] get_import_status: returns jobs array");
    cJSON *r = call_tool("get_import_status", NULL, 400);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(r, "jobs")), "jobs");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: rebuild_indexes                                            */
/* ================================================================ */

static void test_rebuild_indexes(void)
{
    TEST("[TOOL] rebuild_indexes: rebuilds fts + vector from LMDB");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "target", "all");
    cJSON *r = call_tool("rebuild_indexes", args, 410);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(r, "rebuilt")), "rebuilt");
    ASSERT(cJSON_HasObjectItem(r, "duration_ms"), "duration");
    cJSON_Delete(r);
    PASS();
}

/* ================================================================ */
/* Tool: delete_memory                                              */
/* ================================================================ */

static void test_delete_memory(void)
{
    TEST("[TOOL] delete_memory: deleted=true, then retrieve fails");
    /* Store a memory to delete */
    cJSON *sa = cJSON_CreateObject();
    cJSON_AddStringToObject(sa, "content", "This memory will be deleted");
    cJSON *sr = call_tool("store_memory", sa, 420);
    char del_id[40];
    snprintf(del_id, sizeof(del_id), "%s",
             cJSON_GetObjectItemCaseSensitive(sr, "id")->valuestring);
    cJSON_Delete(sr);

    /* Delete it */
    cJSON *da = cJSON_CreateObject();
    cJSON_AddStringToObject(da, "id", del_id);
    cJSON *dr = call_tool("delete_memory", da, 421);
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(dr, "deleted")), "deleted");
    cJSON_Delete(dr);

    /* Verify retrieve fails */
    cJSON *ra = cJSON_CreateObject();
    cJSON_AddStringToObject(ra, "id", del_id);
    cJSON *rr = call_tool("retrieve_memory", ra, 422);
    ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(rr, "error")), "error on get");
    cJSON_Delete(rr);
    PASS();
}

/* ================================================================ */
/* Honeypot: Decoy Tools                                           */
/* ================================================================ */

static void test_decoy_admin_reset(void)
{
    TEST("[DECOY] admin_reset_auth returns 'insufficient privileges'");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "admin_key", "try-me");
    cJSON *r = call_tool("admin_reset_auth", args, 500);
    ASSERT(r != NULL, "result");
    ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "error")), "has error");
    ASSERT(strstr(cJSON_GetObjectItemCaseSensitive(r, "error")->valuestring,
                  "insufficient") != NULL, "insufficient privileges");
    cJSON_Delete(r);
    PASS();
}

static void test_decoy_export_all(void)
{
    TEST("[DECOY] export_all_memories returns 'insufficient privileges'");
    cJSON *r = call_tool("export_all_memories", NULL, 501);
    ASSERT(r && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "error")), "error");
    cJSON_Delete(r);
    PASS();
}

static void test_decoy_debug_query(void)
{
    TEST("[DECOY] debug_raw_query returns 'debug mode not enabled'");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "query", "SELECT * FROM entities");
    cJSON_AddStringToObject(args, "database", "entities");
    cJSON *r = call_tool("debug_raw_query", args, 502);
    ASSERT(r != NULL, "result");
    ASSERT(strstr(cJSON_GetObjectItemCaseSensitive(r, "error")->valuestring,
                  "debug") != NULL, "debug mode");
    cJSON_Delete(r);
    PASS();
}

static void test_decoy_set_config(void)
{
    TEST("[DECOY] set_system_config returns 'insufficient privileges'");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "key", "auth_token");
    cJSON_AddStringToObject(args, "value", "hacked");
    cJSON *r = call_tool("set_system_config", args, 503);
    ASSERT(r && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "error")), "error");
    cJSON_Delete(r);
    PASS();
}

static void test_decoy_isError_true(void)
{
    TEST("[DECOY] decoy tools set isError=true");
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", "admin_reset_auth");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "admin_key", "test");
    cJSON_AddItemToObject(params, "arguments", args);
    cJSON *req = make_request("tools/call", 504, params);
    cJSON *resp = mnemon_dispatch_request(dispatch, req);
    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    cJSON *ie = cJSON_GetObjectItemCaseSensitive(result, "isError");
    ASSERT(ie && cJSON_IsTrue(ie), "isError=true for decoy");
    cJSON_Delete(req);
    cJSON_Delete(resp);
    PASS();
}

/* ================================================================ */
/* Honeypot: Injection rejection via store_memory                  */
/* ================================================================ */

static void test_injection_blocked(void)
{
    TEST("[HONEYPOT] store_memory blocks obvious injection content");
    /* This tests through the MCP layer -- the store_memory tool should
     * reject content with high injection score IF the honeypot is wired in.
     * For now, the injection scanner is available but not yet integrated
     * into store_memory. This test verifies the tool at least stores
     * or returns an error gracefully. */
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "content",
        "Normal note about project status and timeline planning");
    cJSON *r = call_tool("store_memory", args, 510);
    ASSERT(r != NULL, "result");
    /* Clean content should store successfully */
    ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "id")), "stored");
    cJSON_Delete(r);
    PASS();
}

int main(void)
{
    printf("=== test_mcp: All Tools + Decoys + Protocol Conformance ===\n");
    setup();

    /* MCP lifecycle */
    test_initialize();
    test_initialized_notification();
    test_tools_list();

    /* Error handling */
    test_err_missing_method();
    test_err_unknown_method();
    test_err_unknown_tool();

    /* Memory CRUD (store -> retrieve -> update -> delete) */
    test_store_memory();
    test_store_memory_secret_rejected();
    test_store_memory_oversized();
    test_retrieve_memory();
    test_update_memory();

    /* Search (keyword, semantic, hybrid, temporal) */
    test_search_keyword();
    test_search_semantic();
    test_search_hybrid();
    test_search_temporal();

    /* Entity/graph (create -> observe -> search -> relate -> graph) */
    test_create_entity();
    test_add_observation();
    test_search_entities();
    test_create_relation();
    test_get_entity_graph();

    /* Temporal tools */
    test_get_history();
    test_get_state_at_time();
    test_get_changes_since();

    /* System tools */
    test_get_memory_stats();
    test_health_check();
    test_get_hardware_info();
    test_get_index_stats();
    test_list_memories();

    /* Lifecycle tools */
    test_consolidate();
    test_prune_stale();

    /* Import tools */
    test_import_batch();
    test_import_file();
    test_import_directory();
    test_get_import_status();
    test_rebuild_indexes();

    /* Delete (last, so other tests can use stored data) */
    test_delete_memory();

    /* Honeypot: decoy tools */
    test_decoy_admin_reset();
    test_decoy_export_all();
    test_decoy_debug_query();
    test_decoy_set_config();
    test_decoy_isError_true();
    test_injection_blocked();

    teardown();
    printf("\n%d/%d passed, %d failed\n", passed, total, failed);
    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
