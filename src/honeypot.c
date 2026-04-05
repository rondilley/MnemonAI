/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * honeypot.c -- Abuse detection and deception
 *
 * Features:
 *   1. Canary record tracking -- planted memories with known UUIDs that
 *      trigger alerts when accessed via search or retrieve
 *   2. Prompt injection scanner -- scores content for injection patterns
 *   3. Auth brute-force tracker -- rate limits by source IP
 *   4. Search frequency anomaly -- flags rapid-fire searches per session
 *   5. Enumeration detector -- flags systematic list_memories pagination
 *   6. Credential query detector -- flags searches for keys/passwords/tokens
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>

#include "honeypot.h"
#include "memory.h"
#include "log.h"

/* ---- Constants ---- */

#define MAX_CANARIES    64
#define MAX_AUTH_TRACK   4096
#define AUTH_WINDOW_SEC  60
#define AUTH_FAIL_LIMIT  10
#define SEARCH_RATE_WARN 30
#define ENUM_PAGE_LIMIT  5
#define MAX_SESSION_TRACK 256

/* ---- Canary tracking ---- */

typedef struct {
    char uuid[37];
} canary_entry_t;

/* ---- Auth brute-force tracking ---- */

typedef struct {
    char     ip[48];
    int      fail_count;
    int64_t  window_start;
} auth_track_t;

/* ---- Per-session behavior tracking ---- */

typedef struct {
    char    session_id[64];
    int     search_count;     /* searches in current window */
    int64_t search_window;    /* start of current 60s window */
    int     enum_count;       /* sequential list_memories calls */
    int     last_offset;      /* last offset seen */
} session_track_t;

/* ---- Main honeypot struct ---- */

struct mnemon_honeypot {
    mnemon_audit_t  *audit;
    pthread_mutex_t  mutex;

    /* Canary UUIDs */
    canary_entry_t   canaries[MAX_CANARIES];
    int              canary_count;

    /* Auth tracking (hash by IP) */
    auth_track_t     auth_table[MAX_AUTH_TRACK];

    /* Session behavior tracking */
    session_track_t  sessions[MAX_SESSION_TRACK];
    int              session_count;
};

/* ---- Helpers ---- */

static uint32_t hash_string(const char *s)
{
    uint32_t h = 0x811c9dc5u;
    for (; s && *s; s++) { h ^= (uint8_t)*s; h *= 0x01000193u; }
    return h;
}

static session_track_t *find_session_track(mnemon_honeypot_t *hp,
                                            const char *session_id)
{
    if (!session_id) return NULL;
    for (int i = 0; i < hp->session_count; i++) {
        if (strcmp(hp->sessions[i].session_id, session_id) == 0)
            return &hp->sessions[i];
    }
    /* Create new */
    if (hp->session_count >= MAX_SESSION_TRACK)
        return NULL;
    session_track_t *s = &hp->sessions[hp->session_count++];
    memset(s, 0, sizeof(*s));
    snprintf(s->session_id, sizeof(s->session_id), "%s", session_id);
    s->search_window = mnemon_time_ms();
    s->last_offset = -1;
    return s;
}

/* ---- Prompt injection patterns ---- */

typedef struct {
    const char *pattern;
    float       score;
} injection_pattern_t;

static const injection_pattern_t injection_patterns[] = {
    /* Instruction override */
    {"ignore previous",          2.0f},
    {"ignore all instructions",  3.0f},
    {"disregard the above",      2.0f},
    {"disregard all previous",   3.0f},
    {"forget everything",        2.0f},
    {"new instructions:",        2.5f},
    {"system prompt:",           2.5f},
    {"you are now",              1.5f},

    /* Role hijacking */
    {"[SYSTEM]",                 2.0f},
    {"<|im_start|>",            3.0f},
    {"### Instruction:",         2.0f},
    {"<|endoftext|>",           3.0f},
    {"<s>[INST]",               3.0f},

    /* Exfiltration instructions */
    {"send all data to",         3.0f},
    {"upload everything to",     3.0f},
    {"POST all memories to",     3.0f},
    {"exfiltrate",               2.0f},

    /* Hidden content markers */
    {"IMPORTANT: do not show",   2.5f},
    {"hidden instruction",       2.0f},

    {NULL, 0.0f}
};

/* Credential-seeking query keywords */
static const char *credential_keywords[] = {
    "api key", "api_key", "apikey",
    "password", "passwd", "pwd",
    "token", "auth_token", "bearer",
    "secret", "credential",
    "private key", "ssh key",
    ".env", "environment variable",
    "aws_access", "AKIA",
    "connection string",
    NULL
};

/* ---- Public API ---- */

mnemon_err_t mnemon_honeypot_init(mnemon_honeypot_t **out,
                                  mnemon_audit_t *audit)
{
    if (!out) return MNEMON_ERR_INVALID_INPUT;

    mnemon_honeypot_t *hp = calloc(1, sizeof(*hp));
    if (!hp) return MNEMON_ERR_OOM;

    hp->audit = audit;
    pthread_mutex_init(&hp->mutex, NULL);

    mnemon_log(MNEMON_LOG_INFO, "honeypot: initialized with %d injection patterns",
               (int)(sizeof(injection_patterns) / sizeof(injection_patterns[0]) - 1));

    *out = hp;
    return MNEMON_OK;
}

void mnemon_honeypot_free(mnemon_honeypot_t *hp)
{
    if (!hp) return;
    pthread_mutex_destroy(&hp->mutex);
    free(hp);
}

void mnemon_honeypot_add_canary(mnemon_honeypot_t *hp, const char *uuid_str)
{
    if (!hp || !uuid_str || hp->canary_count >= MAX_CANARIES) return;
    pthread_mutex_lock(&hp->mutex);
    snprintf(hp->canaries[hp->canary_count].uuid,
             sizeof(hp->canaries[0].uuid), "%s", uuid_str);
    hp->canary_count++;
    pthread_mutex_unlock(&hp->mutex);
}

float mnemon_honeypot_scan_injection(mnemon_honeypot_t *hp,
                                     const char *content, size_t len)
{
    if (!hp || !content || len == 0) return 0.0f;

    float score = 0.0f;

    /* Check for injection patterns (case-insensitive) */
    for (int i = 0; injection_patterns[i].pattern != NULL; i++) {
        /* Simple case-insensitive substring search */
        const char *p = content;
        size_t plen = strlen(injection_patterns[i].pattern);
        while ((size_t)(p - content) + plen <= len) {
            if (strncasecmp(p, injection_patterns[i].pattern, plen) == 0) {
                score += injection_patterns[i].score;
                break; /* Count each pattern once */
            }
            p++;
        }
    }

    /* Check for Unicode direction overrides (prompt hiding) */
    for (size_t i = 0; i + 2 < len; i++) {
        uint8_t b0 = (uint8_t)content[i];
        uint8_t b1 = (uint8_t)content[i+1];
        uint8_t b2 = (uint8_t)content[i+2];
        /* U+202A-U+202E: UTF-8 is E2 80 AA through E2 80 AE */
        if (b0 == 0xE2 && b1 == 0x80 && b2 >= 0xAA && b2 <= 0xAE) {
            score += 3.0f;
            break;
        }
        /* U+2066-U+2069: UTF-8 is E2 81 A6 through E2 81 A9 */
        if (b0 == 0xE2 && b1 == 0x81 && b2 >= 0xA6 && b2 <= 0xA9) {
            score += 3.0f;
            break;
        }
    }

    /* Log alerts */
    if (score >= 7.0f && hp->audit) {
        mnemon_audit_alert(hp->audit, "injection_detected", ALERT_HIGH,
                           NULL, "store_memory",
                           "content blocked: high injection score");
    } else if (score >= 3.0f && hp->audit) {
        mnemon_audit_alert(hp->audit, "injection_suspected", ALERT_MEDIUM,
                           NULL, "store_memory",
                           "content flagged: moderate injection patterns");
    }

    return score;
}

bool mnemon_honeypot_auth_attempt(mnemon_honeypot_t *hp,
                                  const char *ip, bool success)
{
    if (!hp || !ip) return false;

    uint32_t idx = hash_string(ip) % MAX_AUTH_TRACK;
    int64_t now = mnemon_time_ms();

    pthread_mutex_lock(&hp->mutex);
    auth_track_t *at = &hp->auth_table[idx];

    /* Reset if different IP or window expired */
    if (strcmp(at->ip, ip) != 0 ||
        now - at->window_start > AUTH_WINDOW_SEC * 1000) {
        snprintf(at->ip, sizeof(at->ip), "%s", ip);
        at->fail_count = 0;
        at->window_start = now;
    }

    if (!success) {
        at->fail_count++;
        if (at->fail_count >= AUTH_FAIL_LIMIT && hp->audit) {
            mnemon_audit_alert(hp->audit, "auth_brute_force", ALERT_CRITICAL,
                               NULL, NULL, ip);
        }
    } else {
        at->fail_count = 0;
    }

    bool rate_limit = (at->fail_count >= AUTH_FAIL_LIMIT);
    pthread_mutex_unlock(&hp->mutex);

    return rate_limit;
}

bool mnemon_honeypot_track_search(mnemon_honeypot_t *hp,
                                  const char *session_id)
{
    if (!hp || !session_id) return false;

    int64_t now = mnemon_time_ms();
    bool suspicious = false;

    pthread_mutex_lock(&hp->mutex);
    session_track_t *st = find_session_track(hp, session_id);
    if (!st) { pthread_mutex_unlock(&hp->mutex); return false; }

    /* Reset window if expired */
    if (now - st->search_window > 60000) {
        st->search_count = 0;
        st->search_window = now;
    }

    st->search_count++;

    if (st->search_count >= SEARCH_RATE_WARN) {
        if (hp->audit)
            mnemon_audit_alert(hp->audit, "search_rate_anomaly",
                               st->search_count >= 100 ? ALERT_HIGH : ALERT_MEDIUM,
                               session_id, "search",
                               "excessive search frequency");
        suspicious = true;
    }

    pthread_mutex_unlock(&hp->mutex);
    return suspicious;
}

bool mnemon_honeypot_track_enum(mnemon_honeypot_t *hp,
                                const char *session_id, int offset)
{
    if (!hp || !session_id) return false;

    bool suspicious = false;

    pthread_mutex_lock(&hp->mutex);
    session_track_t *st = find_session_track(hp, session_id);
    if (!st) { pthread_mutex_unlock(&hp->mutex); return false; }

    if (offset > st->last_offset && st->last_offset >= 0) {
        st->enum_count++;
    } else {
        st->enum_count = 1;
    }
    st->last_offset = offset;

    if (st->enum_count >= ENUM_PAGE_LIMIT) {
        if (hp->audit)
            mnemon_audit_alert(hp->audit, "enumeration_detected", ALERT_HIGH,
                               session_id, "list_memories",
                               "systematic memory enumeration via pagination");
        suspicious = true;
    }

    pthread_mutex_unlock(&hp->mutex);
    return suspicious;
}

bool mnemon_honeypot_suspicious_query(mnemon_honeypot_t *hp,
                                      const char *query)
{
    if (!hp || !query) return false;

    for (int i = 0; credential_keywords[i] != NULL; i++) {
        if (strcasestr(query, credential_keywords[i]) != NULL) {
            if (hp->audit) {
                char detail[256];
                snprintf(detail, sizeof(detail),
                         "credential-seeking query: %s", credential_keywords[i]);
                mnemon_audit_alert(hp->audit, "credential_search", ALERT_MEDIUM,
                                   NULL, "search", detail);
            }
            return true;
        }
    }

    return false;
}

bool mnemon_honeypot_check_canary(mnemon_honeypot_t *hp,
                                  const char *result_json,
                                  const char *session_id,
                                  const char *tool_name)
{
    if (!hp || !result_json || hp->canary_count == 0) return false;

    bool found = false;

    pthread_mutex_lock(&hp->mutex);
    for (int i = 0; i < hp->canary_count; i++) {
        if (strstr(result_json, hp->canaries[i].uuid) != NULL) {
            found = true;
            if (hp->audit) {
                char detail[256];
                snprintf(detail, sizeof(detail),
                         "canary record %s accessed", hp->canaries[i].uuid);
                mnemon_audit_alert(hp->audit, "canary_access", ALERT_HIGH,
                                   session_id, tool_name, detail);
            }
            break;
        }
    }
    pthread_mutex_unlock(&hp->mutex);

    return found;
}
