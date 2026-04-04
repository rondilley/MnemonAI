/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * log.h -- Structured logging interface
 *
 * Supports three output modes:
 *   MNEMON_LOG_MODE_FOREGROUND -- INFO/DEBUG/WARNING to stdout, ERROR to stderr
 *   MNEMON_LOG_MODE_SYSLOG    -- all levels to syslog (daemon mode)
 *   MNEMON_LOG_MODE_STDERR    -- all levels to stderr (stdio MCP mode)
 */

#ifndef MNEMON_LOG_H
#define MNEMON_LOG_H

#include "mnemon.h"

typedef enum {
    MNEMON_LOG_DEBUG,
    MNEMON_LOG_INFO,
    MNEMON_LOG_WARNING,
    MNEMON_LOG_ERROR
} mnemon_log_level_t;

typedef enum {
    MNEMON_LOG_MODE_FOREGROUND,  /* stdout for status, stderr for errors */
    MNEMON_LOG_MODE_SYSLOG,      /* syslog (daemon mode, no terminal) */
    MNEMON_LOG_MODE_STDERR,      /* all to stderr (stdio MCP mode) */
} mnemon_log_mode_t;

mnemon_err_t mnemon_log_init(mnemon_log_mode_t mode, mnemon_log_level_t level);
void mnemon_log(mnemon_log_level_t level, const char *fmt, ...);
void mnemon_log_set_level(mnemon_log_level_t level);
void mnemon_log_shutdown(void);

#endif /* MNEMON_LOG_H */
