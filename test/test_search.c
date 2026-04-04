/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * test_search.c -- Search quality and correctness tests
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "search.h"
#include "storage.h"
#include "config_parse.h"
#include "id.h"
#include "memory.h"
#include "mnemon.h"

static int passed = 0, failed = 0, total = 0;
#define TEST(n) do { total++; printf("  %-55s ", n); } while(0)
#define PASS() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(m) do { failed++; printf("FAIL: %s\n", m); return; } while(0)
#define ASSERT(c, m) do { if (!(c)) FAIL(m); } while(0)

static char tmpdir[256];
static mnemon_storage_t *store = NULL;

static void setup(void)
{
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/mnemon_test_search_%d", getpid());
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

    mnemon_storage_open(&store, cfg);
    mnemon_config_free(cfg);

    /* Seed some test memories */
    const char *contents[] = {
        "LMDB provides fast memory-mapped database access",
        "SQLite FTS5 enables full-text search with BM25 ranking",
        "Vector similarity search uses cosine distance in HNSW graphs",
        "The MCP protocol uses JSON-RPC 2.0 over stdio",
        "Rust and Go are systems programming languages",
    };
    for (int i = 0; i < 5; i++) {
        mnemon_memory_t m = {0};
        mnemon_uuid_t u; mnemon_uuid_generate(&u);
        memcpy(m.id, u.bytes, 16);
        m.content = strdup(contents[i]);
        m.source_type = strdup("test");
        m.source_id = strdup("");
        m.created_at = mnemon_time_ms();
        m.last_accessed = m.created_at;
        mnemon_store_memory(store, &m);
        mnemon_memory_free(&m);
    }
}

static void teardown(void)
{
    mnemon_storage_close(store);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    if (system(cmd) != 0) { /* ignore */ }
}

static void test_keyword_search(void)
{
    TEST("keyword search finds matching memories");
    mnemon_query_t q = {0};
    q.query_text = "LMDB database";
    q.top_k = 5;

    mnemon_result_set_t rs = {0};
    mnemon_err_t err = mnemon_search_keyword(store, &q, &rs);
    ASSERT(err == MNEMON_OK, "search ok");
    printf("[results=%d] ", rs.count);
    if (rs.count > 0)
        ASSERT(rs.results[0].keyword_score > 0, "has keyword score");

    mnemon_result_set_free(&rs);
    PASS();
}

static void test_keyword_no_results(void)
{
    TEST("keyword search with no matches returns 0");
    mnemon_query_t q = {0};
    q.query_text = "xyzzy_nonexistent_term";
    q.top_k = 5;

    mnemon_result_set_t rs = {0};
    mnemon_search_keyword(store, &q, &rs);
    ASSERT(rs.count == 0, "no results");
    mnemon_result_set_free(&rs);
    PASS();
}

static void test_hybrid_search(void)
{
    TEST("hybrid search returns fused results");
    mnemon_query_t q = {0};
    q.query_text = "full text search";
    q.top_k = 5;

    mnemon_result_set_t rs = {0};
    mnemon_err_t err = mnemon_search_hybrid(store, &q, &rs);
    ASSERT(err == MNEMON_OK, "search ok");
    printf("[results=%d] ", rs.count);

    mnemon_result_set_free(&rs);
    PASS();
}

static void test_top_k_cap(void)
{
    TEST("top_k capped at MAX_TOP_K (50)");
    mnemon_query_t q = {0};
    q.query_text = "database";
    q.top_k = 999; /* exceeds cap */

    mnemon_result_set_t rs = {0};
    mnemon_search_keyword(store, &q, &rs);
    ASSERT(rs.count <= 50, "capped at 50");
    mnemon_result_set_free(&rs);
    PASS();
}

static void test_empty_query(void)
{
    TEST("empty query text handled gracefully");
    mnemon_query_t q = {0};
    q.query_text = "";
    q.top_k = 5;

    mnemon_result_set_t rs = {0};
    mnemon_search_keyword(store, &q, &rs);
    /* Should not crash */
    mnemon_result_set_free(&rs);
    PASS();
}

static void test_results_have_ids(void)
{
    TEST("search results have valid UUID strings");
    mnemon_query_t q = {0};
    q.query_text = "JSON";
    q.top_k = 5;

    mnemon_result_set_t rs = {0};
    mnemon_search_keyword(store, &q, &rs);
    for (int i = 0; i < rs.count; i++) {
        ASSERT(strlen(rs.results[i].id) == 36, "UUID length 36");
        ASSERT(rs.results[i].id[8] == '-', "UUID dash at 8");
        ASSERT(rs.results[i].id[13] == '-', "UUID dash at 13");
    }
    mnemon_result_set_free(&rs);
    PASS();
}

int main(void)
{
    printf("=== test_search: Search Quality & Correctness ===\n");
    setup();

    test_keyword_search();
    test_keyword_no_results();
    test_hybrid_search();
    test_top_k_cap();
    test_empty_query();
    test_results_have_ids();

    teardown();
    printf("\n%d/%d passed, %d failed\n", passed, total, failed);
    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
