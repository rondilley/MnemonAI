/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * audit.c -- Append-only JSON audit log with security alerts
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "audit.h"
#include "memory.h"
#include "log.h"

struct mnemon_audit {
    FILE           *fp;
    pthread_mutex_t mutex;
};

static const char *level_names[] = {"INFO", "MEDIUM", "HIGH", "CRITICAL"};

const char *mnemon_alert_level_name(mnemon_alert_level_t level)
{
    if ((unsigned)level < 4) return level_names[level];
    return "UNKNOWN";
}

mnemon_err_t mnemon_audit_open(mnemon_audit_t **out, const char *path)
{
    if (!out || !path)
        return MNEMON_ERR_INVALID_INPUT;

    mnemon_audit_t *a = calloc(1, sizeof(*a));
    if (!a) return MNEMON_ERR_OOM;

    a->fp = fopen(path, "a");
    if (!a->fp) {
        free(a);
        mnemon_err_set(MNEMON_ERR_IO, 0, "cannot open audit log: %s", path);
        return MNEMON_ERR_IO;
    }

    setvbuf(a->fp, NULL, _IOLBF, 0);
    pthread_mutex_init(&a->mutex, NULL);

    *out = a;
    return MNEMON_OK;
}

void mnemon_audit_close(mnemon_audit_t *a)
{
    if (!a) return;
    if (a->fp) fclose(a->fp);
    pthread_mutex_destroy(&a->mutex);
    free(a);
}

mnemon_err_t mnemon_audit_log(mnemon_audit_t *a, const char *tool_name,
                              const char *params_json,
                              const char *result_summary)
{
    if (!a || !a->fp || !tool_name)
        return MNEMON_ERR_INVALID_INPUT;

    char ts[32];
    mnemon_format_iso8601(mnemon_time_ms(), ts, sizeof(ts));

    pthread_mutex_lock(&a->mutex);
    fprintf(a->fp,
            "{\"ts\":\"%s\",\"tool\":\"%s\",\"params\":%s,\"result\":\"%s\"}\n",
            ts, tool_name,
            params_json ? params_json : "{}",
            result_summary ? result_summary : "ok");
    pthread_mutex_unlock(&a->mutex);

    return MNEMON_OK;
}

mnemon_err_t mnemon_audit_alert(mnemon_audit_t *a,
                                const char *alert_type,
                                mnemon_alert_level_t level,
                                const char *session_id,
                                const char *tool_name,
                                const char *detail)
{
    if (!a || !a->fp || !alert_type)
        return MNEMON_ERR_INVALID_INPUT;

    char ts[32];
    mnemon_format_iso8601(mnemon_time_ms(), ts, sizeof(ts));

    const char *level_str = mnemon_alert_level_name(level);

    pthread_mutex_lock(&a->mutex);
    fprintf(a->fp,
            "{\"ts\":\"%s\",\"alert\":\"%s\",\"level\":\"%s\","
            "\"session\":\"%s\",\"tool\":\"%s\",\"detail\":\"%s\"}\n",
            ts, alert_type, level_str,
            session_id ? session_id : "",
            tool_name ? tool_name : "",
            detail ? detail : "");
    pthread_mutex_unlock(&a->mutex);

    /* Also log to syslog/stderr for high-severity alerts */
    if (level >= ALERT_HIGH)
        mnemon_log(MNEMON_LOG_WARNING, "SECURITY ALERT [%s] %s: %s (session=%s tool=%s)",
                   level_str, alert_type, detail ? detail : "",
                   session_id ? session_id : "?",
                   tool_name ? tool_name : "?");

    return MNEMON_OK;
}
