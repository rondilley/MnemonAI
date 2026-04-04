/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * secret.c -- FSM-based secret pattern detection
 *
 * Single-pass scanner detects API keys, tokens, passwords, private keys,
 * JWTs, and high-entropy strings. O(n) with no backtracking.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include "secret.h"

const char *mnemon_secret_type_name(mnemon_secret_type_t type)
{
    switch (type) {
    case MNEMON_SECRET_NONE:                return "none";
    case MNEMON_SECRET_GITHUB_TOKEN:        return "GitHub token";
    case MNEMON_SECRET_OPENAI_KEY:          return "OpenAI API key";
    case MNEMON_SECRET_AWS_KEY:             return "AWS access key";
    case MNEMON_SECRET_PRIVATE_KEY:         return "private key";
    case MNEMON_SECRET_JWT:                 return "JWT";
    case MNEMON_SECRET_API_KEY_ASSIGNMENT:  return "API key assignment";
    case MNEMON_SECRET_PASSWORD_ASSIGNMENT: return "password assignment";
    case MNEMON_SECRET_HIGH_ENTROPY:        return "high-entropy string";
    }
    return "unknown";
}

static void add_match(mnemon_secret_result_t *r, mnemon_secret_type_t type,
                      size_t offset, size_t length)
{
    if (r->count >= r->capacity) {
        size_t newcap = r->capacity ? r->capacity * 2 : 8;
        mnemon_secret_match_t *p = realloc(r->matches,
                                            newcap * sizeof(*p));
        if (!p) return;
        r->matches = p;
        r->capacity = newcap;
    }
    r->matches[r->count].type = type;
    r->matches[r->count].offset = offset;
    r->matches[r->count].length = length;
    r->count++;
}

static inline bool is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

static inline bool is_base64url(char c)
{
    return is_alnum(c) || c == '+' || c == '/' || c == '-' || c == '_' ||
           c == '=';
}

float mnemon_entropy(const char *data, size_t len)
{
    int freq[256] = {0};
    size_t i;
    float entropy = 0.0f;

    if (len == 0) return 0.0f;

    for (i = 0; i < len; i++)
        freq[(unsigned char)data[i]]++;

    for (i = 0; i < 256; i++) {
        if (freq[i] == 0) continue;
        float p = (float)freq[i] / (float)len;
        entropy -= p * log2f(p);
    }

    return entropy;
}

mnemon_err_t mnemon_secret_scan(const char *data, size_t len,
                                mnemon_secret_result_t *result)
{
    size_t i;

    if (!data || !result)
        return MNEMON_ERR_INVALID_INPUT;

    memset(result, 0, sizeof(*result));

    for (i = 0; i < len; i++) {
        /* GitHub tokens: gh[phosr]_XXXX... (36+ chars after _) */
        if (i + 40 < len && data[i] == 'g' && data[i+1] == 'h' &&
            (data[i+2] == 'p' || data[i+2] == 'o' || data[i+2] == 's' ||
             data[i+2] == 'r') && data[i+3] == '_') {
            size_t start = i;
            size_t j = i + 4;
            while (j < len && (is_alnum(data[j]) || data[j] == '_')) j++;
            if (j - i - 4 >= 36) {
                add_match(result, MNEMON_SECRET_GITHUB_TOKEN, start, j - start);
                i = j - 1;
                continue;
            }
        }

        /* OpenAI keys: sk-... (32+ chars) */
        if (i + 35 < len && data[i] == 's' && data[i+1] == 'k' &&
            data[i+2] == '-') {
            size_t start = i;
            size_t j = i + 3;
            while (j < len && (is_alnum(data[j]) || data[j] == '-' || data[j] == '_')) j++;
            if (j - i - 3 >= 32) {
                add_match(result, MNEMON_SECRET_OPENAI_KEY, start, j - start);
                i = j - 1;
                continue;
            }
        }

        /* AWS keys: AKIA/ABIA/ACCA/ASIA + 16 uppercase alnum */
        if (i + 20 <= len && data[i] == 'A' &&
            (data[i+1] == 'K' || data[i+1] == 'B' || data[i+1] == 'C' ||
             data[i+1] == 'S') &&
            (data[i+2] == 'I' || data[i+2] == 'C') && data[i+3] == 'A') {
            size_t j = i + 4;
            int ucount = 0;
            while (j < len && ((data[j] >= 'A' && data[j] <= 'Z') ||
                               (data[j] >= '0' && data[j] <= '9'))) {
                ucount++;
                j++;
            }
            if (ucount >= 16) {
                add_match(result, MNEMON_SECRET_AWS_KEY, i, j - i);
                i = j - 1;
                continue;
            }
        }

        /* Private keys: -----BEGIN ... PRIVATE KEY----- */
        if (i + 27 < len && strncmp(data + i, "-----BEGIN ", 11) == 0) {
            const char *end = strstr(data + i + 11, "PRIVATE KEY-----");
            if (end) {
                size_t match_len = (size_t)(end - (data + i)) + 16;
                add_match(result, MNEMON_SECRET_PRIVATE_KEY, i, match_len);
                i += match_len - 1;
                continue;
            }
        }

        /* JWTs: eyJ + base64url.base64url.base64url */
        if (i + 30 < len && data[i] == 'e' && data[i+1] == 'y' &&
            data[i+2] == 'J') {
            size_t j = i + 3;
            int dots = 0, seg_len = 0;
            bool valid = true;
            while (j < len && dots < 3) {
                if (data[j] == '.') {
                    if (seg_len < 10) { valid = false; break; }
                    dots++;
                    seg_len = 0;
                } else if (is_base64url(data[j])) {
                    seg_len++;
                } else {
                    break;
                }
                j++;
            }
            if (valid && dots >= 2 && seg_len >= 10) {
                add_match(result, MNEMON_SECRET_JWT, i, j - i);
                i = j - 1;
                continue;
            }
        }

        /* API key assignment: api[_-]key[:=] value */
        if (i + 12 < len && (data[i] == 'a' || data[i] == 'A') &&
            (data[i+1] == 'p' || data[i+1] == 'P') &&
            (data[i+2] == 'i' || data[i+2] == 'I')) {
            size_t j = i + 3;
            if (j < len && (data[j] == '_' || data[j] == '-')) j++;
            if (j + 3 < len &&
                (data[j] == 'k' || data[j] == 'K') &&
                (data[j+1] == 'e' || data[j+1] == 'E') &&
                (data[j+2] == 'y' || data[j+2] == 'Y')) {
                j += 3;
                while (j < len && data[j] == ' ') j++;
                if (j < len && (data[j] == '=' || data[j] == ':')) {
                    j++;
                    while (j < len && data[j] == ' ') j++;
                    if (j < len && (data[j] == '"' || data[j] == '\'')) j++;
                    size_t val_start = j;
                    while (j < len && !isspace((unsigned char)data[j]) &&
                           data[j] != '"' && data[j] != '\'') j++;
                    if (j - val_start >= 20) {
                        add_match(result, MNEMON_SECRET_API_KEY_ASSIGNMENT,
                                  i, j - i);
                        i = j - 1;
                        continue;
                    }
                }
            }
        }

        /* Password assignment: password/passwd/pwd [:=] value */
        if (i + 10 < len) {
            size_t klen = 0;
            if (strncasecmp(data + i, "password", 8) == 0) klen = 8;
            else if (strncasecmp(data + i, "passwd", 6) == 0) klen = 6;
            else if (strncasecmp(data + i, "pwd", 3) == 0) klen = 3;

            if (klen > 0) {
                size_t j = i + klen;
                while (j < len && data[j] == ' ') j++;
                if (j < len && (data[j] == '=' || data[j] == ':')) {
                    j++;
                    while (j < len && data[j] == ' ') j++;
                    if (j < len && (data[j] == '"' || data[j] == '\'')) j++;
                    size_t val_start = j;
                    while (j < len && !isspace((unsigned char)data[j]) &&
                           data[j] != '"' && data[j] != '\'') j++;
                    if (j - val_start >= 8) {
                        add_match(result, MNEMON_SECRET_PASSWORD_ASSIGNMENT,
                                  i, j - i);
                        i = j - 1;
                        continue;
                    }
                }
            }
        }
    }

    /* High-entropy detection: check words > 32 chars */
    i = 0;
    while (i < len) {
        while (i < len && isspace((unsigned char)data[i])) i++;
        size_t start = i;
        while (i < len && !isspace((unsigned char)data[i])) i++;
        size_t wlen = i - start;
        if (wlen > 32) {
            float ent = mnemon_entropy(data + start, wlen);
            if (ent > 4.5f)
                add_match(result, MNEMON_SECRET_HIGH_ENTROPY, start, wlen);
        }
    }

    return MNEMON_OK;
}

void mnemon_secret_result_free(mnemon_secret_result_t *result)
{
    if (!result) return;
    free(result->matches);
    memset(result, 0, sizeof(*result));
}

bool mnemon_secret_detected(const char *data, size_t len)
{
    mnemon_secret_result_t result;
    memset(&result, 0, sizeof(result));
    mnemon_secret_scan(data, len, &result);
    bool found = result.count > 0;
    mnemon_secret_result_free(&result);
    return found;
}
