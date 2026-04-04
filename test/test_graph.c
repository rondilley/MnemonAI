/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * test_graph.c -- LMDB knowledge graph unit tests
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "graph.h"
#include "id.h"
#include "mnemon.h"

static int passed = 0, failed = 0, total = 0;
#define TEST(n) do { total++; printf("  %-55s ", n); } while(0)
#define PASS() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(m) do { failed++; printf("FAIL: %s\n", m); return; } while(0)
#define ASSERT(c, m) do { if (!(c)) FAIL(m); } while(0)

static char tmpdir[256];

static void setup(void)
{
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/mnemon_test_graph_%d", getpid());
    mkdir(tmpdir, 0700);
}

static void teardown(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    if (system(cmd) != 0) { /* ignore */ }
}

/* ---- Entity CRUD ---- */

static void test_entity_put_get(void)
{
    TEST("entity put/get round-trip");
    mnemon_graph_t *g;
    ASSERT(mnemon_graph_open(&g, tmpdir, 1, 16) == MNEMON_OK, "open");

    mnemon_entity_t e = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(e.id, u.bytes, 16);
    e.name = strdup("Alice");
    e.entity_type = strdup("person");
    e.importance = 0.8f;
    e.created_at = 1000;

    MDB_txn *txn;
    ASSERT(mnemon_graph_txn_begin(g, 0, &txn) == MNEMON_OK, "txn");
    ASSERT(mnemon_graph_put_entity(g, txn, &e) == MNEMON_OK, "put");
    ASSERT(mnemon_graph_txn_commit(txn) == MNEMON_OK, "commit");

    /* Read back */
    ASSERT(mnemon_graph_txn_begin(g, MDB_RDONLY, &txn) == MNEMON_OK, "rd txn");
    mnemon_entity_t out = {0};
    ASSERT(mnemon_graph_get_entity(g, txn, e.id, &out) == MNEMON_OK, "get");
    ASSERT(strcmp(out.name, "Alice") == 0, "name match");
    ASSERT(strcmp(out.entity_type, "person") == 0, "type match");
    ASSERT(out.importance > 0.7f && out.importance < 0.9f, "importance ~0.8");
    ASSERT(out.created_at == 1000, "created_at");
    mnemon_graph_txn_abort(txn);

    mnemon_entity_free(&e);
    mnemon_entity_free(&out);
    mnemon_graph_close(g);
    PASS();
}

static void test_entity_not_found(void)
{
    TEST("entity get not found");
    mnemon_graph_t *g;
    ASSERT(mnemon_graph_open(&g, tmpdir, 1, 16) == MNEMON_OK, "open");

    MDB_txn *txn;
    ASSERT(mnemon_graph_txn_begin(g, MDB_RDONLY, &txn) == MNEMON_OK, "txn");
    uint8_t fake[16] = {0xff};
    mnemon_entity_t out = {0};
    ASSERT(mnemon_graph_get_entity(g, txn, fake, &out) == MNEMON_ERR_NOT_FOUND, "not found");
    mnemon_graph_txn_abort(txn);

    mnemon_graph_close(g);
    PASS();
}

static void test_entity_delete(void)
{
    TEST("entity delete");
    mnemon_graph_t *g;
    ASSERT(mnemon_graph_open(&g, tmpdir, 1, 16) == MNEMON_OK, "open");

    mnemon_entity_t e = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(e.id, u.bytes, 16);
    e.name = strdup("Bob");
    e.entity_type = strdup("person");

    MDB_txn *txn;
    mnemon_graph_txn_begin(g, 0, &txn);
    mnemon_graph_put_entity(g, txn, &e);
    mnemon_graph_txn_commit(txn);

    /* Delete */
    mnemon_graph_txn_begin(g, 0, &txn);
    ASSERT(mnemon_graph_del_entity(g, txn, e.id) == MNEMON_OK, "del");
    mnemon_graph_txn_commit(txn);

    /* Verify gone */
    mnemon_graph_txn_begin(g, MDB_RDONLY, &txn);
    mnemon_entity_t out = {0};
    ASSERT(mnemon_graph_get_entity(g, txn, e.id, &out) == MNEMON_ERR_NOT_FOUND, "gone");
    mnemon_graph_txn_abort(txn);

    mnemon_entity_free(&e);
    mnemon_graph_close(g);
    PASS();
}

/* ---- Entity with observations ---- */

static void test_entity_observations(void)
{
    TEST("entity with observations round-trip");
    mnemon_graph_t *g;
    ASSERT(mnemon_graph_open(&g, tmpdir, 1, 16) == MNEMON_OK, "open");

    mnemon_entity_t e = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(e.id, u.bytes, 16);
    e.name = strdup("Project X");
    e.entity_type = strdup("project");
    e.observation_count = 3;
    e.observations = calloc(3, sizeof(char *));
    e.observations[0] = strdup("Started in Q1");
    e.observations[1] = strdup("Budget: $500K");
    e.observations[2] = strdup("Team size: 12");

    MDB_txn *txn;
    mnemon_graph_txn_begin(g, 0, &txn);
    mnemon_graph_put_entity(g, txn, &e);
    mnemon_graph_txn_commit(txn);

    mnemon_graph_txn_begin(g, MDB_RDONLY, &txn);
    mnemon_entity_t out = {0};
    ASSERT(mnemon_graph_get_entity(g, txn, e.id, &out) == MNEMON_OK, "get");
    ASSERT(out.observation_count == 3, "obs count");
    ASSERT(strcmp(out.observations[0], "Started in Q1") == 0, "obs[0]");
    ASSERT(strcmp(out.observations[2], "Team size: 12") == 0, "obs[2]");
    mnemon_graph_txn_abort(txn);

    mnemon_entity_free(&e);
    mnemon_entity_free(&out);
    mnemon_graph_close(g);
    PASS();
}

/* ---- Memory CRUD ---- */

static void test_memory_put_get(void)
{
    TEST("memory put/get round-trip");
    mnemon_graph_t *g;
    ASSERT(mnemon_graph_open(&g, tmpdir, 1, 16) == MNEMON_OK, "open");

    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);
    m.tier = MNEMON_TIER_EPISODIC;
    m.content = strdup("The user asked about LMDB performance");
    m.source_type = strdup("mcp");
    m.source_id = strdup("session-123");
    m.importance = 0.6f;
    m.created_at = 2000;
    m.tag_count = 2;
    m.tags = calloc(2, sizeof(char *));
    m.tags[0] = strdup("lmdb");
    m.tags[1] = strdup("performance");

    MDB_txn *txn;
    mnemon_graph_txn_begin(g, 0, &txn);
    ASSERT(mnemon_graph_put_memory(g, txn, &m) == MNEMON_OK, "put");
    mnemon_graph_txn_commit(txn);

    mnemon_graph_txn_begin(g, MDB_RDONLY, &txn);
    mnemon_memory_t out = {0};
    ASSERT(mnemon_graph_get_memory(g, txn, m.id, &out) == MNEMON_OK, "get");
    ASSERT(strcmp(out.content, "The user asked about LMDB performance") == 0, "content");
    ASSERT(out.tier == MNEMON_TIER_EPISODIC, "tier");
    ASSERT(strcmp(out.source_type, "mcp") == 0, "source_type");
    ASSERT(out.tag_count == 2, "tag_count");
    ASSERT(strcmp(out.tags[0], "lmdb") == 0, "tag[0]");
    mnemon_graph_txn_abort(txn);

    mnemon_memory_free(&m);
    mnemon_memory_free(&out);
    mnemon_graph_close(g);
    PASS();
}

/* ---- Edge CRUD ---- */

static void test_edge_put_get(void)
{
    TEST("edge put/get_edges_from");
    mnemon_graph_t *g;
    ASSERT(mnemon_graph_open(&g, tmpdir, 1, 16) == MNEMON_OK, "open");

    mnemon_uuid_t u1, u2, u3;
    mnemon_uuid_generate(&u1);
    mnemon_uuid_generate(&u2);
    mnemon_uuid_generate(&u3);

    mnemon_edge_t e = {0};
    memcpy(e.id, u3.bytes, 16);
    memcpy(e.source_id, u1.bytes, 16);
    memcpy(e.target_id, u2.bytes, 16);
    e.edge_type = strdup("knows");
    e.description = strdup("colleagues");
    e.weight = 0.9f;
    e.valid_from = 1000;
    e.created_at = 1000;

    MDB_txn *txn;
    mnemon_graph_txn_begin(g, 0, &txn);
    ASSERT(mnemon_graph_put_edge(g, txn, &e) == MNEMON_OK, "put");
    mnemon_graph_txn_commit(txn);

    /* Query edges from source */
    mnemon_graph_txn_begin(g, MDB_RDONLY, &txn);
    mnemon_edge_list_t list = {0};
    ASSERT(mnemon_graph_get_edges_from(g, txn, u1.bytes, NULL, &list) == MNEMON_OK, "get");
    ASSERT(list.count >= 1, "found edge");
    mnemon_graph_txn_abort(txn);

    mnemon_edge_free(&e);
    mnemon_edge_list_free(&list);
    mnemon_graph_close(g);
    PASS();
}

/* ---- Counts ---- */

static void test_counts(void)
{
    TEST("entity/edge/memory counts");
    mnemon_graph_t *g;
    ASSERT(mnemon_graph_open(&g, tmpdir, 1, 16) == MNEMON_OK, "open");

    MDB_txn *txn;
    mnemon_graph_txn_begin(g, MDB_RDONLY, &txn);
    size_t ent, edg, mem;
    ASSERT(mnemon_graph_count(g, txn, &ent, &edg, &mem) == MNEMON_OK, "count");
    /* We've inserted items in previous tests so counts should be > 0 */
    printf("[ent=%zu edg=%zu mem=%zu] ", ent, edg, mem);
    mnemon_graph_txn_abort(txn);
    mnemon_graph_close(g);
    PASS();
}

/* ---- Meta ---- */

static void test_meta(void)
{
    TEST("meta put/get");
    mnemon_graph_t *g;
    ASSERT(mnemon_graph_open(&g, tmpdir, 1, 16) == MNEMON_OK, "open");

    MDB_txn *txn;
    mnemon_graph_txn_begin(g, 0, &txn);
    ASSERT(mnemon_graph_put_meta(g, txn, "test_key", "test_value") == MNEMON_OK, "put");
    mnemon_graph_txn_commit(txn);

    mnemon_graph_txn_begin(g, MDB_RDONLY, &txn);
    char buf[64];
    ASSERT(mnemon_graph_get_meta(g, txn, "test_key", buf, sizeof(buf)) == MNEMON_OK, "get");
    ASSERT(strcmp(buf, "test_value") == 0, "value match");
    mnemon_graph_txn_abort(txn);

    mnemon_graph_close(g);
    PASS();
}

/* ---- Transaction abort ---- */

static void test_txn_abort_rollback(void)
{
    TEST("transaction abort rolls back changes");
    mnemon_graph_t *g;
    ASSERT(mnemon_graph_open(&g, tmpdir, 1, 16) == MNEMON_OK, "open");

    mnemon_entity_t e = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(e.id, u.bytes, 16);
    e.name = strdup("Temporary");
    e.entity_type = strdup("test");

    MDB_txn *txn;
    mnemon_graph_txn_begin(g, 0, &txn);
    mnemon_graph_put_entity(g, txn, &e);
    mnemon_graph_txn_abort(txn); /* ABORT - should not persist */

    mnemon_graph_txn_begin(g, MDB_RDONLY, &txn);
    mnemon_entity_t out = {0};
    ASSERT(mnemon_graph_get_entity(g, txn, e.id, &out) == MNEMON_ERR_NOT_FOUND, "aborted");
    mnemon_graph_txn_abort(txn);

    mnemon_entity_free(&e);
    mnemon_graph_close(g);
    PASS();
}

/* ---- Schema version ---- */

static void test_schema_version(void)
{
    TEST("schema version set on first open");
    mnemon_graph_t *g;
    ASSERT(mnemon_graph_open(&g, tmpdir, 1, 16) == MNEMON_OK, "open");

    MDB_txn *txn;
    mnemon_graph_txn_begin(g, MDB_RDONLY, &txn);
    char buf[16];
    ASSERT(mnemon_graph_get_meta(g, txn, "schema_version", buf, sizeof(buf)) == MNEMON_OK, "get");
    ASSERT(strcmp(buf, "1") == 0, "version 1");
    mnemon_graph_txn_abort(txn);

    mnemon_graph_close(g);
    PASS();
}

/* ---- Memory delete ---- */

static void test_memory_delete(void)
{
    TEST("memory put then delete");
    mnemon_graph_t *g;
    ASSERT(mnemon_graph_open(&g, tmpdir, 1, 16) == MNEMON_OK, "open");

    mnemon_memory_t m = {0};
    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    memcpy(m.id, u.bytes, 16);
    m.content = strdup("Will be deleted");
    m.source_type = strdup("test");
    m.source_id = strdup("");
    m.created_at = 9999;
    m.last_accessed = 9999;

    MDB_txn *txn;
    mnemon_graph_txn_begin(g, 0, &txn);
    mnemon_graph_put_memory(g, txn, &m);
    mnemon_graph_txn_commit(txn);

    mnemon_graph_txn_begin(g, 0, &txn);
    ASSERT(mnemon_graph_del_memory(g, txn, m.id) == MNEMON_OK, "del");
    mnemon_graph_txn_commit(txn);

    mnemon_graph_txn_begin(g, MDB_RDONLY, &txn);
    mnemon_memory_t out = {0};
    ASSERT(mnemon_graph_get_memory(g, txn, m.id, &out) == MNEMON_ERR_NOT_FOUND, "gone");
    mnemon_graph_txn_abort(txn);

    mnemon_memory_free(&m);
    mnemon_graph_close(g);
    PASS();
}

/* ---- Edge reverse lookup ---- */

static void test_edges_to(void)
{
    TEST("get_edges_to reverse edge lookup");
    mnemon_graph_t *g;
    ASSERT(mnemon_graph_open(&g, tmpdir, 1, 16) == MNEMON_OK, "open");

    mnemon_uuid_t u1, u2, u3;
    mnemon_uuid_generate(&u1);
    mnemon_uuid_generate(&u2);
    mnemon_uuid_generate(&u3);

    mnemon_edge_t e = {0};
    memcpy(e.id, u3.bytes, 16);
    memcpy(e.source_id, u1.bytes, 16);
    memcpy(e.target_id, u2.bytes, 16);
    e.edge_type = strdup("points_to");
    e.weight = 1.0f;
    e.valid_from = 1000;
    e.created_at = 1000;

    MDB_txn *txn;
    mnemon_graph_txn_begin(g, 0, &txn);
    mnemon_graph_put_edge(g, txn, &e);
    mnemon_graph_txn_commit(txn);

    /* Reverse lookup: edges TO u2 */
    mnemon_graph_txn_begin(g, MDB_RDONLY, &txn);
    mnemon_edge_list_t list = {0};
    ASSERT(mnemon_graph_get_edges_to(g, txn, u2.bytes, NULL, &list) == MNEMON_OK, "get_to");
    ASSERT(list.count >= 1, "found reverse edge");
    mnemon_graph_txn_abort(txn);

    mnemon_edge_free(&e);
    mnemon_edge_list_free(&list);
    mnemon_graph_close(g);
    PASS();
}

/* ---- Intent log ---- */

static void test_intent_lifecycle(void)
{
    TEST("intent put/update/delete lifecycle");
    mnemon_graph_t *g;
    ASSERT(mnemon_graph_open(&g, tmpdir, 1, 16) == MNEMON_OK, "open");

    mnemon_uuid_t u; mnemon_uuid_generate(&u);
    MDB_txn *txn;

    /* Put intent */
    mnemon_graph_txn_begin(g, 0, &txn);
    ASSERT(mnemon_graph_put_intent(g, txn, u.bytes, 1, 0, NULL, 0) == MNEMON_OK, "put");
    mnemon_graph_txn_commit(txn);

    /* Update intent */
    mnemon_graph_txn_begin(g, 0, &txn);
    ASSERT(mnemon_graph_update_intent(g, txn, u.bytes, 0x03) == MNEMON_OK, "update");
    mnemon_graph_txn_commit(txn);

    /* Delete intent */
    mnemon_graph_txn_begin(g, 0, &txn);
    ASSERT(mnemon_graph_del_intent(g, txn, u.bytes) == MNEMON_OK, "del");
    mnemon_graph_txn_commit(txn);

    mnemon_graph_close(g);
    PASS();
}

/* ---- BFS traversal ---- */

static int bfs_count;
static int bfs_visitor(const mnemon_entity_t *entity, const mnemon_edge_t *edge,
                       int depth, void *ctx)
{
    (void)entity; (void)edge; (void)depth; (void)ctx;
    bfs_count++;
    return 0;
}

static void test_bfs(void)
{
    TEST("BFS traversal visits connected nodes");
    mnemon_graph_t *g;
    ASSERT(mnemon_graph_open(&g, tmpdir, 1, 16) == MNEMON_OK, "open");

    /* Create 3 entities: A -> B -> C */
    mnemon_uuid_t ua, ub, uc;
    mnemon_uuid_generate(&ua);
    mnemon_uuid_generate(&ub);
    mnemon_uuid_generate(&uc);

    MDB_txn *txn;
    mnemon_graph_txn_begin(g, 0, &txn);

    mnemon_entity_t ea = {0}; memcpy(ea.id, ua.bytes, 16);
    ea.name = strdup("BFS_A"); ea.entity_type = strdup("test");
    mnemon_graph_put_entity(g, txn, &ea);

    mnemon_entity_t eb = {0}; memcpy(eb.id, ub.bytes, 16);
    eb.name = strdup("BFS_B"); eb.entity_type = strdup("test");
    mnemon_graph_put_entity(g, txn, &eb);

    mnemon_entity_t ec = {0}; memcpy(ec.id, uc.bytes, 16);
    ec.name = strdup("BFS_C"); ec.entity_type = strdup("test");
    mnemon_graph_put_entity(g, txn, &ec);

    mnemon_uuid_t edge_id;
    mnemon_edge_t e1 = {0};
    mnemon_uuid_generate(&edge_id); memcpy(e1.id, edge_id.bytes, 16);
    memcpy(e1.source_id, ua.bytes, 16); memcpy(e1.target_id, ub.bytes, 16);
    e1.edge_type = strdup("links"); e1.weight = 1.0f;
    mnemon_graph_put_edge(g, txn, &e1);

    mnemon_edge_t e2 = {0};
    mnemon_uuid_generate(&edge_id); memcpy(e2.id, edge_id.bytes, 16);
    memcpy(e2.source_id, ub.bytes, 16); memcpy(e2.target_id, uc.bytes, 16);
    e2.edge_type = strdup("links"); e2.weight = 1.0f;
    mnemon_graph_put_edge(g, txn, &e2);

    mnemon_graph_txn_commit(txn);

    /* BFS from A with depth 3 should visit A, B, C */
    mnemon_graph_txn_begin(g, MDB_RDONLY, &txn);
    bfs_count = 0;
    ASSERT(mnemon_graph_bfs(g, txn, ua.bytes, 3, bfs_visitor, NULL) == MNEMON_OK, "bfs");
    ASSERT(bfs_count >= 3, "visited 3+ nodes");
    printf("[visited=%d] ", bfs_count);
    mnemon_graph_txn_abort(txn);

    mnemon_entity_free(&ea); mnemon_entity_free(&eb); mnemon_entity_free(&ec);
    mnemon_edge_free(&e1); mnemon_edge_free(&e2);
    mnemon_graph_close(g);
    PASS();
}

/* ---- env accessor ---- */

static void test_env_accessor(void)
{
    TEST("mnemon_graph_env returns non-NULL");
    mnemon_graph_t *g;
    ASSERT(mnemon_graph_open(&g, tmpdir, 1, 16) == MNEMON_OK, "open");
    ASSERT(mnemon_graph_env(g) != NULL, "env non-null");
    mnemon_graph_close(g);
    PASS();
}

int main(void)
{
    printf("=== test_graph: LMDB Knowledge Graph ===\n");
    setup();

    test_entity_put_get();
    test_entity_not_found();
    test_entity_delete();
    test_entity_observations();
    test_memory_put_get();
    test_edge_put_get();
    test_counts();
    test_meta();
    test_txn_abort_rollback();
    test_schema_version();
    test_memory_delete();
    test_edges_to();
    test_intent_lifecycle();
    test_bfs();
    test_env_accessor();

    teardown();
    printf("\n%d/%d passed, %d failed\n", passed, total, failed);
    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
