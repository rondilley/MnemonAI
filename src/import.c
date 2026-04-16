/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * import.c -- Bulk import: JSONL, CSV, mbox, text/markdown parsers
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
#include <sys/stat.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include <cJSON.h>

#include "import.h"
#include "embed.h"
#include "secret.h"
#include "id.h"
#include "memory.h"
#include "log.h"

#define MAX_FILE_SIZE (100 * 1024 * 1024) /* 100MB */

/* Read entire file into malloc'd buffer */
static char *read_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0 || (size_t)sz > MAX_FILE_SIZE) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }

    *out_len = fread(buf, 1, (size_t)sz, fp);
    buf[*out_len] = '\0';
    fclose(fp);
    return buf;
}

/* Store a single chunk as a memory.
 * created_at_ms: if > 0, use as the created_at timestamp;
 *                if 0, use current wall-clock time. */
static int store_chunk(mnemon_storage_t *s, const char *content,
                       const mnemon_import_opts_t *opts,
                       int64_t created_at_ms)
{
    mnemon_memory_t mem;
    memset(&mem, 0, sizeof(mem));

    mnemon_uuid_t uuid;
    mnemon_uuid_generate(&uuid);
    memcpy(mem.id, uuid.bytes, 16);

    mem.content = strdup(content);
    mem.source_type = strdup(opts->source_type ? opts->source_type : "import");
    mem.source_id = strdup("");
    mem.tier = MNEMON_TIER_EPISODIC;
    mem.importance = 0.5f;
    mem.created_at = (created_at_ms > 0) ? created_at_ms : mnemon_time_ms();
    mem.last_accessed = mem.created_at;

    /* Tags */
    if (opts->tags && opts->tag_count > 0) {
        mem.tag_count = (uint32_t)opts->tag_count;
        mem.tags = calloc(mem.tag_count, sizeof(char *));
        for (int i = 0; i < opts->tag_count; i++)
            mem.tags[i] = strdup(opts->tags[i]);
    }

    /* Embedding */
    mnemon_embed_t *embed = mnemon_storage_embed(s);
    if (embed && mnemon_embed_available(embed)) {
        int dims = mnemon_embed_dimensions(embed);
        mem.embedding = malloc((size_t)dims * sizeof(float));
        if (mem.embedding)
            mnemon_embed_text(embed, content, strlen(content), mem.embedding, dims, false);
    }

    mnemon_err_t err = mnemon_store_memory(s, &mem);
    mnemon_memory_free(&mem);
    return (err == MNEMON_OK) ? 0 : -1;
}

/* ---- Chunking ---- */

mnemon_err_t mnemon_chunk_text(const char *text, size_t len,
                               const char *strategy, int max_chunk_size,
                               mnemon_chunks_t *out)
{
    if (!text || !out) return MNEMON_ERR_INVALID_INPUT;
    memset(out, 0, sizeof(*out));
    if (max_chunk_size <= 0) max_chunk_size = 4096;
    if (!strategy) strategy = "paragraph";

    const char *sep;
    size_t sep_len;

    if (strcmp(strategy, "line") == 0) {
        sep = "\n"; sep_len = 1;
    } else if (strcmp(strategy, "page") == 0) {
        sep = "\f"; sep_len = 1;
    } else if (strcmp(strategy, "none") == 0) {
        /* Entire text as one chunk */
        out->chunks = calloc(1, sizeof(char *));
        if (!out->chunks) return MNEMON_ERR_OOM;
        out->chunks[0] = strndup(text, len);
        out->count = 1;
        return MNEMON_OK;
    } else {
        /* paragraph: split on double newlines */
        sep = "\n\n"; sep_len = 2;
    }

    int cap = 64;
    out->chunks = calloc((size_t)cap, sizeof(char *));
    if (!out->chunks) return MNEMON_ERR_OOM;

    const char *p = text;
    const char *end = text + len;

    while (p < end) {
        const char *next = NULL;

        /* Find next separator */
        for (const char *s = p; s + sep_len <= end; s++) {
            if (memcmp(s, sep, sep_len) == 0) {
                next = s;
                break;
            }
        }

        const char *chunk_end = next ? next : end;
        size_t chunk_len = (size_t)(chunk_end - p);

        /* Skip empty chunks */
        if (chunk_len > 0) {
            /* Split further if over max_chunk_size */
            size_t offset = 0;
            while (offset < chunk_len) {
                size_t piece = chunk_len - offset;
                if (piece > (size_t)max_chunk_size)
                    piece = (size_t)max_chunk_size;

                if (out->count >= cap) {
                    cap *= 2;
                    char **n = realloc(out->chunks, (size_t)cap * sizeof(char *));
                    if (!n) return MNEMON_ERR_OOM;
                    out->chunks = n;
                }

                out->chunks[out->count] = strndup(p + offset, piece);
                out->count++;
                offset += piece;
            }
        }

        p = next ? next + sep_len : end;
    }

    return MNEMON_OK;
}

void mnemon_chunks_free(mnemon_chunks_t *c)
{
    if (!c) return;
    for (int i = 0; i < c->count; i++)
        free(c->chunks[i]);
    free(c->chunks);
    memset(c, 0, sizeof(*c));
}

/* ---- Parsers ---- */

static mnemon_err_t parse_jsonl(mnemon_storage_t *s, const char *data,
                                size_t len, const mnemon_import_opts_t *opts,
                                mnemon_import_result_t *result)
{
    const char *p = data;
    const char *end = data + len;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);

        if (line_len > 0) {
            char *line = strndup(p, line_len);
            cJSON *obj = cJSON_Parse(line);
            free(line);

            if (obj) {
                const cJSON *content = cJSON_GetObjectItemCaseSensitive(obj, "content");
                if (cJSON_IsString(content)) {
                    int64_t ts = 0;
                    if (opts->preserve_timestamps) {
                        const cJSON *ca = cJSON_GetObjectItemCaseSensitive(obj, "created_at");
                        if (cJSON_IsString(ca))
                            ts = mnemon_parse_iso8601(ca->valuestring);
                    }
                    if (store_chunk(s, content->valuestring, opts, ts) == 0)
                        result->imported++;
                    else
                        result->errors++;
                } else {
                    result->skipped++;
                }
                cJSON_Delete(obj);
            } else {
                result->skipped++;
            }
        }

        p = nl ? nl + 1 : end;
    }

    return MNEMON_OK;
}

static mnemon_err_t parse_csv(mnemon_storage_t *s, const char *data,
                              size_t len, const mnemon_import_opts_t *opts,
                              mnemon_import_result_t *result)
{
    /* Find content column in header */
    const char *nl = memchr(data, '\n', len);
    if (!nl) return MNEMON_ERR_INVALID_INPUT;

    /* Simple CSV: each line after header is content */
    const char *p = nl + 1;
    const char *end = data + len;

    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = line_end ? (size_t)(line_end - p) : (size_t)(end - p);

        if (line_len > 0) {
            char *line = strndup(p, line_len);
            if (store_chunk(s, line, opts, 0) == 0)
                result->imported++;
            else
                result->errors++;
            free(line);
        }

        p = line_end ? line_end + 1 : end;
    }

    return MNEMON_OK;
}

/* Parse RFC 2822 Date: header into milliseconds since epoch.
 * Handles common formats like "Tue, 15 Jan 2025 14:30:00 +0000".
 * Returns 0 on failure. */
static int64_t parse_rfc2822_date(const char *str)
{
    if (!str) return 0;

    /* Skip optional day-of-week */
    const char *p = strchr(str, ',');
    if (p) p++;
    else   p = str;
    while (*p == ' ') p++;

    static const char *months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };

    int day = 0, year = 0, hour = 0, min = 0, sec = 0;
    char mon_str[4] = {0};

    if (sscanf(p, "%d %3s %d %d:%d:%d", &day, mon_str, &year, &hour, &min, &sec) < 5)
        return 0;

    int mon = -1;
    for (int i = 0; i < 12; i++) {
        if (strncasecmp(mon_str, months[i], 3) == 0) { mon = i; break; }
    }
    if (mon < 0) return 0;

    /* 2-digit year handling */
    if (year < 100) year += (year < 50) ? 2000 : 1900;

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_mday = day;
    tm.tm_mon  = mon;
    tm.tm_year = year - 1900;
    tm.tm_hour = hour;
    tm.tm_min  = min;
    tm.tm_sec  = sec;

    /* Parse timezone offset (e.g., +0000, -0500) */
    const char *tz = p;
    int tz_offset = 0;
    while (*tz && *tz != '+' && *tz != '-') tz++;
    if (*tz == '+' || *tz == '-') {
        int tz_val = 0;
        if (sscanf(tz + 1, "%4d", &tz_val) == 1) {
            tz_offset = (tz_val / 100) * 3600 + (tz_val % 100) * 60;
            if (*tz == '-') tz_offset = -tz_offset;
        }
    }

    time_t t = timegm(&tm);
    if (t == (time_t)-1) return 0;
    return ((int64_t)t - tz_offset) * 1000;
}

static mnemon_err_t parse_mbox(mnemon_storage_t *s, const char *data,
                               size_t len, const mnemon_import_opts_t *opts,
                               mnemon_import_result_t *result)
{
    const char *p = data;
    const char *end = data + len;

    while (p < end) {
        /* Find "From " separator */
        if (strncmp(p, "From ", 5) != 0) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            p = nl ? nl + 1 : end;
            continue;
        }

        /* Skip "From " line */
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        p = nl + 1;

        /* Parse headers -- extract Date: if preserve_timestamps is set */
        int64_t msg_timestamp = 0;
        const char *body = NULL;
        while (p < end) {
            nl = memchr(p, '\n', (size_t)(end - p));
            if (!nl) break;
            if (nl == p || (nl == p + 1 && *p == '\r')) {
                body = nl + 1;
                break;
            }
            /* Extract Date: header */
            if (opts->preserve_timestamps &&
                (size_t)(nl - p) > 6 &&
                strncasecmp(p, "Date:", 5) == 0) {
                const char *val = p + 5;
                while (*val == ' ' && val < nl) val++;
                size_t vlen = (size_t)(nl - val);
                if (vlen > 0 && vlen < 256) {
                    char datebuf[256];
                    memcpy(datebuf, val, vlen);
                    datebuf[vlen] = '\0';
                    /* Strip trailing \r */
                    if (vlen > 0 && datebuf[vlen - 1] == '\r')
                        datebuf[vlen - 1] = '\0';
                    msg_timestamp = parse_rfc2822_date(datebuf);
                }
            }
            p = nl + 1;
        }

        if (!body) break;

        /* Find end of message (next "From " or EOF) */
        const char *msg_end = end;
        const char *scan = body;
        while (scan < end) {
            nl = memchr(scan, '\n', (size_t)(end - scan));
            if (!nl) break;
            if (nl + 1 < end && strncmp(nl + 1, "From ", 5) == 0) {
                msg_end = nl + 1;
                break;
            }
            scan = nl + 1;
        }

        size_t body_len = (size_t)(msg_end - body);
        if (body_len > 0) {
            char *content = strndup(body, body_len);
            if (store_chunk(s, content, opts, msg_timestamp) == 0)
                result->imported++;
            else
                result->errors++;
            free(content);
        }

        p = msg_end;
    }

    return MNEMON_OK;
}

static mnemon_err_t parse_text(mnemon_storage_t *s, const char *data,
                               size_t len, const mnemon_import_opts_t *opts,
                               mnemon_import_result_t *result)
{
    mnemon_chunks_t chunks;
    mnemon_err_t err = mnemon_chunk_text(data, len,
                                          opts->chunking ? opts->chunking : "paragraph",
                                          opts->max_chunk_size,
                                          &chunks);
    if (err != MNEMON_OK) return err;

    for (int i = 0; i < chunks.count; i++) {
        /* Skip whitespace-only chunks */
        const char *c = chunks.chunks[i];
        while (*c && isspace((unsigned char)*c)) c++;
        if (*c == '\0') { result->skipped++; continue; }

        if (store_chunk(s, chunks.chunks[i], opts, 0) == 0)
            result->imported++;
        else
            result->errors++;
        result->chunks_created++;
    }

    mnemon_chunks_free(&chunks);
    return MNEMON_OK;
}

/* ---- Path validation ---- */

bool mnemon_import_path_allowed(const char *path, const char *allowed_paths)
{
    if (!path || !allowed_paths) return false;

    char resolved[4096];
    if (!realpath(path, resolved)) return false;

    /* Parse comma-separated allowed paths */
    char *allowed = strdup(allowed_paths);
    char *tok = strtok(allowed, ",");

    while (tok) {
        while (*tok == ' ') tok++;

        /* Tilde expand */
        char expanded[4096];
        if (tok[0] == '~') {
            const char *home = getenv("HOME");
            if (home)
                snprintf(expanded, sizeof(expanded), "%s%s", home, tok + 1);
            else
                snprintf(expanded, sizeof(expanded), "%s", tok);
        } else {
            snprintf(expanded, sizeof(expanded), "%s", tok);
        }

        char resolved_allowed[4096];
        if (realpath(expanded, resolved_allowed)) {
            size_t alen = strlen(resolved_allowed);
            /* Prefix match with path boundary check:
             * /home/user must not match /home/username.
             * Require next char is '/' or '\0' (exact match). */
            if (strncmp(resolved, resolved_allowed, alen) == 0 &&
                (resolved[alen] == '/' || resolved[alen] == '\0')) {
                free(allowed);
                return true;
            }
        }

        tok = strtok(NULL, ",");
    }

    free(allowed);
    return false;
}

/* ---- Auto-format detection ---- */

static const char *detect_format(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "text";
    if (strcmp(ext, ".jsonl") == 0) return "jsonl";
    if (strcmp(ext, ".csv") == 0) return "csv";
    if (strcmp(ext, ".mbox") == 0 || strcmp(ext, ".mbx") == 0) return "mbox";
    if (strcmp(ext, ".md") == 0 || strcmp(ext, ".markdown") == 0) return "markdown";
    return "text";
}

/* ---- Main import entry point ---- */

mnemon_err_t mnemon_import_file(mnemon_storage_t *s, const char *path,
                                const char *format,
                                const mnemon_import_opts_t *opts,
                                mnemon_import_result_t *result)
{
    if (!s || !path || !result)
        return MNEMON_ERR_INVALID_INPUT;

    memset(result, 0, sizeof(*result));
    int64_t start = mnemon_time_ms();

    mnemon_import_opts_t default_opts;
    if (!opts) {
        memset(&default_opts, 0, sizeof(default_opts));
        default_opts.source_type = "import";
        default_opts.chunking = "paragraph";
        default_opts.max_chunk_size = 4096;
        opts = &default_opts;
    }

    /* Read file */
    size_t data_len;
    char *data = read_file(path, &data_len);
    if (!data) {
        mnemon_err_set(MNEMON_ERR_IO, 0, "cannot read file: %s", path);
        return MNEMON_ERR_IO;
    }

    /* Detect format */
    if (!format || strcmp(format, "auto") == 0)
        format = detect_format(path);

    mnemon_err_t err;
    if (strcmp(format, "jsonl") == 0)
        err = parse_jsonl(s, data, data_len, opts, result);
    else if (strcmp(format, "csv") == 0)
        err = parse_csv(s, data, data_len, opts, result);
    else if (strcmp(format, "mbox") == 0)
        err = parse_mbox(s, data, data_len, opts, result);
    else
        err = parse_text(s, data, data_len, opts, result);

    free(data);
    result->duration_ms = mnemon_time_ms() - start;

    mnemon_log(MNEMON_LOG_INFO, "import %s: %d imported, %d skipped, %d errors, %lldms",
               path, result->imported, result->skipped, result->errors,
               (long long)result->duration_ms);

    return err;
}
