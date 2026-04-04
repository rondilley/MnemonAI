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
static char *sanitize_query(const char *input)
{
    if (!input || !input[0]) return NULL;

    size_t len = strlen(input);
    if (len > 10000) len = 10000;  /* cap query length to prevent abuse */
    /* Worst case: every char is '"' (doubled) + 2 quotes per word + " OR " separators.
     * Budget: len*2 (doubled quotes) + len (OR separators) + len (quotes) + 1 */
    char *buf = malloc(len * 4 + len + 1);
    if (!buf) return NULL;

    size_t pos = 0;
    const char *p = input;
    bool first = true;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        const char *word_start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        size_t wlen = (size_t)(p - word_start);

        if (!first) { buf[pos++] = ' '; buf[pos++] = 'O'; buf[pos++] = 'R'; buf[pos++] = ' '; }
        buf[pos++] = '"';
        for (size_t i = 0; i < wlen; i++) {
            if (word_start[i] == '"') { buf[pos++] = '"'; buf[pos++] = '"'; }
            else buf[pos++] = word_start[i];
        }
        buf[pos++] = '"';
        first = false;
    }
    buf[pos] = '\0';
    return buf;
}

mnemon_err_t mnemon_fts_search(mnemon_fts_t *f, const char *query, int top_k,
                               mnemon_fts_results_t *out)
{
    sqlite3_stmt *stmt;
    int rc;

    if (!f || !query || !out) return MNEMON_ERR_INVALID_INPUT;
    memset(out, 0, sizeof(*out));

    char *sq = sanitize_query(query);
    if (!sq) return MNEMON_OK;  /* empty/invalid query -> zero results */

    rc = sqlite3_prepare_v2(f->db,
        "SELECT m.uuid, m.source_type, f.rank "
        "FROM memory_fts f "
        "JOIN fts_id_map m ON f.rowid = m.rowid "
        "WHERE memory_fts MATCH ? "
        "ORDER BY f.rank "
        "LIMIT ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) { free(sq); return MNEMON_ERR_SQLITE; }

    sqlite3_bind_text(stmt, 1, sq, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, top_k > 0 ? top_k : 10);

    out->results = calloc((size_t)(top_k > 0 ? top_k : 10),
                          sizeof(mnemon_fts_result_t));
    if (!out->results) { sqlite3_finalize(stmt); free(sq); return MNEMON_ERR_OOM; }

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
    out->count = count;

    sqlite3_finalize(stmt);
    free(sq);
    return MNEMON_OK;
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
