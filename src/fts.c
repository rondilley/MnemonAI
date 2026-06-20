/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * fts.c -- SQLite FTS5 full-text search wrapper
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

#include <sqlite3.h>

#include "fts.h"
#include "log.h"

struct mnemon_fts {
    sqlite3 *db;
    char    *db_path;
    int64_t  next_rowid;
};

static const char *SCHEMA_SQL =
    "CREATE VIRTUAL TABLE IF NOT EXISTS memory_fts USING fts5("
    "  content, name, entity_type, observations,"
    "  source_type, source_author, tags,"
    "  tokenize='porter unicode61'"
    ");"
    "CREATE TABLE IF NOT EXISTS fts_id_map ("
    "  rowid INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  uuid BLOB NOT NULL,"
    "  source_type INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_fts_uuid ON fts_id_map(uuid);";

mnemon_err_t mnemon_fts_open(mnemon_fts_t **out, const char *db_path)
{
    mnemon_fts_t *f;
    int rc;

    if (!out || !db_path) return MNEMON_ERR_INVALID_INPUT;

    f = calloc(1, sizeof(*f));
    if (!f) return MNEMON_ERR_OOM;
    f->db_path = strdup(db_path);

    rc = sqlite3_open_v2(db_path, &f->db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        mnemon_err_set(MNEMON_ERR_SQLITE, rc, "sqlite3_open: %s",
                       sqlite3_errmsg(f->db));
        sqlite3_close(f->db);
        free(f->db_path);
        free(f);
        return MNEMON_ERR_SQLITE;
    }

    sqlite3_exec(f->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(f->db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(f->db, "PRAGMA wal_autocheckpoint=1000;", NULL, NULL, NULL);

    char *errmsg = NULL;
    rc = sqlite3_exec(f->db, SCHEMA_SQL, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        mnemon_err_set(MNEMON_ERR_SQLITE, rc, "schema: %s",
                       errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        sqlite3_close(f->db);
        free(f->db_path);
        free(f);
        return MNEMON_ERR_SQLITE;
    }

    /* Get next rowid */
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(f->db,
        "SELECT COALESCE(MAX(rowid),0)+1 FROM fts_id_map", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            f->next_rowid = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (f->next_rowid < 1) f->next_rowid = 1;

    *out = f;
    return MNEMON_OK;
}

void mnemon_fts_close(mnemon_fts_t *f)
{
    if (!f) return;
    if (f->db) sqlite3_close(f->db);
    free(f->db_path);
    free(f);
}

static char *join_strings(char **arr, uint32_t count, const char *sep)
{
    if (!arr || count == 0) return strdup("");
    size_t total = 0;
    size_t seplen = strlen(sep);
    for (uint32_t i = 0; i < count; i++)
        total += (arr[i] ? strlen(arr[i]) : 0) + seplen;
    char *buf = malloc(total + 1);
    if (!buf) return strdup("");
    buf[0] = '\0';
    for (uint32_t i = 0; i < count; i++) {
        if (i > 0) strcat(buf, sep);
        if (arr[i]) strcat(buf, arr[i]);
    }
    return buf;
}

mnemon_err_t mnemon_fts_index_memory(mnemon_fts_t *f, const mnemon_memory_t *mem)
{
    sqlite3_stmt *stmt;
    int rc;
    int64_t rid = f->next_rowid++;

    /* Insert into fts_id_map */
    rc = sqlite3_prepare_v2(f->db,
        "INSERT INTO fts_id_map(rowid, uuid, source_type) VALUES(?,?,0)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MNEMON_ERR_SQLITE;
    sqlite3_bind_int64(stmt, 1, rid);
    sqlite3_bind_blob(stmt, 2, mem->id, 16, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Insert into FTS5 */
    char *tags = join_strings(mem->tags, mem->tag_count, " ");
    rc = sqlite3_prepare_v2(f->db,
        "INSERT INTO memory_fts(rowid, content, name, entity_type, "
        "observations, source_type, source_author, tags) "
        "VALUES(?,?,?,?,?,?,?,?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) { free(tags); return MNEMON_ERR_SQLITE; }

    sqlite3_bind_int64(stmt, 1, rid);
    sqlite3_bind_text(stmt, 2, mem->content ? mem->content : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, mem->source_type ? mem->source_type : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, mem->source_author ? mem->source_author : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, tags, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(tags);

    return (rc == SQLITE_DONE) ? MNEMON_OK : MNEMON_ERR_SQLITE;
}

mnemon_err_t mnemon_fts_index_entity(mnemon_fts_t *f, const mnemon_entity_t *e)
{
    sqlite3_stmt *stmt;
    int rc;
    int64_t rid = f->next_rowid++;

    rc = sqlite3_prepare_v2(f->db,
        "INSERT INTO fts_id_map(rowid, uuid, source_type) VALUES(?,?,1)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MNEMON_ERR_SQLITE;
    sqlite3_bind_int64(stmt, 1, rid);
    sqlite3_bind_blob(stmt, 2, e->id, 16, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    char *obs = join_strings(e->observations, e->observation_count, " ");
    rc = sqlite3_prepare_v2(f->db,
        "INSERT INTO memory_fts(rowid, content, name, entity_type, "
        "observations, source_type, source_author, tags) "
        "VALUES(?,?,?,?,?,?,?,?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) { free(obs); return MNEMON_ERR_SQLITE; }

    sqlite3_bind_int64(stmt, 1, rid);
    sqlite3_bind_text(stmt, 2, "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, e->name ? e->name : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, e->entity_type ? e->entity_type : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, obs, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, "", -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(obs);

    return (rc == SQLITE_DONE) ? MNEMON_OK : MNEMON_ERR_SQLITE;
}

mnemon_err_t mnemon_fts_remove(mnemon_fts_t *f, const uint8_t id[16], int source_type)
{
    sqlite3_stmt *stmt;
    int rc;

    /* Find rowid */
    rc = sqlite3_prepare_v2(f->db,
        "SELECT rowid FROM fts_id_map WHERE uuid=? AND source_type=?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MNEMON_ERR_SQLITE;
    sqlite3_bind_blob(stmt, 1, id, 16, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, source_type);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return MNEMON_ERR_NOT_FOUND;
    }
    int64_t rid = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    /* Delete from FTS5 */
    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM memory_fts WHERE rowid=%lld",
             (long long)rid);
    sqlite3_exec(f->db, sql, NULL, NULL, NULL);

    /* Delete from id map */
    snprintf(sql, sizeof(sql), "DELETE FROM fts_id_map WHERE rowid=%lld",
             (long long)rid);
    sqlite3_exec(f->db, sql, NULL, NULL, NULL);

    return MNEMON_OK;
}

mnemon_err_t mnemon_fts_update_memory(mnemon_fts_t *f, const mnemon_memory_t *mem)
{
    mnemon_fts_remove(f, mem->id, 0);
    return mnemon_fts_index_memory(f, mem);
}

mnemon_err_t mnemon_fts_update_entity(mnemon_fts_t *f, const mnemon_entity_t *e)
{
    mnemon_fts_remove(f, e->id, 1);
    return mnemon_fts_index_entity(f, e);
}

/* Sanitize FTS5 query: quote each word */
/* FTS5 query tier:
 *   TIER_AND  — all words must be present (highest precision)
 *   TIER_NEAR — words within 10 tokens of each other
 *   TIER_OR   — any word matches (original behavior, lowest precision)
 */
enum fts_tier { TIER_AND, TIER_NEAR, TIER_OR };

/* Common words that add noise to AND/NEAR queries.  Kept short to avoid
 * false positives -- these are the highest-frequency English function words
 * that rarely contribute to retrieval quality in FTS5 BM25. */
static bool is_stopword(const char *w, size_t len)
{
    static const char *stops[] = {
        "a","an","the","is","was","are","were","be","been","am",
        "do","did","does","have","has","had","will","would","shall",
        "should","can","could","may","might","must",
        "i","me","my","we","our","you","your","he","she","it","they",
        "his","her","its","them","their",
        "in","on","at","to","for","of","with","by","from","as",
        "and","or","but","not","no","if","so","that","this","what",
        "which","who","how","when","where","why",
        NULL
    };
    for (const char **s = stops; *s; s++) {
        if (strlen(*s) == len && strncasecmp(*s, w, len) == 0)
            return true;
    }
    return false;
}

static char *sanitize_query(const char *input, enum fts_tier tier)
{
    if (!input || !input[0]) return NULL;

    size_t len = strlen(input);
    if (len > 10000) len = 10000;  /* cap query length to prevent abuse */

    /* Collect non-stopword tokens for AND/NEAR, all tokens for OR */
    /* Budget: len*4 (quoted doubled) + separators + NEAR syntax + 1 */
    char *buf = malloc(len * 5 + 64);
    if (!buf) return NULL;

    /* First pass: extract words, filter stopwords for AND/NEAR */
    typedef struct { const char *start; size_t len; } word_t;
    word_t *words = malloc(len * sizeof(word_t));
    if (!words) { free(buf); return NULL; }
    int nwords = 0;

    const char *p = input;
    while (*p && (size_t)(p - input) < len) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *ws = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        size_t wl = (size_t)(p - ws);
        if (tier == TIER_OR || !is_stopword(ws, wl))
            words[nwords++] = (word_t){ws, wl};
    }

    /* If stopword filtering removed all words, include everything */
    if (nwords == 0 && tier != TIER_OR) {
        free(words);
        free(buf);
        return sanitize_query(input, TIER_OR);
    }
    if (nwords == 0) { free(words); free(buf); return NULL; }

    /* Build FTS5 query string */
    size_t pos = 0;
    const char *sep;
    switch (tier) {
    case TIER_AND:  sep = " AND "; break;
    case TIER_NEAR: sep = " NEAR/10 "; break;
    case TIER_OR:   sep = " OR "; break;
    }
    size_t sep_len = strlen(sep);

    for (int i = 0; i < nwords; i++) {
        if (i > 0) {
            memcpy(buf + pos, sep, sep_len);
            pos += sep_len;
        }
        buf[pos++] = '"';
        for (size_t j = 0; j < words[i].len; j++) {
            if (words[i].start[j] == '"') { buf[pos++] = '"'; buf[pos++] = '"'; }
            else buf[pos++] = words[i].start[j];
        }
        buf[pos++] = '"';
    }
    buf[pos] = '\0';

    free(words);
    return buf;
}

/* Execute a single FTS5 MATCH query at a given tier. Returns result count. */
static int fts_exec_tier(mnemon_fts_t *f, const char *query, enum fts_tier tier,
                         int source_type, int top_k, mnemon_fts_results_t *out)
{
    char *sq = sanitize_query(query, tier);
    if (!sq) return 0;

    /* source_type < 0 = any; otherwise restrict to memories (0) or entities (1)
     * at the SQL level so one document class cannot crowd out the other in the
     * top_k window. */
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(f->db,
        source_type < 0
        ? "SELECT m.uuid, m.source_type, f.rank "
          "FROM memory_fts f "
          "JOIN fts_id_map m ON f.rowid = m.rowid "
          "WHERE memory_fts MATCH ? "
          "ORDER BY f.rank "
          "LIMIT ?"
        : "SELECT m.uuid, m.source_type, f.rank "
          "FROM memory_fts f "
          "JOIN fts_id_map m ON f.rowid = m.rowid "
          "WHERE memory_fts MATCH ? AND m.source_type = ? "
          "ORDER BY f.rank "
          "LIMIT ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) { free(sq); return 0; }

    sqlite3_bind_text(stmt, 1, sq, -1, SQLITE_TRANSIENT);
    if (source_type < 0) {
        sqlite3_bind_int(stmt, 2, top_k);
    } else {
        sqlite3_bind_int(stmt, 2, source_type);
        sqlite3_bind_int(stmt, 3, top_k);
    }

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < top_k) {
        const void *uuid = sqlite3_column_blob(stmt, 0);
        int uuid_len = sqlite3_column_bytes(stmt, 0);
        if (uuid && uuid_len == 16)
            memcpy(out->results[count].id, uuid, 16);
        out->results[count].source_type = sqlite3_column_int(stmt, 1);
        /* FTS5 rank is negative (more negative = better) */
        out->results[count].score = -(float)sqlite3_column_double(stmt, 2);
        count++;
    }

    sqlite3_finalize(stmt);
    free(sq);
    return count;
}

mnemon_err_t mnemon_fts_search_typed(mnemon_fts_t *f, const char *query,
                                     int source_type, int top_k,
                                     mnemon_fts_results_t *out)
{
    if (!f || !query || !out) return MNEMON_ERR_INVALID_INPUT;
    memset(out, 0, sizeof(*out));

    int k = top_k > 0 ? top_k : 10;
    out->results = calloc((size_t)k, sizeof(mnemon_fts_result_t));
    if (!out->results) return MNEMON_ERR_OOM;

    /* Tiered query strategy: AND (precise) -> NEAR/10 -> OR (broad).
     * Stop at the first tier that returns results. */
    int count = fts_exec_tier(f, query, TIER_AND, source_type, k, out);
    if (count == 0)
        count = fts_exec_tier(f, query, TIER_NEAR, source_type, k, out);
    if (count == 0)
        count = fts_exec_tier(f, query, TIER_OR, source_type, k, out);
    out->count = count;

    return MNEMON_OK;
}

mnemon_err_t mnemon_fts_search(mnemon_fts_t *f, const char *query, int top_k,
                               mnemon_fts_results_t *out)
{
    return mnemon_fts_search_typed(f, query, -1, top_k, out);
}

void mnemon_fts_results_free(mnemon_fts_results_t *r)
{
    if (!r) return;
    free(r->results);
    memset(r, 0, sizeof(*r));
}

mnemon_err_t mnemon_fts_checkpoint(mnemon_fts_t *f)
{
    if (!f || !f->db) return MNEMON_ERR_INVALID_INPUT;
    sqlite3_wal_checkpoint_v2(f->db, NULL, SQLITE_CHECKPOINT_TRUNCATE,
                              NULL, NULL);
    return MNEMON_OK;
}

mnemon_err_t mnemon_fts_clear(mnemon_fts_t *f)
{
    if (!f || !f->db) return MNEMON_ERR_INVALID_INPUT;
    sqlite3_exec(f->db, "DELETE FROM memory_fts;", NULL, NULL, NULL);
    sqlite3_exec(f->db, "DELETE FROM fts_id_map;", NULL, NULL, NULL);
    f->next_rowid = 1;
    return MNEMON_OK;
}

mnemon_err_t mnemon_fts_count(mnemon_fts_t *f, size_t *out)
{
    sqlite3_stmt *stmt;
    if (!f || !out) return MNEMON_ERR_INVALID_INPUT;
    *out = 0;
    int rc = sqlite3_prepare_v2(f->db, "SELECT COUNT(*) FROM fts_id_map",
                                -1, &stmt, NULL);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
        *out = (size_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return MNEMON_OK;
}
