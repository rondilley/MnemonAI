/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * test_temporal.c -- Temporal query and time utility tests
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "memory.h"
#include "id.h"
#include "mnemon.h"

static int passed = 0, failed = 0, total = 0;
#define TEST(n) do { total++; printf("  %-55s ", n); } while(0)
#define PASS() do { passed++; printf("PASS\n"); } while(0)
#define FAIL(m) do { failed++; printf("FAIL: %s\n", m); return; } while(0)
#define ASSERT(c, m) do { if (!(c)) FAIL(m); } while(0)

/* ---- ISO 8601 Parsing ---- */

static void test_iso8601_parse_basic(void)
{
    TEST("parse ISO 8601: 2026-01-15T10:30:00Z");
    int64_t ms = mnemon_parse_iso8601("2026-01-15T10:30:00Z");
    ASSERT(ms > 0, "non-zero");
    /* 2026-01-15 10:30:00 UTC = epoch + some large number */
    ASSERT(ms > 1700000000000LL, "after 2023");
    ASSERT(ms < 1900000000000LL, "before 2030");
    PASS();
}

static void test_iso8601_parse_null(void)
{
    TEST("parse ISO 8601: NULL returns 0");
    ASSERT(mnemon_parse_iso8601(NULL) == 0, "null -> 0");
    PASS();
}

static void test_iso8601_parse_garbage(void)
{
    TEST("parse ISO 8601: garbage returns 0");
    ASSERT(mnemon_parse_iso8601("not a date") == 0, "garbage -> 0");
    PASS();
}

static void test_iso8601_round_trip(void)
{
    TEST("ISO 8601 format/parse round-trip");
    int64_t now = mnemon_time_ms();
    char buf[32];
    mnemon_format_iso8601(now, buf, sizeof(buf));
    int64_t parsed = mnemon_parse_iso8601(buf);
    /* Should be within 1 second (format truncates sub-second) */
    int64_t diff = now - parsed;
    if (diff < 0) diff = -diff;
    ASSERT(diff < 1000, "round-trip within 1s");
    printf("[%s] ", buf);
    PASS();
}

/* ---- Time Utility ---- */

static void test_time_ms(void)
{
    TEST("mnemon_time_ms returns reasonable value");
    int64_t t = mnemon_time_ms();
    ASSERT(t > 1700000000000LL, "after 2023");
    ASSERT(t < 2000000000000LL, "before 2033");
    PASS();
}

static void test_time_ms_monotonic(void)
{
    TEST("mnemon_time_ms is monotonically increasing");
    int64_t t1 = mnemon_time_ms();
    int64_t t2 = mnemon_time_ms();
    ASSERT(t2 >= t1, "t2 >= t1");
    PASS();
}

/* ---- Importance Decay ---- */

static void test_decay_no_time(void)
{
    TEST("decay: no elapsed time = no decay");
    int64_t now = mnemon_time_ms();
    float d = mnemon_importance_decay(1.0f, now, now, 90);
    ASSERT(fabsf(d - 1.0f) < 0.001f, "no decay");
    PASS();
}

static void test_decay_one_half_life(void)
{
    TEST("decay: one half-life = 50% importance");
    int64_t now = mnemon_time_ms();
    int64_t ninety_days_ms = 90LL * 86400000LL;
    float d = mnemon_importance_decay(1.0f, now - ninety_days_ms, now, 90);
    ASSERT(fabsf(d - 0.5f) < 0.01f, "~0.5 after one half-life");
    PASS();
}

static void test_decay_two_half_lives(void)
{
    TEST("decay: two half-lives = 25% importance");
    int64_t now = mnemon_time_ms();
    int64_t one_eighty_days_ms = 180LL * 86400000LL;
    float d = mnemon_importance_decay(1.0f, now - one_eighty_days_ms, now, 90);
    ASSERT(fabsf(d - 0.25f) < 0.02f, "~0.25 after two half-lives");
    PASS();
}

/* ---- Importance Update ---- */

static void test_importance_update(void)
{
    TEST("importance_update increases toward 1.0");
    float imp = 0.5f;
    float updated = mnemon_importance_update(imp, 1);
    ASSERT(updated > imp, "increased");
    ASSERT(updated <= 1.0f, "<= 1.0");
    PASS();
}

static void test_importance_update_ceiling(void)
{
    TEST("importance_update from 0.99 stays <= 1.0");
    float updated = mnemon_importance_update(0.99f, 100);
    ASSERT(updated <= 1.0f, "<= 1.0");
    ASSERT(updated > 0.99f, "increased slightly");
    PASS();
}

/* ---- Prune ---- */

static void test_should_prune_fresh(void)
{
    TEST("should_prune: recently accessed = false");
    int64_t now = mnemon_time_ms();
    ASSERT(!mnemon_should_prune(0.5f, now, now, 90, 0.01f), "not prunable");
    PASS();
}

static void test_should_prune_stale(void)
{
    TEST("should_prune: very old + low importance = true");
    int64_t now = mnemon_time_ms();
    int64_t year_ago = now - 365LL * 86400000LL;
    ASSERT(mnemon_should_prune(0.1f, year_ago, now, 90, 0.01f), "prunable");
    PASS();
}

/* ---- UUID Tests ---- */

static void test_uuid_generate(void)
{
    TEST("UUID generate produces unique IDs");
    mnemon_uuid_t a, b;
    mnemon_uuid_generate(&a);
    mnemon_uuid_generate(&b);
    ASSERT(mnemon_uuid_compare(&a, &b) != 0, "unique");
    PASS();
}

static void test_uuid_string_round_trip(void)
{
    TEST("UUID to_string/from_string round-trip");
    mnemon_uuid_t orig, parsed;
    mnemon_uuid_generate(&orig);
    char buf[37];
    mnemon_uuid_to_string(&orig, buf, sizeof(buf));
    ASSERT(strlen(buf) == 36, "length 36");
    ASSERT(mnemon_uuid_from_string(buf, &parsed) == MNEMON_OK, "parse ok");
    ASSERT(mnemon_uuid_compare(&orig, &parsed) == 0, "matches");
    PASS();
}

static void test_uuid_is_zero(void)
{
    TEST("UUID is_zero on zeroed struct");
    mnemon_uuid_t z;
    memset(&z, 0, sizeof(z));
    ASSERT(mnemon_uuid_is_zero(&z), "is zero");
    mnemon_uuid_t nz;
    mnemon_uuid_generate(&nz);
    ASSERT(!mnemon_uuid_is_zero(&nz), "not zero");
    PASS();
}

static void test_uuid_invalid_string(void)
{
    TEST("UUID from_string rejects invalid input");
    mnemon_uuid_t u;
    ASSERT(mnemon_uuid_from_string("not-a-uuid", &u) == MNEMON_ERR_INVALID_INPUT, "rejected");
    ASSERT(mnemon_uuid_from_string(NULL, &u) == MNEMON_ERR_INVALID_INPUT, "null rejected");
    ASSERT(mnemon_uuid_from_string("", &u) == MNEMON_ERR_INVALID_INPUT, "empty rejected");
    PASS();
}

/* ---- Admission Control ---- */

#include "admit.h"

static void test_admit_normal(void)
{
    TEST("admit: normal content passes");
    ASSERT(mnemon_admit_check("This is a meaningful piece of content with information", 54), "admitted");
    PASS();
}

static void test_admit_boilerplate(void)
{
    TEST("admit: boilerplate rejected");
    ASSERT(!mnemon_admit_check("hello", 5), "hello rejected");
    ASSERT(!mnemon_admit_check("thanks", 6), "thanks rejected");
    ASSERT(!mnemon_admit_check("ok", 2), "ok rejected");
    ASSERT(!mnemon_admit_check("got it", 6), "got it rejected");
    PASS();
}

static void test_admit_too_short(void)
{
    TEST("admit: content < 10 chars rejected");
    ASSERT(!mnemon_admit_check("hi", 2), "too short");
    ASSERT(!mnemon_admit_check("", 0), "empty");
    ASSERT(!mnemon_admit_check(NULL, 0), "null");
    PASS();
}

/* ---- Audit Log ---- */

#include "audit.h"

static void test_audit_lifecycle(void)
{
    TEST("audit: open/log/close");
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mnemon_test_audit_%d.log", getpid());

    mnemon_audit_t *a = NULL;
    ASSERT(mnemon_audit_open(&a, path) == MNEMON_OK, "open");
    ASSERT(a != NULL, "non-null");

    ASSERT(mnemon_audit_log(a, "store_memory", "{\"content\":\"test\"}", "ok") == MNEMON_OK, "log1");
    ASSERT(mnemon_audit_log(a, "search_keyword", "{\"query\":\"test\"}", "3 results") == MNEMON_OK, "log2");

    mnemon_audit_close(a);

    /* Verify file contents */
    FILE *fp = fopen(path, "r");
    ASSERT(fp != NULL, "file exists");
    char line[512];
    int lines = 0;
    while (fgets(line, sizeof(line), fp)) lines++;
    fclose(fp);
    ASSERT(lines == 2, "2 log entries");

    unlink(path);
    PASS();
}

/* ---- Model Manager ---- */

#include "model_mgr.h"

static void test_model_recommend(void)
{
    TEST("model_mgr: recommend based on hardware");
    mnemon_hardware_t hw;
    memset(&hw, 0, sizeof(hw));

    /* 128GB RAM -> should recommend Q8_0 */
    hw.ram_total_bytes = 128ULL * 1024 * 1024 * 1024;
    const mnemon_model_rec_t *rec = mnemon_model_recommend(&hw);
    ASSERT(rec != NULL, "recommendation");
    ASSERT(rec->dimensions == 768, "768 dims");
    ASSERT(strstr(rec->filename, "Q8_0") != NULL, "Q8_0 for 128GB");
    printf("[%s] ", rec->filename);
    PASS();
}

static void test_model_recommend_low_ram(void)
{
    TEST("model_mgr: low RAM recommends smaller model");
    mnemon_hardware_t hw;
    memset(&hw, 0, sizeof(hw));
    hw.ram_total_bytes = 3ULL * 1024 * 1024 * 1024; /* 3GB */
    const mnemon_model_rec_t *rec = mnemon_model_recommend(&hw);
    ASSERT(rec != NULL, "recommendation");
    ASSERT(strstr(rec->filename, "Q4_K_M") != NULL, "Q4_K_M for 3GB");
    printf("[%s] ", rec->filename);
    PASS();
}

static void test_model_ensure_disabled(void)
{
    TEST("model_mgr: model_path='none' disables without download");
    char out[4096] = {0};
    mnemon_hardware_t hw = {0};
    hw.ram_total_bytes = 128ULL * 1024 * 1024 * 1024;
    mnemon_err_t err = mnemon_model_ensure("/tmp", "none", &hw, out, sizeof(out));
    ASSERT(err == MNEMON_ERR_EMBED, "disabled");
    ASSERT(out[0] == '\0', "empty path");
    PASS();
}

int main(void)
{
    printf("=== test_temporal: Time Utilities, Decay, UUID ===\n");

    test_iso8601_parse_basic();
    test_iso8601_parse_null();
    test_iso8601_parse_garbage();
    test_iso8601_round_trip();
    test_time_ms();
    test_time_ms_monotonic();
    test_decay_no_time();
    test_decay_one_half_life();
    test_decay_two_half_lives();
    test_importance_update();
    test_importance_update_ceiling();
    test_should_prune_fresh();
    test_should_prune_stale();
    test_uuid_generate();
    test_uuid_string_round_trip();
    test_uuid_is_zero();
    test_uuid_invalid_string();

    /* Admission control */
    test_admit_normal();
    test_admit_boilerplate();
    test_admit_too_short();

    /* Audit log */
    test_audit_lifecycle();

    /* Model manager */
    test_model_recommend();
    test_model_recommend_low_ram();
    test_model_ensure_disabled();

    printf("\n%d/%d passed, %d failed\n", passed, total, failed);
    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
