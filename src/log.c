/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * log.c -- Structured logging with mode-dependent output routing
 *
 * Three modes:
 *   FOREGROUND -- normal status (DEBUG/INFO/WARNING) to stdout,
 *                 errors (ERROR) to stderr. For interactive use.
 *   SYSLOG    -- all output to syslog. For daemon mode where
 *                stdout/stderr are /dev/null.
 *   STDERR    -- all output to stderr. For stdio MCP mode where
 *                stdout is reserved for the JSON-RPC protocol.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

#ifdef HAVE_SYSLOG_H
# include <syslog.h>
#endif

#include "log.h"

static mnemon_log_mode_t  log_mode  = MNEMON_LOG_MODE_STDERR;
static mnemon_log_level_t log_level = MNEMON_LOG_INFO;
static pthread_mutex_t    log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *level_names[] = {"DEBUG", "INFO", "WARN", "ERROR"};

mnemon_err_t mnemon_log_init(mnemon_log_mode_t mode, mnemon_log_level_t level)
{
    log_mode  = mode;
    log_level = level;

#ifdef HAVE_SYSLOG_H
    if (mode == MNEMON_LOG_MODE_SYSLOG)
        openlog("mnemon_ai", LOG_PID | LOG_NDELAY, LOG_DAEMON);
#endif

    return MNEMON_OK;
}

void mnemon_log(mnemon_log_level_t level, const char *fmt, ...)
{
    va_list ap;

    if (level < log_level)
        return;

    pthread_mutex_lock(&log_mutex);

    /* ---- syslog mode ---- */
#ifdef HAVE_SYSLOG_H
    if (log_mode == MNEMON_LOG_MODE_SYSLOG) {
        int prio;
        switch (level) {
        case MNEMON_LOG_DEBUG:   prio = LOG_DEBUG;   break;
        case MNEMON_LOG_INFO:    prio = LOG_INFO;    break;
        case MNEMON_LOG_WARNING: prio = LOG_WARNING; break;
        case MNEMON_LOG_ERROR:   prio = LOG_ERR;     break;
        default:                 prio = LOG_INFO;    break;
        }
        va_start(ap, fmt);
        vsyslog(prio, fmt, ap);
        va_end(ap);
        pthread_mutex_unlock(&log_mutex);
        return;
    }
#endif

    /* ---- foreground or stderr mode ---- */

    /* Pick the output stream:
     *   FOREGROUND: ERROR -> stderr, everything else -> stdout
     *   STDERR:     everything -> stderr */
    FILE *fp;
    if (log_mode == MNEMON_LOG_MODE_FOREGROUND && level < MNEMON_LOG_ERROR)
        fp = stdout;
    else
        fp = stderr;

    /* Timestamp */
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);

    fprintf(fp, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ [%s] ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec,
            ts.tv_nsec / 1000000,
            (unsigned)level < 4 ? level_names[level] : "???");

    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    fputc('\n', fp);
    fflush(fp);

    pthread_mutex_unlock(&log_mutex);
}

void mnemon_log_set_level(mnemon_log_level_t level)
{
    log_level = level;
}

void mnemon_log_shutdown(void)
{
#ifdef HAVE_SYSLOG_H
    if (log_mode == MNEMON_LOG_MODE_SYSLOG)
        closelog();
#endif
}
