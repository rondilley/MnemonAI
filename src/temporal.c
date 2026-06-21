/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * temporal.c -- Bi-temporal query logic
 *
 * Phase 2: Time-filtered queries by scanning LMDB memories/entities
 * and filtering by created_at timestamps. Full bi-temporal point queries
 * with the temporal index are available for entity history.
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

#include <lmdb.h>

#include "temporal.h"
#include "graph.h"
#include "id.h"
#include "memory.h"
#include "log.h"

#define MAX_TEMPORAL_RESULTS 50

mnemon_err_t mnemon_search_temporal(mnemon_storage_t *s,
                                    const uint8_t *entity_id,
                                    int64_t since, int64_t until,
                                    int top_k,
                                    mnemon_result_set_t *out)
{
    (void)entity_id; /* filter by entity association is a future enhancement */

    if (!s || !out)
        return MNEMON_ERR_INVALID_INPUT;

    memset(out, 0, sizeof(*out));
    if (top_k <= 0) top_k = 10;
    if (top_k > MAX_TEMPORAL_RESULTS) top_k = MAX_TEMPORAL_RESULTS;

    /* Scan memories from LMDB, filter by created_at timestamp */
    mnemon_graph_t *graph = mnemon_storage_graph(s);
    MDB_txn *txn;
    mnemon_err_t err = mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn);
    if (err != MNEMON_OK)
        return err;

    MDB_dbi dbi;
    int rc = mdb_dbi_open(txn, "memories", 0, &dbi);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        return MNEMON_OK; /* No memories DB yet */
    }

    MDB_cursor *cur;
    rc = mdb_cursor_open(txn, dbi, &cur);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        return MNEMON_ERR_LMDB;
    }

    /* Collect matching memories into a temp array, sorted by created_at desc */
    typedef struct { uint8_t id[16]; int64_t created_at; } temp_t;
    size_t cap = 256;
    temp_t *temps = malloc(cap * sizeof(temp_t));
    if (!temps) {
        mdb_cursor_close(cur);
        mnemon_graph_txn_abort(txn);
        return MNEMON_ERR_OOM;
    }
    int count = 0;

    MDB_val key, val;
    rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
    while (rc == 0) {
        mnemon_memory_t mem = {0};
        mnemon_graph_get_memory(graph, txn, key.mv_data, &mem);

        bool matches = true;
        if (since > 0 && mem.created_at < since) matches = false;
        if (until > 0 && mem.created_at > until) matches = false;

        if (matches) {
            if ((size_t)count >= cap) {
                cap *= 2;
                temp_t *p = realloc(temps, cap * sizeof(temp_t));
                if (!p) { mnemon_memory_free(&mem); break; }
                temps = p;
            }
            memcpy(temps[count].id, mem.id, 16);
            temps[count].created_at = mem.created_at;
            count++;
        }

        mnemon_memory_free(&mem);
        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
    }

    mdb_cursor_close(cur);

    /* Sort by created_at descending */
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (temps[j].created_at > temps[i].created_at) {
                temp_t tmp = temps[i]; temps[i] = temps[j]; temps[j] = tmp;
            }

    /* Build result set */
    int n = count < top_k ? count : top_k;
    out->results = calloc((size_t)n, sizeof(mnemon_result_t));
    if (!out->results) {
        free(temps);
        mnemon_graph_txn_abort(txn);
        return MNEMON_ERR_OOM;
    }

    for (int i = 0; i < n; i++) {
        mnemon_result_t *r = &out->results[i];
        mnemon_uuid_t u;
        memcpy(u.bytes, temps[i].id, 16);
        mnemon_uuid_to_string(&u, r->id, sizeof(r->id));

        mnemon_memory_t mem = {0};
        if (mnemon_graph_get_memory(graph, txn, temps[i].id, &mem) == MNEMON_OK) {
            r->content = mem.content ? strdup(mem.content) : strdup("");
            switch (mem.tier) {
            case MNEMON_TIER_EPISODIC:   r->tier = strdup("episodic"); break;
            case MNEMON_TIER_SEMANTIC:   r->tier = strdup("semantic"); break;
            case MNEMON_TIER_PROCEDURAL: r->tier = strdup("procedural"); break;
            }
            r->score = (float)temps[i].created_at / 1e12f; /* normalize for display */
            mnemon_memory_free(&mem);
        } else {
            r->content = strdup("");
            r->tier = strdup("unknown");
        }
    }
    out->count = n;
    out->truncated = (count > top_k);

    free(temps);
    mnemon_graph_txn_abort(txn);
    return MNEMON_OK;
}

mnemon_err_t mnemon_get_state_at_time(mnemon_storage_t *s,
                                      const uint8_t entity_id[16],
                                      int64_t timestamp,
                                      mnemon_entity_t *out)
{
    if (!s || !entity_id || !out)
        return MNEMON_ERR_INVALID_INPUT;

    memset(out, 0, sizeof(*out));

    mnemon_graph_t *graph = mnemon_storage_graph(s);
    MDB_txn *txn;
    if (mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn) != MNEMON_OK)
        return MNEMON_ERR_LMDB;

    /* Point-in-time: the snapshot with the greatest valid_from <= timestamp.
     * load_versions returns ascending order, so that is the last in range. */
    mnemon_entity_t *vers = NULL;
    uint32_t n = 0;
    mnemon_graph_load_versions(graph, txn, entity_id, 0, timestamp, &vers, &n);

    if (n > 0) {
        *out = vers[n - 1];                 /* transfer ownership of newest */
        for (uint32_t i = 0; i + 1 < n; i++)
            mnemon_entity_free(&vers[i]);
        free(vers);
        mnemon_graph_txn_abort(txn);
        return MNEMON_OK;
    }
    free(vers);

    /* No version history (e.g. legacy entity stored before versioning): fall
     * back to current state, gated on creation time. */
    mnemon_err_t err = mnemon_graph_get_entity(graph, txn, entity_id, out);
    mnemon_graph_txn_abort(txn);
    if (err != MNEMON_OK)
        return err;
    if (out->created_at > timestamp) {
        mnemon_entity_free(out);
        memset(out, 0, sizeof(*out));
        return MNEMON_ERR_NOT_FOUND;
    }
    return MNEMON_OK;
}

mnemon_err_t mnemon_get_history(mnemon_storage_t *s,
                                const uint8_t entity_id[16],
                                int64_t since, int64_t until,
                                mnemon_version_list_t *out)
{
    if (!s || !entity_id || !out)
        return MNEMON_ERR_INVALID_INPUT;

    memset(out, 0, sizeof(*out));

    mnemon_graph_t *graph = mnemon_storage_graph(s);
    MDB_txn *txn;
    if (mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn) != MNEMON_OK)
        return MNEMON_ERR_LMDB;

    /* All snapshots in the time window, ascending by valid_from. */
    mnemon_entity_t *vers = NULL;
    uint32_t n = 0;
    mnemon_graph_load_versions(graph, txn, entity_id, since, until, &vers, &n);

    if (n > 0) {
        out->versions = vers;
        out->count = n;
        mnemon_graph_txn_abort(txn);
        return MNEMON_OK;
    }
    free(vers);

    /* No recorded history (legacy entity): fall back to current state as a
     * single version, honoring the time filter. */
    mnemon_entity_t current;
    memset(&current, 0, sizeof(current));
    mnemon_err_t err = mnemon_graph_get_entity(graph, txn, entity_id, &current);
    mnemon_graph_txn_abort(txn);
    if (err != MNEMON_OK)
        return err;

    if ((since > 0 && current.updated_at < since) ||
        (until > 0 && current.created_at > until)) {
        mnemon_entity_free(&current);
        return MNEMON_OK; /* empty within window */
    }

    out->versions = calloc(1, sizeof(mnemon_entity_t));
    if (!out->versions) {
        mnemon_entity_free(&current);
        return MNEMON_ERR_OOM;
    }
    out->versions[0] = current; /* transfer ownership */
    out->count = 1;
    return MNEMON_OK;
}

mnemon_err_t mnemon_get_changes_since(mnemon_storage_t *s,
                                      int64_t since,
                                      const char *entity_type,
                                      int top_k,
                                      mnemon_result_set_t *out)
{
    if (!s || !out)
        return MNEMON_ERR_INVALID_INPUT;

    memset(out, 0, sizeof(*out));
    if (top_k <= 0) top_k = 50;
    if (top_k > MAX_TEMPORAL_RESULTS) top_k = MAX_TEMPORAL_RESULTS;

    /* Scan entities from LMDB, filter by updated_at > since */
    mnemon_graph_t *graph = mnemon_storage_graph(s);
    MDB_txn *txn;
    mnemon_err_t err = mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn);
    if (err != MNEMON_OK)
        return err;

    MDB_dbi dbi;
    int rc = mdb_dbi_open(txn, "entities", 0, &dbi);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        return MNEMON_OK;
    }

    MDB_cursor *cur;
    rc = mdb_cursor_open(txn, dbi, &cur);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        return MNEMON_ERR_LMDB;
    }

    out->results = calloc((size_t)top_k, sizeof(mnemon_result_t));
    if (!out->results) {
        mdb_cursor_close(cur);
        mnemon_graph_txn_abort(txn);
        return MNEMON_ERR_OOM;
    }

    int count = 0;
    MDB_val key, val;
    rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
    while (rc == 0 && count < top_k) {
        mnemon_entity_t ent = {0};
        mnemon_graph_get_entity(graph, txn, key.mv_data, &ent);

        bool matches = true;
        if (since > 0 && ent.updated_at < since) matches = false;
        if (entity_type && ent.entity_type &&
            strcmp(ent.entity_type, entity_type) != 0) matches = false;

        if (matches) {
            mnemon_result_t *r = &out->results[count];
            mnemon_uuid_t u;
            memcpy(u.bytes, ent.id, 16);
            mnemon_uuid_to_string(&u, r->id, sizeof(r->id));
            r->content = ent.name ? strdup(ent.name) : strdup("");
            r->tier = ent.entity_type ? strdup(ent.entity_type) : strdup("");
            r->score = (float)ent.updated_at / 1e12f;
            count++;
        }

        mnemon_entity_free(&ent);
        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
    }

    out->count = count;
    out->truncated = (rc == 0); /* More entries exist */

    mdb_cursor_close(cur);
    mnemon_graph_txn_abort(txn);
    return MNEMON_OK;
}

/* ================================================================== */
/* Natural language date parsing + event extraction                     */
/* ================================================================== */

static const char *month_names[] = {
    "january", "february", "march", "april", "may", "june",
    "july", "august", "september", "october", "november", "december"
};
static const char *month_abbrevs[] = {
    "jan", "feb", "mar", "apr", "may", "jun",
    "jul", "aug", "sep", "oct", "nov", "dec"
};

/* Match a month name at position p (case-insensitive).
 * Returns month 1-12, or 0 if no match. Sets *advance to chars consumed. */
static int match_month(const char *p, int *advance)
{
    for (int i = 0; i < 12; i++) {
        size_t flen = strlen(month_names[i]);
        if (strncasecmp(p, month_names[i], flen) == 0 &&
            !isalpha((unsigned char)p[flen])) {
            *advance = (int)flen;
            return i + 1;
        }
    }
    for (int i = 0; i < 12; i++) {
        size_t alen = strlen(month_abbrevs[i]);
        if (strncasecmp(p, month_abbrevs[i], alen) == 0 &&
            !isalpha((unsigned char)p[alen])) {
            *advance = (int)alen;
            return i + 1;
        }
    }
    return 0;
}

int64_t mnemon_parse_natural_date(const char *str, int context_year)
{
    if (!str) return 0;

    /* Try ISO8601 first */
    int64_t iso = mnemon_parse_iso8601(str);
    if (iso > 0) return iso;

    /* Try: "Month Day[st/nd/rd/th][, Year]" */
    const char *p = str;
    while (*p && isspace((unsigned char)*p)) p++;

    int adv = 0;
    int month = match_month(p, &adv);
    if (month == 0) return 0;
    p += adv;

    /* Skip spaces */
    while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;

    /* Parse day */
    int day = 0;
    if (!isdigit((unsigned char)*p)) return 0;
    day = (int)strtol(p, (char **)&p, 10);
    if (day < 1 || day > 31) return 0;

    /* Skip ordinal suffix (st, nd, rd, th) */
    if ((*p == 's' && *(p+1) == 't') ||
        (*p == 'n' && *(p+1) == 'd') ||
        (*p == 'r' && *(p+1) == 'd') ||
        (*p == 't' && *(p+1) == 'h'))
        p += 2;

    /* Skip comma, spaces */
    while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;

    /* Parse optional year */
    int year = context_year;
    if (isdigit((unsigned char)*p)) {
        int y = (int)strtol(p, (char **)&p, 10);
        if (y >= 1900 && y <= 2100) year = y;
    }
    if (year <= 0) return 0;

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = year - 1900;
    tm.tm_mon  = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12; /* Noon to avoid timezone edge cases */

    time_t t = timegm(&tm);
    if (t == (time_t)-1) return 0;
    return (int64_t)t * 1000;
}

int mnemon_duration_days(int64_t from_ms, int64_t to_ms)
{
    if (from_ms <= 0 || to_ms <= 0) return -1;
    int64_t diff = to_ms - from_ms;
    if (diff < 0) diff = -diff;
    return (int)(diff / (86400LL * 1000));
}

void mnemon_event_list_free(mnemon_event_list_t *list)
{
    if (!list) return;
    for (int i = 0; i < list->count; i++)
        free(list->events[i].description);
    free(list->events);
    memset(list, 0, sizeof(*list));
}

/* True if p points at a bare ISO calendar date "YYYY-MM-DD". Short-circuits on
 * the NUL terminator, so it never reads past the end of the string. */
static bool looks_like_iso_date(const char *p)
{
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (p[i] != '-') return false;
        } else if (!isdigit((unsigned char)p[i])) {
            return false;
        }
    }
    return true;
}

mnemon_err_t mnemon_extract_events(const char *text, int context_year,
                                   mnemon_event_list_t *out)
{
    if (!text || !out) return MNEMON_ERR_INVALID_INPUT;
    memset(out, 0, sizeof(*out));

    /* Scan text for date patterns and extract surrounding context */
    int cap = 32;
    out->events = calloc((size_t)cap, sizeof(mnemon_event_t));
    if (!out->events) return MNEMON_ERR_OOM;

    const char *p = text;
    size_t len = strlen(text);

    while (*p) {
        int adv = 0;
        int64_t date = 0;

        /* ISO date (2026-06-10) takes priority -- match_month never sees a
         * digit, so without this branch ISO dates were silently skipped. */
        if (isdigit((unsigned char)*p) && looks_like_iso_date(p)) {
            char buf[11];
            memcpy(buf, p, 10);
            buf[10] = '\0';
            date = mnemon_parse_iso8601(buf);
            adv = 10;
        } else {
            /* Otherwise try to match a natural-language month name here. */
            int month = match_month(p, &adv);
            if (month == 0) {
                p++;
                continue;
            }
            date = mnemon_parse_natural_date(p, context_year);
        }

        if (date <= 0) {
            p += (adv > 0 ? adv : 1);
            continue;
        }

        /* Extract context: ~80 chars before and after the date mention */
        size_t pos = (size_t)(p - text);
        size_t ctx_start = (pos > 80) ? pos - 80 : 0;
        size_t ctx_end = pos + 80;
        if (ctx_end > len) ctx_end = len;

        /* Find sentence boundaries for cleaner context */
        while (ctx_start > 0 && text[ctx_start] != '.' &&
               text[ctx_start] != '\n')
            ctx_start--;
        if (text[ctx_start] == '.' || text[ctx_start] == '\n')
            ctx_start++;
        while (ctx_end < len && text[ctx_end] != '.' &&
               text[ctx_end] != '\n')
            ctx_end++;

        /* Skip leading whitespace */
        while (ctx_start < ctx_end &&
               isspace((unsigned char)text[ctx_start]))
            ctx_start++;

        size_t desc_len = ctx_end - ctx_start;
        if (desc_len > 0 && desc_len < 500) {
            if (out->count >= cap) {
                cap *= 2;
                mnemon_event_t *n = realloc(
                    out->events, (size_t)cap * sizeof(mnemon_event_t));
                if (!n) break;
                out->events = n;
            }
            out->events[out->count].description = strndup(
                text + ctx_start, desc_len);
            out->events[out->count].event_date = date;
            out->count++;
        }

        /* Advance past this date to avoid re-matching */
        p += adv;
        /* Skip past the day number */
        while (*p && !isalpha((unsigned char)*p) && *p != '\n') p++;
    }

    return MNEMON_OK;
}

mnemon_err_t mnemon_search_events(mnemon_storage_t *s,
                                  int64_t since, int64_t until,
                                  const char *name_filter,
                                  int top_k,
                                  mnemon_result_set_t *out)
{
    if (!s || !out) return MNEMON_ERR_INVALID_INPUT;
    memset(out, 0, sizeof(*out));
    if (top_k <= 0) top_k = 20;
    if (top_k > MAX_TEMPORAL_RESULTS) top_k = MAX_TEMPORAL_RESULTS;

    mnemon_graph_t *graph = mnemon_storage_graph(s);
    MDB_txn *txn;
    mnemon_err_t err = mnemon_graph_txn_begin(graph, MDB_RDONLY, &txn);
    if (err != MNEMON_OK) return err;

    MDB_dbi dbi;
    int rc = mdb_dbi_open(txn, "entities", 0, &dbi);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        return MNEMON_OK;
    }

    MDB_cursor *cur;
    rc = mdb_cursor_open(txn, dbi, &cur);
    if (rc != 0) {
        mnemon_graph_txn_abort(txn);
        return MNEMON_ERR_LMDB;
    }

    /* Collect entities with event_date in range */
    typedef struct { uint8_t id[16]; int64_t event_date; } evt_t;
    evt_t *evts = calloc((size_t)top_k * 2, sizeof(evt_t));
    if (!evts) {
        mdb_cursor_close(cur);
        mnemon_graph_txn_abort(txn);
        return MNEMON_ERR_OOM;
    }
    int count = 0;

    MDB_val key, val;
    rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
    while (rc == 0) {
        mnemon_entity_t ent = {0};
        mnemon_graph_get_entity(graph, txn, key.mv_data, &ent);

        if (ent.event_date > 0) {
            bool matches = true;
            if (since > 0 && ent.event_date < since) matches = false;
            if (until > 0 && ent.event_date > until) matches = false;
            if (name_filter && name_filter[0] &&
                ent.name &&
                !strcasestr(ent.name, name_filter))
                matches = false;

            if (matches && count < top_k * 2) {
                memcpy(evts[count].id, ent.id, 16);
                evts[count].event_date = ent.event_date;
                count++;
            }
        }
        mnemon_entity_free(&ent);
        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
    }
    mdb_cursor_close(cur);

    /* Sort by event_date ascending (chronological) */
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (evts[j].event_date < evts[i].event_date) {
                evt_t tmp = evts[i]; evts[i] = evts[j]; evts[j] = tmp;
            }

    int n = count < top_k ? count : top_k;
    out->results = calloc((size_t)n, sizeof(mnemon_result_t));
    if (!out->results) {
        free(evts);
        mnemon_graph_txn_abort(txn);
        return MNEMON_ERR_OOM;
    }

    for (int i = 0; i < n; i++) {
        mnemon_result_t *r = &out->results[i];
        mnemon_uuid_t u;
        memcpy(u.bytes, evts[i].id, 16);
        mnemon_uuid_to_string(&u, r->id, sizeof(r->id));

        mnemon_entity_t ent = {0};
        if (mnemon_graph_get_entity(graph, txn, evts[i].id, &ent) == MNEMON_OK) {
            /* Build content: name + event_date + observations */
            char date_buf[32];
            mnemon_format_iso8601(ent.event_date, date_buf, sizeof(date_buf));
            size_t clen = 256;
            for (uint32_t j = 0; j < ent.observation_count; j++)
                clen += strlen(ent.observations[j]) + 2;
            char *content = malloc(clen);
            if (content) {
                int off = snprintf(content, clen, "%s [date: %s]",
                                   ent.name ? ent.name : "", date_buf);
                for (uint32_t j = 0; j < ent.observation_count; j++)
                    off += snprintf(content + off, clen - (size_t)off,
                                    "\n  %s", ent.observations[j]);
                r->content = content;
            } else {
                r->content = strdup(ent.name ? ent.name : "");
            }
            r->tier = ent.entity_type ? strdup(ent.entity_type) : strdup("event");
            r->score = (float)ent.event_date / 1e12f;
            mnemon_entity_free(&ent);
        } else {
            r->content = strdup("");
            r->tier = strdup("event");
        }
    }
    out->count = n;
    out->truncated = (count > top_k);

    free(evts);
    mnemon_graph_txn_abort(txn);
    return MNEMON_OK;
}
