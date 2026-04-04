/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mnemon_err.c -- Error handling and struct free functions
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "mnemon.h"

/* Thread-local error context */
typedef struct {
    mnemon_err_t err;
    int          native_code;
    char         msg[512];
} mnemon_err_ctx_t;

static _Thread_local mnemon_err_ctx_t tl_err = {MNEMON_OK, 0, ""};

const char *mnemon_strerror(mnemon_err_t err)
{
    switch (err) {
    case MNEMON_OK:                 return "success";
    case MNEMON_ERR_NOT_FOUND:      return "not found";
    case MNEMON_ERR_ALREADY_EXISTS: return "already exists";
    case MNEMON_ERR_LMDB:           return "LMDB error";
    case MNEMON_ERR_SQLITE:         return "SQLite error";
    case MNEMON_ERR_USEARCH:        return "usearch error";
    case MNEMON_ERR_EMBED:          return "embedding error";
    case MNEMON_ERR_EXTRACTION:     return "extraction error";
    case MNEMON_ERR_SECRET_DETECTED:return "secret detected in content";
    case MNEMON_ERR_INVALID_INPUT:  return "invalid input";
    case MNEMON_ERR_QUEUE_FULL:     return "write queue full";
    case MNEMON_ERR_SHUTDOWN:       return "shutdown in progress";
    case MNEMON_ERR_OOM:            return "out of memory";
    case MNEMON_ERR_IO:             return "I/O error";
    case MNEMON_ERR_INTERNAL:       return "internal error";
    }
    return "unknown error";
}

const char *mnemon_err_msg(void)
{
    if (tl_err.msg[0] != '\0')
        return tl_err.msg;
    return mnemon_strerror(tl_err.err);
}

int mnemon_err_code(void)
{
    return tl_err.native_code;
}

void mnemon_err_set(mnemon_err_t err, int native_code,
                    const char *fmt, ...)
{
    va_list ap;
    tl_err.err = err;
    tl_err.native_code = native_code;
    if (fmt) {
        va_start(ap, fmt);
        vsnprintf(tl_err.msg, sizeof(tl_err.msg), fmt, ap);
        va_end(ap);
    } else {
        tl_err.msg[0] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* Free functions                                                      */
/* ------------------------------------------------------------------ */

void mnemon_entity_free(mnemon_entity_t *e)
{
    uint32_t i;
    if (!e) return;
    free(e->name);
    free(e->entity_type);
    if (e->observations) {
        for (i = 0; i < e->observation_count; i++)
            free(e->observations[i]);
        free(e->observations);
    }
    free(e->embedding);
    memset(e, 0, sizeof(*e));
}

void mnemon_edge_free(mnemon_edge_t *e)
{
    if (!e) return;
    free(e->edge_type);
    free(e->description);
    memset(e, 0, sizeof(*e));
}

void mnemon_edge_list_free(mnemon_edge_list_t *list)
{
    uint32_t i;
    if (!list) return;
    if (list->edges) {
        for (i = 0; i < list->count; i++)
            mnemon_edge_free(&list->edges[i]);
        free(list->edges);
    }
    memset(list, 0, sizeof(*list));
}

void mnemon_memory_free(mnemon_memory_t *mem)
{
    uint32_t i;
    if (!mem) return;
    free(mem->content);
    free(mem->source_type);
    free(mem->source_id);
    free(mem->source_author);
    if (mem->tags) {
        for (i = 0; i < mem->tag_count; i++)
            free(mem->tags[i]);
        free(mem->tags);
    }
    free(mem->embedding);
    if (mem->entity_ids) {
        for (i = 0; i < mem->entity_id_count; i++)
            free(mem->entity_ids[i]);
        free(mem->entity_ids);
    }
    memset(mem, 0, sizeof(*mem));
}

void mnemon_result_set_free(mnemon_result_set_t *rs)
{
    int i;
    if (!rs) return;
    if (rs->results) {
        for (i = 0; i < rs->count; i++) {
            free(rs->results[i].content);
            free(rs->results[i].tier);
        }
        free(rs->results);
    }
    memset(rs, 0, sizeof(*rs));
}

void mnemon_version_list_free(mnemon_version_list_t *vl)
{
    uint32_t i;
    if (!vl) return;
    if (vl->versions) {
        for (i = 0; i < vl->count; i++)
            mnemon_entity_free(&vl->versions[i]);
        free(vl->versions);
    }
    memset(vl, 0, sizeof(*vl));
}
