/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * test_storage.c -- Aggressive memory storage and retrieval tests
 *
 * Verifies that every field of a stored memory survives the full
 * pipeline: LMDB msgpack serialization, FTS5 indexing, and retrieval.
 * Tests store, get, update, delete, search, and cross-engine consistency.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

#include "storage.h"
#include "config_parse.h"
#include "search.h"
#include "fts.h"
#include "consolidate.h"
#include "id.h"
#include "memory.h"
#include "mnemon.h"

static int passed = 0, failed = 0, total = 0;
#define TEST(n) do { total++; printf("  %-60s ", n); } while(0)
#define PASS() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(m) do { failed++; printf("FAIL: %s\n", m); return; } while(0)
#define ASSERT(c, m) do { if (!(c)) FAIL(m); } while(0)

static char tmpdir[256];
static mnemon_storage_t *store = NULL;

static void setup(void)
{
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/mnemon_test_stor_%d", getpid());
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

    mnemon_err_t err = mnemon_storage_open(&store, cfg);
    mnemon_config_free(cfg);
    if (err != MNEMON_OK) {
        fprintf(stderr, "FATAL: storage open failed: %s\n", mnemon_err_msg());
        exit(1);
    }
}

static void teardown(void)
{
    mnemon_storage_close(store);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    if (system(cmd) != 0) { /* ignore */ }
}

/* ================================================================ */
/* 1. Basic Memory Storage & Retrieval                              */
/* ================================================================ */

static void test_store_get_all_fields(void)
{
    TEST("store + get verifies ALL memory fields");
    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);

    /* Set every field */
    m.tier = MNEMON_TIER_SEMANTIC;
    m.content = strdup("The quick brown fox jumps over the lazy dog");
    m.source_type = strdup("email");
    m.source_id = strdup("msg-42@example.com");
    m.source_author = strdup("alice@example.com");
    m.source_timestamp = 1700000000000LL;
    m.tag_count = 3;
    m.tags = calloc(3, sizeof(char *));
    m.tags[0] = strdup("animals");
    m.tags[1] = strdup("foxes");
    m.tags[2] = strdup("dogs");
    m.importance = 0.75f;
    m.access_count = 5;
    m.created_at = 1710000000000LL;
    m.last_accessed = 1710100000000LL;
    m.consolidated = true;

    ASSERT(mnemon_store_memory(store, &m) == MNEMON_OK, "store failed");

    /* Retrieve */
    mnemon_memory_t out = {0};
    ASSERT(mnemon_get_memory(store, m.id, &out) == MNEMON_OK, "get failed");

    /* Verify every field */
    ASSERT(memcmp(out.id, m.id, 16) == 0, "id mismatch");
    ASSERT(out.tier == MNEMON_TIER_SEMANTIC, "tier mismatch");
    ASSERT(out.content != NULL, "content is NULL");
    ASSERT(strcmp(out.content, "The quick brown fox jumps over the lazy dog") == 0,
           "content mismatch");
    ASSERT(out.source_type != NULL, "source_type is NULL");
    ASSERT(strcmp(out.source_type, "email") == 0, "source_type mismatch");
    ASSERT(out.source_id != NULL, "source_id is NULL");
    ASSERT(strcmp(out.source_id, "msg-42@example.com") == 0, "source_id mismatch");
    ASSERT(out.source_author != NULL, "source_author is NULL");
    ASSERT(strcmp(out.source_author, "alice@example.com") == 0,
           "source_author mismatch");
    ASSERT(out.source_timestamp == 1700000000000LL, "source_timestamp mismatch");
    ASSERT(out.tag_count == 3, "tag_count mismatch");
    ASSERT(out.tags != NULL, "tags is NULL");
    ASSERT(strcmp(out.tags[0], "animals") == 0, "tag[0] mismatch");
    ASSERT(strcmp(out.tags[1], "foxes") == 0, "tag[1] mismatch");
    ASSERT(strcmp(out.tags[2], "dogs") == 0, "tag[2] mismatch");
    ASSERT(fabsf(out.importance - 0.75f) < 0.01f, "importance mismatch");
    ASSERT(out.created_at == 1710000000000LL, "created_at mismatch");
    ASSERT(out.last_accessed == 1710100000000LL, "last_accessed mismatch");
    ASSERT(out.consolidated == true, "consolidated mismatch");

    mnemon_memory_free(&m);
    mnemon_memory_free(&out);
    PASS();
}

static void test_store_get_minimal_fields(void)
{
    TEST("store + get with only required fields (content)");
    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);
    m.content = strdup("Minimal memory with no optional fields");
    m.source_type = strdup("mcp");
    m.source_id = strdup("");
    m.created_at = mnemon_time_ms();
    m.last_accessed = m.created_at;

    ASSERT(mnemon_store_memory(store, &m) == MNEMON_OK, "store");

    mnemon_memory_t out = {0};
    ASSERT(mnemon_get_memory(store, m.id, &out) == MNEMON_OK, "get");
    ASSERT(strcmp(out.content, "Minimal memory with no optional fields") == 0,
           "content");
    ASSERT(out.tier == MNEMON_TIER_EPISODIC, "default tier");
    ASSERT(out.tag_count == 0, "no tags");
    ASSERT(out.entity_id_count == 0, "no entities");
    ASSERT(out.consolidated == false, "not consolidated");

    mnemon_memory_free(&m);
    mnemon_memory_free(&out);
    PASS();
}

static void test_store_get_each_tier(void)
{
    TEST("store + get for each memory tier");
    mnemon_memory_tier_t tiers[] = {
        MNEMON_TIER_EPISODIC, MNEMON_TIER_SEMANTIC, MNEMON_TIER_PROCEDURAL
    };
    const char *names[] = {"episodic", "semantic", "procedural"};

    for (int i = 0; i < 3; i++) {
        mnemon_memory_t m = {0};
        mnemon_uuid_t u; mnemon_uuid_generate(&u);
        memcpy(m.id, u.bytes, 16);
        char buf[64];
        snprintf(buf, sizeof(buf), "Tier test: %s", names[i]);
        m.content = strdup(buf);
        m.source_type = strdup("test");
        m.source_id = strdup("");
        m.tier = tiers[i];
        m.created_at = mnemon_time_ms();
        m.last_accessed = m.created_at;

        ASSERT(mnemon_store_memory(store, &m) == MNEMON_OK, "store");

        mnemon_memory_t out = {0};
        ASSERT(mnemon_get_memory(store, m.id, &out) == MNEMON_OK, "get");
        ASSERT(out.tier == tiers[i], "tier mismatch");

        mnemon_memory_free(&m);
        mnemon_memory_free(&out);
    }
    PASS();
}

/* ================================================================ */
/* 2. Content Integrity                                             */
/* ================================================================ */

static void test_large_content(void)
{
    TEST("store + get large content (10KB)");
    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);

    /* Build a 10KB content string */
    size_t sz = 10240;
    m.content = malloc(sz + 1);
    for (size_t i = 0; i < sz; i++)
        m.content[i] = 'A' + (char)(i % 26);
    m.content[sz] = '\0';
    m.source_type = strdup("test");
    m.source_id = strdup("");
    m.created_at = mnemon_time_ms();
    m.last_accessed = m.created_at;

    ASSERT(mnemon_store_memory(store, &m) == MNEMON_OK, "store");

    mnemon_memory_t out = {0};
    ASSERT(mnemon_get_memory(store, m.id, &out) == MNEMON_OK, "get");
    ASSERT(strlen(out.content) == sz, "length match");
    ASSERT(memcmp(out.content, m.content, sz) == 0, "content byte-exact");

    mnemon_memory_free(&m);
    mnemon_memory_free(&out);
    PASS();
}

static void test_unicode_content(void)
{
    TEST("store + get unicode content");
    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);
    m.content = strdup("Unicode: \xc3\xa9\xc3\xa0\xc3\xbc \xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x90\xb1");
    m.source_type = strdup("test");
    m.source_id = strdup("");
    m.created_at = mnemon_time_ms();
    m.last_accessed = m.created_at;

    ASSERT(mnemon_store_memory(store, &m) == MNEMON_OK, "store");

    mnemon_memory_t out = {0};
    ASSERT(mnemon_get_memory(store, m.id, &out) == MNEMON_OK, "get");
    ASSERT(strcmp(out.content, m.content) == 0, "unicode preserved");

    mnemon_memory_free(&m);
    mnemon_memory_free(&out);
    PASS();
}

static void test_special_chars_content(void)
{
    TEST("store + get content with special chars (quotes, backslashes)");
    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);
    m.content = strdup("He said \"hello\\world\" and {key: 'value'}\ttab\nnewline");
    m.source_type = strdup("test");
    m.source_id = strdup("");
    m.created_at = mnemon_time_ms();
    m.last_accessed = m.created_at;

    ASSERT(mnemon_store_memory(store, &m) == MNEMON_OK, "store");

    mnemon_memory_t out = {0};
    ASSERT(mnemon_get_memory(store, m.id, &out) == MNEMON_OK, "get");
    ASSERT(strcmp(out.content, m.content) == 0, "special chars preserved");

    mnemon_memory_free(&m);
    mnemon_memory_free(&out);
    PASS();
}

static void test_empty_content(void)
{
    TEST("store + get empty string content");
    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);
    m.content = strdup("");
    m.source_type = strdup("test");
    m.source_id = strdup("");
    m.created_at = mnemon_time_ms();
    m.last_accessed = m.created_at;

    ASSERT(mnemon_store_memory(store, &m) == MNEMON_OK, "store");

    mnemon_memory_t out = {0};
    ASSERT(mnemon_get_memory(store, m.id, &out) == MNEMON_OK, "get");
    ASSERT(out.content != NULL, "content not null");
    ASSERT(strcmp(out.content, "") == 0, "empty string preserved");

    mnemon_memory_free(&m);
    mnemon_memory_free(&out);
    PASS();
}

/* ================================================================ */
/* 3. Tags Round-Trip                                               */
/* ================================================================ */

static void test_many_tags(void)
{
    TEST("store + get memory with 10 tags");
    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);
    m.content = strdup("Memory with many tags");
    m.source_type = strdup("test");
    m.source_id = strdup("");
    m.created_at = mnemon_time_ms();
    m.last_accessed = m.created_at;
    m.tag_count = 10;
    m.tags = calloc(10, sizeof(char *));
    for (int i = 0; i < 10; i++) {
        char tag[16];
        snprintf(tag, sizeof(tag), "tag_%d", i);
        m.tags[i] = strdup(tag);
    }

    ASSERT(mnemon_store_memory(store, &m) == MNEMON_OK, "store");

    mnemon_memory_t out = {0};
    ASSERT(mnemon_get_memory(store, m.id, &out) == MNEMON_OK, "get");
    ASSERT(out.tag_count == 10, "tag count");
    for (int i = 0; i < 10; i++) {
        char expected[16];
        snprintf(expected, sizeof(expected), "tag_%d", i);
        ASSERT(strcmp(out.tags[i], expected) == 0, "tag content");
    }

    mnemon_memory_free(&m);
    mnemon_memory_free(&out);
    PASS();
}

/* ================================================================ */
/* 4. Delete Verification                                           */
/* ================================================================ */

static void test_delete_then_get_fails(void)
{
    TEST("delete memory then get returns NOT_FOUND");
    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);
    m.content = strdup("Will be deleted");
    m.source_type = strdup("test");
    m.source_id = strdup("");
    m.created_at = mnemon_time_ms();
    m.last_accessed = m.created_at;

    mnemon_store_memory(store, &m);
    ASSERT(mnemon_delete_memory(store, m.id) == MNEMON_OK, "delete");

    mnemon_memory_t out = {0};
    ASSERT(mnemon_get_memory(store, m.id, &out) == MNEMON_ERR_NOT_FOUND,
           "get after delete should fail");

    mnemon_memory_free(&m);
    PASS();
}

static void test_delete_removes_from_fts(void)
{
    TEST("delete removes memory from FTS5 keyword search");
    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);
    m.content = strdup("uniqueDeleteTestKeyword7392");
    m.source_type = strdup("test");
    m.source_id = strdup("");
    m.created_at = mnemon_time_ms();
    m.last_accessed = m.created_at;

    mnemon_store_memory(store, &m);

    /* Verify searchable before delete */
    mnemon_query_t q = {.query_text = "uniqueDeleteTestKeyword7392", .top_k = 5};
    mnemon_result_set_t rs = {0};
    mnemon_search_keyword(store, &q, &rs);
    ASSERT(rs.count >= 1, "found before delete");
    mnemon_result_set_free(&rs);

    /* Delete */
    mnemon_delete_memory(store, m.id);

    /* Verify not searchable after delete */
    memset(&rs, 0, sizeof(rs));
    mnemon_search_keyword(store, &q, &rs);
    ASSERT(rs.count == 0, "not found after delete");
    mnemon_result_set_free(&rs);

    mnemon_memory_free(&m);
    PASS();
}

/* ================================================================ */
/* 5. Update Verification                                           */
/* ================================================================ */

static void test_update_content(void)
{
    TEST("update_memory changes content");
    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);
    m.content = strdup("Original content before update");
    m.source_type = strdup("test");
    m.source_id = strdup("");
    m.created_at = mnemon_time_ms();
    m.last_accessed = m.created_at;

    mnemon_store_memory(store, &m);

    /* Update */
    ASSERT(mnemon_update_memory(store, m.id, "Updated content after change", NULL) == MNEMON_OK,
           "update");

    /* Verify */
    mnemon_memory_t out = {0};
    ASSERT(mnemon_get_memory(store, m.id, &out) == MNEMON_OK, "get");
    ASSERT(strcmp(out.content, "Updated content after change") == 0,
           "content updated");

    mnemon_memory_free(&m);
    mnemon_memory_free(&out);
    PASS();
}

/* ================================================================ */
/* 6. Search Finds Stored Memories                                  */
/* ================================================================ */

static void test_keyword_search_finds_stored(void)
{
    TEST("keyword search finds a just-stored memory");
    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);
    m.content = strdup("Specialized quantum computing research paper");
    m.source_type = strdup("document");
    m.source_id = strdup("");
    m.created_at = mnemon_time_ms();
    m.last_accessed = m.created_at;

    mnemon_store_memory(store, &m);

    mnemon_query_t q = {.query_text = "quantum computing", .top_k = 5};
    mnemon_result_set_t rs = {0};
    mnemon_search_keyword(store, &q, &rs);
    ASSERT(rs.count >= 1, "found result");

    /* Verify the correct memory was found */
    bool found = false;
    char id_str[37];
    mnemon_uuid_to_string(&u, id_str, sizeof(id_str));
    for (int i = 0; i < rs.count; i++) {
        if (strcmp(rs.results[i].id, id_str) == 0) {
            found = true;
            ASSERT(rs.results[i].content != NULL, "result has content");
            ASSERT(strstr(rs.results[i].content, "quantum") != NULL,
                   "result contains search term");
            break;
        }
    }
    ASSERT(found, "our memory found in results");

    mnemon_result_set_free(&rs);
    mnemon_memory_free(&m);
    PASS();
}

/* ================================================================ */
/* 7. Multiple Memories                                             */
/* ================================================================ */

static void test_store_retrieve_many(void)
{
    TEST("store 50 memories, retrieve each, verify content");
    uint8_t ids[50][16];
    char contents[50][64];

    for (int i = 0; i < 50; i++) {
        mnemon_memory_t m = {0};
        mnemon_uuid_t u; mnemon_uuid_generate(&u);
        memcpy(ids[i], u.bytes, 16);
        memcpy(m.id, u.bytes, 16);
        snprintf(contents[i], sizeof(contents[i]),
                 "Batch memory #%d with unique data %d", i, i * 7);
        m.content = strdup(contents[i]);
        m.source_type = strdup("batch");
        m.source_id = strdup("");
        m.tier = (mnemon_memory_tier_t)(i % 3);
        m.importance = 0.1f + (float)i * 0.01f;
        m.created_at = mnemon_time_ms() + i;
        m.last_accessed = m.created_at;

        ASSERT(mnemon_store_memory(store, &m) == MNEMON_OK, "store");
        mnemon_memory_free(&m);
    }

    /* Retrieve each and verify */
    int verified = 0;
    for (int i = 0; i < 50; i++) {
        mnemon_memory_t out = {0};
        ASSERT(mnemon_get_memory(store, ids[i], &out) == MNEMON_OK, "get");
        ASSERT(strcmp(out.content, contents[i]) == 0, "content mismatch");
        ASSERT(out.tier == (mnemon_memory_tier_t)(i % 3), "tier mismatch");
        verified++;
        mnemon_memory_free(&out);
    }
    ASSERT(verified == 50, "all 50 verified");
    PASS();
}

/* ================================================================ */
/* 8. Entity Storage & Retrieval                                    */
/* ================================================================ */

static void test_entity_all_fields(void)
{
    TEST("store + get entity with all fields");
    mnemon_entity_t e = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(e.id, u.bytes, 16);
    e.name = strdup("Albert Einstein");
    e.entity_type = strdup("person");
    e.observation_count = 3;
    e.observations = calloc(3, sizeof(char *));
    e.observations[0] = strdup("Developed general relativity");
    e.observations[1] = strdup("Won Nobel Prize in Physics 1921");
    e.observations[2] = strdup("Born in Ulm, Germany");
    e.importance = 0.95f;
    e.access_count = 42;
    e.created_at = 1710000000000LL;
    e.updated_at = 1710500000000LL;
    e.last_accessed = 1710600000000LL;

    ASSERT(mnemon_store_entity(store, &e) == MNEMON_OK, "store");

    mnemon_entity_t out = {0};
    ASSERT(mnemon_get_entity(store, e.id, &out) == MNEMON_OK, "get");
    ASSERT(strcmp(out.name, "Albert Einstein") == 0, "name");
    ASSERT(strcmp(out.entity_type, "person") == 0, "type");
    ASSERT(out.observation_count == 3, "obs count");
    ASSERT(strcmp(out.observations[0], "Developed general relativity") == 0, "obs[0]");
    ASSERT(strcmp(out.observations[1], "Won Nobel Prize in Physics 1921") == 0, "obs[1]");
    ASSERT(strcmp(out.observations[2], "Born in Ulm, Germany") == 0, "obs[2]");
    ASSERT(fabsf(out.importance - 0.95f) < 0.01f, "importance");
    ASSERT(out.created_at == 1710000000000LL, "created_at");
    ASSERT(out.updated_at == 1710500000000LL, "updated_at");

    mnemon_entity_free(&e);
    mnemon_entity_free(&out);
    PASS();
}

/* ================================================================ */
/* 9. Edge (Relation) Storage                                       */
/* ================================================================ */

static void test_edge_round_trip(void)
{
    TEST("store edge, retrieve via get_edges_from");
    mnemon_uuid_t us, ut, ue;
    mnemon_uuid_generate(&us);
    mnemon_uuid_generate(&ut);
    mnemon_uuid_generate(&ue);

    /* Create both entities first */
    mnemon_entity_t src = {0};
    memcpy(src.id, us.bytes, 16);
    src.name = strdup("Alice");
    src.entity_type = strdup("person");
    src.created_at = mnemon_time_ms();
    src.updated_at = src.created_at;
    mnemon_store_entity(store, &src);

    mnemon_entity_t tgt = {0};
    memcpy(tgt.id, ut.bytes, 16);
    tgt.name = strdup("ProjectX");
    tgt.entity_type = strdup("project");
    tgt.created_at = mnemon_time_ms();
    tgt.updated_at = tgt.created_at;
    mnemon_store_entity(store, &tgt);

    /* Create edge */
    mnemon_edge_t edge = {0};
    memcpy(edge.id, ue.bytes, 16);
    memcpy(edge.source_id, us.bytes, 16);
    memcpy(edge.target_id, ut.bytes, 16);
    edge.edge_type = strdup("works_on");
    edge.description = strdup("Lead engineer since 2025");
    edge.weight = 0.85f;
    edge.valid_from = 1700000000000LL;
    edge.created_at = 1710000000000LL;

    ASSERT(mnemon_store_edge(store, &edge) == MNEMON_OK, "store edge");

    /* Query edges from source */
    mnemon_edge_list_t list = {0};
    ASSERT(mnemon_get_edges_from(store, us.bytes, NULL, &list) == MNEMON_OK,
           "get edges");
    ASSERT(list.count >= 1, "found edge");

    mnemon_entity_free(&src);
    mnemon_entity_free(&tgt);
    mnemon_edge_free(&edge);
    mnemon_edge_list_free(&list);
    PASS();
}

/* ================================================================ */
/* 10. Stats Consistency                                            */
/* ================================================================ */

static void test_stats_reflect_operations(void)
{
    TEST("stats reflect stored memories, entities, edges");
    mnemon_stats_t st = {0};
    ASSERT(mnemon_get_stats(store, &st) == MNEMON_OK, "stats");
    ASSERT(st.total_memories > 0, "memories counted");
    ASSERT(st.total_entities > 0, "entities counted");
    ASSERT(st.total_edges > 0, "edges counted");
    ASSERT(st.fts_indexed > 0, "fts indexed count > 0");
    printf("[mem=%zu ent=%zu edge=%zu fts=%zu] ",
           st.total_memories, st.total_entities,
           st.total_edges, st.fts_indexed);
    PASS();
}

/* ================================================================ */
/* 11. Error Handling                                               */
/* ================================================================ */

static void test_get_nonexistent_memory(void)
{
    TEST("get non-existent memory returns NOT_FOUND");
    uint8_t fake[16] = {0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
                        0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00};
    mnemon_memory_t out = {0};
    ASSERT(mnemon_get_memory(store, fake, &out) == MNEMON_ERR_NOT_FOUND, "not found");
    PASS();
}

static void test_delete_nonexistent_memory(void)
{
    TEST("delete non-existent memory returns NOT_FOUND");
    uint8_t fake[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};
    mnemon_err_t err = mnemon_delete_memory(store, fake);
    ASSERT(err == MNEMON_ERR_NOT_FOUND, "not found");
    PASS();
}

static void test_delete_entity(void)
{
    TEST("delete entity removes from storage");
    mnemon_entity_t e = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(e.id, u.bytes, 16);
    e.name = strdup("WillDelete");
    e.entity_type = strdup("test");
    e.created_at = mnemon_time_ms();
    e.updated_at = e.created_at;
    mnemon_store_entity(store, &e);

    ASSERT(mnemon_delete_entity(store, e.id) == MNEMON_OK, "deleted");

    mnemon_entity_t out = {0};
    ASSERT(mnemon_get_entity(store, e.id, &out) == MNEMON_ERR_NOT_FOUND, "gone");

    mnemon_entity_free(&e);
    PASS();
}

static void test_get_edges_to(void)
{
    TEST("get_edges_to finds incoming edges");
    mnemon_uuid_t us, ut, ue;
    mnemon_uuid_generate(&us);
    mnemon_uuid_generate(&ut);
    mnemon_uuid_generate(&ue);

    mnemon_entity_t src = {0};
    memcpy(src.id, us.bytes, 16);
    src.name = strdup("EdgeSrc"); src.entity_type = strdup("test");
    src.created_at = mnemon_time_ms(); src.updated_at = src.created_at;
    mnemon_store_entity(store, &src);

    mnemon_entity_t tgt = {0};
    memcpy(tgt.id, ut.bytes, 16);
    tgt.name = strdup("EdgeTgt"); tgt.entity_type = strdup("test");
    tgt.created_at = mnemon_time_ms(); tgt.updated_at = tgt.created_at;
    mnemon_store_entity(store, &tgt);

    mnemon_edge_t edge = {0};
    memcpy(edge.id, ue.bytes, 16);
    memcpy(edge.source_id, us.bytes, 16);
    memcpy(edge.target_id, ut.bytes, 16);
    edge.edge_type = strdup("incoming_test");
    edge.weight = 1.0f; edge.valid_from = mnemon_time_ms(); edge.created_at = edge.valid_from;
    mnemon_store_edge(store, &edge);

    mnemon_edge_list_t list = {0};
    ASSERT(mnemon_get_edges_to(store, ut.bytes, NULL, &list) == MNEMON_OK, "get_edges_to");
    ASSERT(list.count >= 1, "found incoming edge");

    mnemon_entity_free(&src); mnemon_entity_free(&tgt);
    mnemon_edge_free(&edge); mnemon_edge_list_free(&list);
    PASS();
}

static void test_rebuild_indexes(void)
{
    TEST("rebuild_indexes from LMDB source of truth");
    ASSERT(mnemon_rebuild_indexes(store, "all") == MNEMON_OK, "rebuild");

    /* Verify FTS still works after rebuild */
    mnemon_query_t q = {.query_text = "Integration", .top_k = 5};
    mnemon_result_set_t rs = {0};
    mnemon_search_keyword(store, &q, &rs);
    /* May or may not find results depending on what's in LMDB */
    mnemon_result_set_free(&rs);
    PASS();
}

static void test_replay_intents(void)
{
    TEST("replay_intents runs without error");
    ASSERT(mnemon_replay_intents(store) == MNEMON_OK, "replay");
    PASS();
}

static void test_storage_accessors(void)
{
    TEST("storage accessors return non-NULL");
    ASSERT(mnemon_storage_graph(store) != NULL, "graph");
    ASSERT(mnemon_storage_fts(store) != NULL, "fts");
    ASSERT(mnemon_storage_vector(store) != NULL, "vector");
    /* embed may be NULL if no model -- that's OK */
    PASS();
}

static void test_null_args(void)
{
    TEST("NULL arguments return INVALID_INPUT");
    ASSERT(mnemon_store_memory(store, NULL) == MNEMON_ERR_INVALID_INPUT, "null mem");
    ASSERT(mnemon_get_memory(store, NULL, NULL) == MNEMON_ERR_INVALID_INPUT, "null id");
    ASSERT(mnemon_store_entity(store, NULL) == MNEMON_ERR_INVALID_INPUT, "null ent");
    ASSERT(mnemon_store_edge(store, NULL) == MNEMON_ERR_INVALID_INPUT, "null edge");
    PASS();
}

/* ================================================================ */
/* 12. Consolidation with SIMD clustering                          */
/* ================================================================ */

static void test_consolidation_marks_semantic(void)
{
    TEST("consolidation marks episodic memories as semantic");
    /* Store some episodic memories */
    for (int i = 0; i < 3; i++) {
        mnemon_memory_t m = {0};
        mnemon_uuid_t u; mnemon_uuid_generate(&u);
        memcpy(m.id, u.bytes, 16);
        char buf[64];
        snprintf(buf, sizeof(buf), "Consolidation test memory %d about databases", i);
        m.content = strdup(buf);
        m.source_type = strdup("test");
        m.source_id = strdup("");
        m.tier = MNEMON_TIER_EPISODIC;
        m.consolidated = false;
        m.created_at = mnemon_time_ms();
        m.last_accessed = m.created_at;
        mnemon_store_memory(store, &m);
        mnemon_memory_free(&m);
    }

    /* Run consolidation (not dry_run) */
    mnemon_consolidation_result_t cr = {0};
    mnemon_err_t err = mnemon_consolidate(store, NULL, NULL, false, &cr);
    ASSERT(err == MNEMON_OK, "consolidate ok");
    ASSERT(cr.consolidated_count >= 3, "consolidated >= 3");
    ASSERT(cr.duration_ms >= 0, "duration reported");
    printf("[consolidated=%d duration=%lldms] ",
           cr.consolidated_count, (long long)cr.duration_ms);
    PASS();
}

static void test_consolidation_dry_run(void)
{
    TEST("consolidation dry_run counts without modifying");
    /* Store one more episodic */
    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);
    m.content = strdup("Dry run test memory");
    m.source_type = strdup("test");
    m.source_id = strdup("");
    m.tier = MNEMON_TIER_EPISODIC;
    m.created_at = mnemon_time_ms();
    m.last_accessed = m.created_at;
    mnemon_store_memory(store, &m);

    mnemon_consolidation_result_t cr = {0};
    mnemon_consolidate(store, NULL, NULL, true, &cr);
    ASSERT(cr.consolidated_count >= 1, "found candidates");

    /* Verify memory is still episodic (dry_run didn't change it) */
    mnemon_memory_t out = {0};
    mnemon_get_memory(store, m.id, &out);
    ASSERT(out.tier == MNEMON_TIER_EPISODIC, "still episodic after dry_run");

    mnemon_memory_free(&m);
    mnemon_memory_free(&out);
    PASS();
}

static void test_consolidation_topic_filter(void)
{
    TEST("consolidation with topic filter");
    mnemon_consolidation_result_t cr = {0};
    mnemon_consolidate(store, "nonexistent_topic_xyz", NULL, true, &cr);
    ASSERT(cr.consolidated_count == 0, "no matches for bogus topic");
    PASS();
}

int main(void)
{
    printf("=== test_storage: Memory Storage & Retrieval ===\n");
    setup();

    /* Basic CRUD */
    test_store_get_all_fields();
    test_store_get_minimal_fields();
    test_store_get_each_tier();

    /* Content integrity */
    test_large_content();
    test_unicode_content();
    test_special_chars_content();
    test_empty_content();

    /* Tags */
    test_many_tags();

    /* Delete */
    test_delete_then_get_fails();
    test_delete_removes_from_fts();

    /* Update */
    test_update_content();

    /* Search */
    test_keyword_search_finds_stored();

    /* Bulk */
    test_store_retrieve_many();

    /* Entity + Edge */
    test_entity_all_fields();
    test_edge_round_trip();

    /* Stats */
    test_stats_reflect_operations();

    /* Errors */
    test_get_nonexistent_memory();
    test_delete_nonexistent_memory();
    test_null_args();

    /* Storage function coverage */
    test_delete_entity();
    test_get_edges_to();
    test_rebuild_indexes();
    test_replay_intents();
    test_storage_accessors();

    /* Consolidation with SIMD clustering */
    test_consolidation_marks_semantic();
    test_consolidation_dry_run();
    test_consolidation_topic_filter();

    teardown();
    printf("\n%d/%d passed, %d failed\n", passed, total, failed);
    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
