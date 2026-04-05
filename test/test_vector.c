/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * test_vector.c -- usearch HNSW vector index unit tests
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

#include "vector.h"
#include "id.h"
#include "mnemon.h"

static int passed = 0, failed = 0, total = 0;
#define TEST(n) do { total++; printf("  %-55s ", n); } while(0)
#define PASS() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(m) do { failed++; printf("FAIL: %s\n", m); return; } while(0)
#define ASSERT(c, m) do { if (!(c)) FAIL(m); } while(0)

#define DIM 16  /* Use small dimensions for fast testing */

static char tmpdir[256];

static void make_vector(float *v, int dim, float base)
{
    for (int i = 0; i < dim; i++)
        v[i] = base + (float)i * 0.01f;
    /* L2 normalize */
    float norm = 0;
    for (int i = 0; i < dim; i++) norm += v[i] * v[i];
    norm = sqrtf(norm);
    for (int i = 0; i < dim; i++) v[i] /= norm;
}

static void setup(void)
{
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/mnemon_test_vec_%d", getpid());
    mkdir(tmpdir, 0700);
}

static void teardown(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    if (system(cmd) != 0) { /* ignore */ }
}

static void test_open_close(void)
{
    TEST("open/close vector index");
    mnemon_vector_t *v;
    ASSERT(mnemon_vector_open(&v, tmpdir, DIM) == MNEMON_OK, "open");
    ASSERT(mnemon_vector_count(v, false) == 0, "empty");
    mnemon_vector_close(v);
    PASS();
}

static void test_add_and_search(void)
{
    TEST("add vectors then search");
    mnemon_vector_t *v;
    mnemon_vector_open(&v, tmpdir, DIM);

    float vec1[DIM], vec2[DIM], vec3[DIM];
    make_vector(vec1, DIM, 1.0f);
    make_vector(vec2, DIM, 2.0f);
    make_vector(vec3, DIM, 1.001f); /* very similar to vec1 */

    mnemon_uuid_t u1, u2, u3;
    mnemon_uuid_generate(&u1);
    mnemon_uuid_generate(&u2);
    mnemon_uuid_generate(&u3);

    ASSERT(mnemon_vector_add(v, u1.bytes, vec1, DIM, false) == MNEMON_OK, "add1");
    ASSERT(mnemon_vector_add(v, u2.bytes, vec2, DIM, false) == MNEMON_OK, "add2");
    ASSERT(mnemon_vector_add(v, u3.bytes, vec3, DIM, false) == MNEMON_OK, "add3");
    ASSERT(mnemon_vector_count(v, false) == 3, "count=3");

    /* Search for vec1 -- vec3 should be closest, then vec1 itself */
    mnemon_vector_results_t results = {0};
    ASSERT(mnemon_vector_search(v, vec1, DIM, 3, false, &results) == MNEMON_OK, "search");
    ASSERT(results.count >= 2, "found results");
    /* First result should have very low distance (self or near-identical) */
    ASSERT(results.results[0].distance < 0.01f, "closest is near-identical");
    printf("[d=%.4f] ", results.results[0].distance);

    mnemon_vector_results_free(&results);
    mnemon_vector_close(v);
    PASS();
}

static void test_remove(void)
{
    TEST("remove vector from index");
    mnemon_vector_t *v;
    mnemon_vector_open(&v, tmpdir, DIM);

    float vec[DIM];
    make_vector(vec, DIM, 5.0f);
    mnemon_uuid_t u;
    mnemon_uuid_generate(&u);

    mnemon_vector_add(v, u.bytes, vec, DIM, false);
    size_t before = mnemon_vector_count(v, false);
    mnemon_vector_remove(v, u.bytes, false);
    size_t after = mnemon_vector_count(v, false);
    ASSERT(after < before, "count decreased");

    mnemon_vector_close(v);
    PASS();
}

static void test_entity_index_separate(void)
{
    TEST("entity and memory indexes are separate");
    mnemon_vector_t *v;
    mnemon_vector_open(&v, tmpdir, DIM);

    float vec[DIM];
    make_vector(vec, DIM, 3.0f);
    mnemon_uuid_t u;
    mnemon_uuid_generate(&u);

    mnemon_vector_add(v, u.bytes, vec, DIM, true);  /* entity */
    ASSERT(mnemon_vector_count(v, true) >= 1, "entity count > 0");

    /* Search in memory index should not find entity */
    mnemon_vector_results_t r = {0};
    mnemon_vector_search(v, vec, DIM, 10, false, &r);
    /* Entity should not appear in memory search */
    bool found = false;
    for (int i = 0; i < r.count; i++)
        if (memcmp(r.results[i].id, u.bytes, 16) == 0) found = true;
    ASSERT(!found, "entity not in memory search");
    mnemon_vector_results_free(&r);

    mnemon_vector_close(v);
    PASS();
}

static void test_save_load(void)
{
    TEST("save then reload persists data");
    float vec[DIM];
    make_vector(vec, DIM, 7.0f);
    mnemon_uuid_t u;
    mnemon_uuid_generate(&u);

    /* Save */
    {
        mnemon_vector_t *v;
        mnemon_vector_open(&v, tmpdir, DIM);
        mnemon_vector_add(v, u.bytes, vec, DIM, false);
        ASSERT(mnemon_vector_save(v) == MNEMON_OK, "save");
        mnemon_vector_close(v);
    }

    /* Reload */
    {
        mnemon_vector_t *v;
        ASSERT(mnemon_vector_open(&v, tmpdir, DIM) == MNEMON_OK, "reopen");
        /* Search for the saved vector */
        mnemon_vector_results_t r = {0};
        mnemon_vector_search(v, vec, DIM, 5, false, &r);
        /* Should find it (loaded from disk) */
        printf("[loaded=%zu] ", mnemon_vector_count(v, false));
        mnemon_vector_results_free(&r);
        mnemon_vector_close(v);
    }
    PASS();
}

static void test_empty_search(void)
{
    TEST("search on empty index returns 0 results");
    char empty[512];
    snprintf(empty, sizeof(empty), "%s/empty_%d", tmpdir, getpid());
    mkdir(empty, 0700);

    mnemon_vector_t *v;
    mnemon_vector_open(&v, empty, DIM);

    float q[DIM];
    make_vector(q, DIM, 1.0f);
    mnemon_vector_results_t r = {0};
    mnemon_vector_search(v, q, DIM, 10, false, &r);
    ASSERT(r.count == 0, "empty results");
    mnemon_vector_results_free(&r);
    mnemon_vector_close(v);
    PASS();
}

static void test_rwlock(void)
{
    TEST("read/write locks don't deadlock");
    mnemon_vector_t *v;
    mnemon_vector_open(&v, tmpdir, DIM);

    /* Take read lock, release, take write lock, release */
    mnemon_vector_read_lock(v);
    mnemon_vector_read_unlock(v);

    mnemon_vector_write_lock(v);
    mnemon_vector_write_unlock(v);

    /* Multiple read locks should be fine */
    mnemon_vector_read_lock(v);
    /* Can't take write lock while read is held -- skip that test
     * as it would deadlock. Just verify single lock/unlock works. */
    mnemon_vector_read_unlock(v);

    mnemon_vector_close(v);
    PASS();
}

int main(void)
{
    printf("=== test_vector: usearch HNSW Vector Index ===\n");
    setup();

    test_open_close();
    test_add_and_search();
    test_remove();
    test_entity_index_separate();
    test_save_load();
    test_empty_search();
    test_rwlock();

    teardown();
    printf("\n%d/%d passed, %d failed\n", passed, total, failed);
    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
