/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * daemon.h -- Daemonize interface
 *
 * Phase 1: --stdio mode only (no daemonization).
 * Phase 3 adds: fork/setsid, PID file, sd_notify.
 */

#ifndef MNEMON_DAEMON_H
#define MNEMON_DAEMON_H

#include "mnemon.h"

/* Daemonize the process (fork, setsid, close fds) */
mnemon_err_t mnemon_daemonize(void);

/* PID file management */
mnemon_err_t mnemon_pidfile_create(const char *path);
mnemon_err_t mnemon_pidfile_remove(const char *path);

/* systemd notification (if available) */
mnemon_err_t mnemon_sd_notify_ready(void);
mnemon_err_t mnemon_sd_notify_stopping(void);

#endif /* MNEMON_DAEMON_H */
