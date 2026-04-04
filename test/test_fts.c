/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * test_fts.c -- SQLite FTS5 full-text search unit tests
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fts.h"
#include "id.h"
#include "mnemon.h"

static int passed = 0, failed = 0, total = 0;
#define TEST(n) do { total++; printf("  %-55s ", n); } while(0)
#define PASS() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(m) do { failed++; printf("FAIL: %s\n", m); return; } while(0)
#define ASSERT(c, m) do { if (!(c)) FAIL(m); } while(0)

static char dbpath[256];

static void setup(void)
{
    snprintf(dbpath, sizeof(dbpath), "/tmp/mnemon_test_fts_%d.db", getpid());
}

static void teardown(void)
{
    unlink(dbpath);
    char wal[270], shm[270];
    snprintf(wal, sizeof(wal), "%s-wal", dbpath);
    snprintf(shm, sizeof(shm), "%s-shm", dbpath);
    unlink(wal);
    unlink(shm);
}

static void test_open_close(void)
{
    TEST("open/close FTS5 database");
    mnemon_fts_t *f;
    ASSERT(mnemon_fts_open(&f, dbpath) == MNEMON_OK, "open");
    mnemon_fts_close(f);
    PASS();
}

static void test_index_memory_and_search(void)
{
    TEST("index memory then search by keyword");
    mnemon_fts_t *f;
    mnemon_fts_open(&f, dbpath);

    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);
    m.content = strdup("LMDB provides lightning-fast memory-mapped I/O");
    m.source_type = strdup("test");

    ASSERT(mnemon_fts_index_memory(f, &m) == MNEMON_OK, "index");

    mnemon_fts_results_t results = {0};
    ASSERT(mnemon_fts_search(f, "lightning fast", 10, &results) == MNEMON_OK, "search");
    ASSERT(results.count >= 1, "found result");
    ASSERT(results.results[0].source_type == 0, "source_type = memory");
    ASSERT(memcmp(results.results[0].id, m.id, 16) == 0, "correct id");

    mnemon_fts_results_free(&results);
    mnemon_memory_free(&m);
    mnemon_fts_close(f);
    PASS();
}

static void test_index_entity_search(void)
{
    TEST("index entity then search by name");
    mnemon_fts_t *f;
    mnemon_fts_open(&f, dbpath);

    mnemon_entity_t e = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(e.id, u.bytes, 16);
    e.name = strdup("AlphaProject");
    e.entity_type = strdup("project");

    ASSERT(mnemon_fts_index_entity(f, &e) == MNEMON_OK, "index");

    mnemon_fts_results_t results = {0};
    mnemon_fts_search(f, "AlphaProject", 10, &results);
    ASSERT(results.count >= 1, "found entity");
    ASSERT(results.results[0].source_type == 1, "source_type = entity");

    mnemon_fts_results_free(&results);
    mnemon_entity_free(&e);
    mnemon_fts_close(f);
    PASS();
}

static void test_remove_memory(void)
{
    TEST("remove memory from FTS5 index");
    mnemon_fts_t *f;
    mnemon_fts_open(&f, dbpath);

    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);
    m.content = strdup("unique_removal_test_keyword");
    m.source_type = strdup("test");
    mnemon_fts_index_memory(f, &m);

    /* Verify it exists */
    mnemon_fts_results_t r1 = {0};
    mnemon_fts_search(f, "unique_removal_test_keyword", 10, &r1);
    ASSERT(r1.count >= 1, "found before remove");
    mnemon_fts_results_free(&r1);

    /* Remove */
    ASSERT(mnemon_fts_remove(f, m.id, 0) == MNEMON_OK, "remove");

    /* Verify gone */
    mnemon_fts_results_t r2 = {0};
    mnemon_fts_search(f, "unique_removal_test_keyword", 10, &r2);
    ASSERT(r2.count == 0, "gone after remove");
    mnemon_fts_results_free(&r2);

    mnemon_memory_free(&m);
    mnemon_fts_close(f);
    PASS();
}

static void test_count(void)
{
    TEST("count indexed documents");
    mnemon_fts_t *f;
    mnemon_fts_open(&f, dbpath);
    size_t count;
    ASSERT(mnemon_fts_count(f, &count) == MNEMON_OK, "count");
    printf("[count=%zu] ", count);
    mnemon_fts_close(f);
    PASS();
}

static void test_checkpoint(void)
{
    TEST("WAL checkpoint does not error");
    mnemon_fts_t *f;
    mnemon_fts_open(&f, dbpath);
    ASSERT(mnemon_fts_checkpoint(f) == MNEMON_OK, "checkpoint");
    mnemon_fts_close(f);
    PASS();
}

static void test_empty_query(void)
{
    TEST("search with empty query");
    mnemon_fts_t *f;
    mnemon_fts_open(&f, dbpath);
    mnemon_fts_results_t r = {0};
    /* Empty query should not crash */
    mnemon_fts_search(f, "", 10, &r);
    mnemon_fts_results_free(&r);
    mnemon_fts_close(f);
    PASS();
}

static void test_special_chars_query(void)
{
    TEST("search with special characters in query");
    mnemon_fts_t *f;
    mnemon_fts_open(&f, dbpath);
    mnemon_fts_results_t r = {0};
    /* FTS5 special chars should be sanitized */
    mnemon_fts_search(f, "hello AND world OR \"test\"", 10, &r);
    mnemon_fts_results_free(&r);
    mnemon_fts_close(f);
    PASS();
}

static void test_update_memory(void)
{
    TEST("update_memory re-indexes with new content");
    mnemon_fts_t *f;
    mnemon_fts_open(&f, dbpath);

    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);
    m.content = strdup("original_fts_update_test_content");
    m.source_type = strdup("test");
    mnemon_fts_index_memory(f, &m);

    /* Update content */
    free(m.content);
    m.content = strdup("modified_fts_update_new_content");
    ASSERT(mnemon_fts_update_memory(f, &m) == MNEMON_OK, "update");

    /* Search for new content */
    mnemon_fts_results_t r = {0};
    mnemon_fts_search(f, "modified_fts_update_new_content", 10, &r);
    ASSERT(r.count >= 1, "found updated");
    mnemon_fts_results_free(&r);

    /* Old content should not be found */
    mnemon_fts_results_t r2 = {0};
    mnemon_fts_search(f, "original_fts_update_test_content", 10, &r2);
    ASSERT(r2.count == 0, "old content gone");
    mnemon_fts_results_free(&r2);

    mnemon_memory_free(&m);
    mnemon_fts_close(f);
    PASS();
}

static void test_update_entity(void)
{
    TEST("update_entity re-indexes with new name");
    mnemon_fts_t *f;
    mnemon_fts_open(&f, dbpath);

    mnemon_entity_t e = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(e.id, u.bytes, 16);
    e.name = strdup("OriginalEntityName7391");
    e.entity_type = strdup("test");
    mnemon_fts_index_entity(f, &e);

    free(e.name);
    e.name = strdup("UpdatedEntityName7391");
    ASSERT(mnemon_fts_update_entity(f, &e) == MNEMON_OK, "update");

    mnemon_fts_results_t r = {0};
    mnemon_fts_search(f, "UpdatedEntityName7391", 10, &r);
    ASSERT(r.count >= 1, "found updated entity");
    mnemon_fts_results_free(&r);

    mnemon_entity_free(&e);
    mnemon_fts_close(f);
    PASS();
}

static void test_clear(void)
{
    TEST("clear drops all indexed documents");
    mnemon_fts_t *f;
    mnemon_fts_open(&f, dbpath);
    mnemon_fts_clear(f);
    size_t count;
    mnemon_fts_count(f, &count);
    ASSERT(count == 0, "count=0 after clear");
    mnemon_fts_close(f);
    PASS();
}

int main(void)
{
    printf("=== test_fts: SQLite FTS5 Full-Text Search ===\n");
    setup();

    test_open_close();
    test_index_memory_and_search();
    test_index_entity_search();
    test_remove_memory();
    test_count();
    test_checkpoint();
    test_empty_query();
    test_special_chars_query();
    test_update_memory();
    test_update_entity();
    test_clear();

    teardown();
    printf("\n%d/%d passed, %d failed\n", passed, total, failed);
    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
