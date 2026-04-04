/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * test_secret.c -- Aggressive tests for FSM-based secret detection
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "secret.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { tests_run++; printf("  %-55s ", name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { tests_failed++; printf("FAIL: %s\n", msg); } while(0)

#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* Helper: check if a specific secret type was found */
static bool has_type(const mnemon_secret_result_t *r, mnemon_secret_type_t t)
{
    for (size_t i = 0; i < r->count; i++)
        if (r->matches[i].type == t) return true;
    return false;
}

/* ================================================================ */
/* GitHub Token Detection                                           */
/* ================================================================ */

static void test_github_pat(void)
{
    TEST("detect GitHub PAT (ghp_)");
    const char *s = "ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmn";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_GITHUB_TOKEN), "should detect ghp_");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_github_oauth(void)
{
    TEST("detect GitHub OAuth (gho_)");
    const char *s = "gho_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmn";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_GITHUB_TOKEN), "should detect gho_");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_github_server(void)
{
    TEST("detect GitHub server token (ghs_)");
    const char *s = "ghs_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmn";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_GITHUB_TOKEN), "should detect ghs_");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_github_refresh(void)
{
    TEST("detect GitHub refresh token (ghr_)");
    const char *s = "ghr_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmn";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_GITHUB_TOKEN), "should detect ghr_");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_github_too_short(void)
{
    TEST("reject short GitHub token (< 36 chars after _)");
    const char *s = "ghp_ABCDEFshort";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(!has_type(&r, MNEMON_SECRET_GITHUB_TOKEN), "too short");
    PASS();
    mnemon_secret_result_free(&r);
}

/* ================================================================ */
/* OpenAI Key Detection                                             */
/* ================================================================ */

static void test_openai_sk(void)
{
    TEST("detect OpenAI key (sk-)");
    const char *s = "sk-ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefgh";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_OPENAI_KEY), "should detect sk-");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_openai_proj(void)
{
    TEST("detect OpenAI project key (sk-proj-)");
    const char *s = "sk-proj-ABCDEFGHIJKLMNOPQRSTUVWXYZabcde";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_OPENAI_KEY), "should detect sk-proj-");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_openai_too_short(void)
{
    TEST("reject short OpenAI key (< 32 chars after sk-)");
    const char *s = "sk-tooshort";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(!has_type(&r, MNEMON_SECRET_OPENAI_KEY), "too short");
    PASS();
    mnemon_secret_result_free(&r);
}

/* ================================================================ */
/* AWS Key Detection                                                */
/* ================================================================ */

static void test_aws_akia(void)
{
    TEST("detect AWS access key (AKIA...)");
    const char *s = "AKIAIOSFODNN7EXAMPLE1";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_AWS_KEY), "should detect AKIA");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_aws_asia(void)
{
    TEST("detect AWS STS key (ASIA...)");
    const char *s = "ASIAISAMPLEKEYVALUE0";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_AWS_KEY), "should detect ASIA");
    PASS();
    mnemon_secret_result_free(&r);
}

/* ================================================================ */
/* Private Key Detection                                            */
/* ================================================================ */

static void test_rsa_private_key(void)
{
    TEST("detect RSA private key header");
    const char *s = "-----BEGIN RSA PRIVATE KEY-----\nMIIE...data";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_PRIVATE_KEY), "should detect RSA key");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_ec_private_key(void)
{
    TEST("detect EC private key header");
    const char *s = "-----BEGIN EC PRIVATE KEY-----\ndata...";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_PRIVATE_KEY), "should detect EC key");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_generic_private_key(void)
{
    TEST("detect generic PRIVATE KEY header");
    const char *s = "-----BEGIN PRIVATE KEY-----\ndata...";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_PRIVATE_KEY), "should detect key");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_public_key_not_detected(void)
{
    TEST("reject PUBLIC KEY (not a secret)");
    const char *s = "-----BEGIN PUBLIC KEY-----\ndata...";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(!has_type(&r, MNEMON_SECRET_PRIVATE_KEY), "public key != secret");
    PASS();
    mnemon_secret_result_free(&r);
}

/* ================================================================ */
/* JWT Detection                                                    */
/* ================================================================ */

static void test_jwt_valid(void)
{
    TEST("detect valid JWT");
    const char *s = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
                    "eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4ifQ."
                    "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_JWT), "should detect JWT");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_jwt_in_header(void)
{
    TEST("detect JWT in Authorization header");
    const char *s = "Authorization: Bearer eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9."
                    "eyJzdWIiOiJ1c2VyMTIzIiwiZXhwIjoxNzAwMDAwMDAwfQ."
                    "signatureabcdefghijk";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_JWT), "JWT in auth header");
    PASS();
    mnemon_secret_result_free(&r);
}

/* ================================================================ */
/* API Key Assignment Detection                                     */
/* ================================================================ */

static void test_apikey_equals(void)
{
    TEST("detect api_key = <value>");
    const char *s = "api_key = sk_live_abcdefghijklmnopqrstu";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_API_KEY_ASSIGNMENT), "api_key =");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_apikey_colon(void)
{
    TEST("detect api-key: <value>");
    const char *s = "api-key: abcdefghijklmnopqrstuvwxyz";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_API_KEY_ASSIGNMENT), "api-key:");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_apikey_quoted(void)
{
    TEST("detect api_key = \"<quoted value>\"");
    const char *s = "api_key = \"sk_test_abcdefghijklmnopqrst\"";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_API_KEY_ASSIGNMENT), "quoted api_key");
    PASS();
    mnemon_secret_result_free(&r);
}

/* ================================================================ */
/* Password Assignment Detection                                    */
/* ================================================================ */

static void test_password_equals(void)
{
    TEST("detect password=<value>");
    const char *s = "password=MyS3cur3P@ss!";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_PASSWORD_ASSIGNMENT), "password=");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_passwd_colon(void)
{
    TEST("detect passwd: <value>");
    const char *s = "passwd: longpassword123";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_PASSWORD_ASSIGNMENT), "passwd:");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_pwd_short_not_detected(void)
{
    TEST("reject short password (< 8 chars)");
    const char *s = "pwd=abc";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(!has_type(&r, MNEMON_SECRET_PASSWORD_ASSIGNMENT), "too short");
    PASS();
    mnemon_secret_result_free(&r);
}

/* ================================================================ */
/* Shannon Entropy                                                  */
/* ================================================================ */

static void test_entropy_low_text(void)
{
    TEST("entropy: normal English text < 4.5");
    float e = mnemon_entropy("the quick brown fox jumps over lazy", 35);
    ASSERT(e < 4.5f, "normal text");
    PASS();
}

static void test_entropy_zero_repeated(void)
{
    TEST("entropy: repeated char = 0.0");
    float e = mnemon_entropy("aaaaaaaaaa", 10);
    ASSERT(fabsf(e) < 0.01f, "repeated char");
    PASS();
}

/* ================================================================ */
/* False Positives                                                  */
/* ================================================================ */

static void test_fp_normal_prose(void)
{
    TEST("false positive: normal prose");
    const char *s = "The committee discussed the project schedule "
                    "and decided to skip the meeting on Friday.";
    ASSERT(!mnemon_secret_detected(s, strlen(s)), "no secrets");
    PASS();
}

static void test_fp_c_code(void)
{
    TEST("false positive: C source code");
    const char *s = "int skip_whitespace(const char *s) { while (*s == ' ') s++; return 0; }";
    ASSERT(!mnemon_secret_detected(s, strlen(s)), "no secrets");
    PASS();
}

static void test_fp_url(void)
{
    TEST("false positive: URL with query params");
    const char *s = "https://example.com/api?key=value&format=json";
    ASSERT(!mnemon_secret_detected(s, strlen(s)), "no secrets");
    PASS();
}

/* ================================================================ */
/* Edge Cases                                                       */
/* ================================================================ */

static void test_empty_input(void)
{
    TEST("edge: empty input");
    mnemon_secret_result_t r = {0};
    mnemon_err_t err = mnemon_secret_scan("", 0, &r);
    ASSERT(err == MNEMON_OK, "should succeed");
    ASSERT(r.count == 0, "no matches");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_null_input(void)
{
    TEST("edge: NULL input returns error");
    mnemon_secret_result_t r = {0};
    mnemon_err_t err = mnemon_secret_scan(NULL, 0, &r);
    ASSERT(err == MNEMON_ERR_INVALID_INPUT, "should error");
    PASS();
}

static void test_multiple_secrets(void)
{
    TEST("edge: multiple secrets in one buffer");
    const char *s = "ghp_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA "
                    "sk-BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBbbbb "
                    "password=hunter2hunter2h";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(r.count >= 3, "at least 3 secrets");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_secret_at_buffer_start(void)
{
    TEST("edge: secret at buffer start");
    const char *s = "sk-ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefgh then text";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_OPENAI_KEY), "found at start");
    ASSERT(r.matches[0].offset == 0, "offset should be 0");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_secret_at_buffer_end(void)
{
    TEST("edge: secret at buffer end");
    const char *s = "text then ghp_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    mnemon_secret_result_t r = {0};
    mnemon_secret_scan(s, strlen(s), &r);
    ASSERT(has_type(&r, MNEMON_SECRET_GITHUB_TOKEN), "found at end");
    PASS();
    mnemon_secret_result_free(&r);
}

static void test_type_name_coverage(void)
{
    TEST("type name strings all non-NULL");
    for (int i = 0; i <= MNEMON_SECRET_HIGH_ENTROPY; i++) {
        const char *n = mnemon_secret_type_name((mnemon_secret_type_t)i);
        ASSERT(n != NULL, "name should exist");
        ASSERT(strlen(n) > 0, "name should be non-empty");
    }
    PASS();
}

int main(void)
{
    printf("=== test_secret: FSM Secret Detection (%d tests) ===\n", 35);

    /* GitHub tokens */
    test_github_pat();
    test_github_oauth();
    test_github_server();
    test_github_refresh();
    test_github_too_short();

    /* OpenAI keys */
    test_openai_sk();
    test_openai_proj();
    test_openai_too_short();

    /* AWS keys */
    test_aws_akia();
    test_aws_asia();

    /* Private keys */
    test_rsa_private_key();
    test_ec_private_key();
    test_generic_private_key();
    test_public_key_not_detected();

    /* JWTs */
    test_jwt_valid();
    test_jwt_in_header();

    /* API key assignments */
    test_apikey_equals();
    test_apikey_colon();
    test_apikey_quoted();

    /* Password assignments */
    test_password_equals();
    test_passwd_colon();
    test_pwd_short_not_detected();

    /* Entropy */
    test_entropy_low_text();
    test_entropy_zero_repeated();

    /* False positives */
    test_fp_normal_prose();
    test_fp_c_code();
    test_fp_url();

    /* Edge cases */
    test_empty_input();
    test_null_input();
    test_multiple_secrets();
    test_secret_at_buffer_start();
    test_secret_at_buffer_end();
    test_type_name_coverage();

    printf("\n%d/%d tests passed, %d failed\n", tests_passed, tests_run, tests_failed);
    return (tests_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
