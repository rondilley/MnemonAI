/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * daemon.c -- Daemonize, PID file, signal handling
 *
 * Phase 1: Minimal implementation for --stdio mode.
 * Phase 3 adds full daemonization and systemd integration.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#endif

#ifdef HAVE_SIGNAL_H
# include <signal.h>
#endif

#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif

#include "daemon.h"
#include "log.h"

mnemon_err_t mnemon_daemonize(void)
{
#ifdef HAVE_FORK
    pid_t pid = fork();
    if (pid < 0) {
        mnemon_err_set(MNEMON_ERR_INTERNAL, 0, "fork() failed");
        return MNEMON_ERR_INTERNAL;
    }
    if (pid > 0)
        _exit(0); /* Parent exits */

#ifdef HAVE_SETSID
    if (setsid() < 0) {
        mnemon_err_set(MNEMON_ERR_INTERNAL, 0, "setsid() failed");
        return MNEMON_ERR_INTERNAL;
    }
#endif

    /* Second fork to prevent acquiring a controlling terminal */
    pid = fork();
    if (pid < 0) {
        mnemon_err_set(MNEMON_ERR_INTERNAL, 0, "second fork() failed");
        return MNEMON_ERR_INTERNAL;
    }
    if (pid > 0)
        _exit(0);

    /* Set file permissions */
    umask(0077);

    /* Change to root directory */
    if (chdir("/") < 0) {
        /* Non-fatal */
    }

    /* Close stdin/stdout/stderr and redirect to /dev/null */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
    }

    return MNEMON_OK;
#else
    mnemon_err_set(MNEMON_ERR_INTERNAL, 0, "fork() not available");
    return MNEMON_ERR_INTERNAL;
#endif
}

mnemon_err_t mnemon_pidfile_create(const char *path)
{
    FILE *f;

    if (!path)
        return MNEMON_ERR_INVALID_INPUT;

    f = fopen(path, "w");
    if (!f) {
        mnemon_err_set(MNEMON_ERR_INTERNAL, 0,
                       "cannot create PID file: %s", path);
        return MNEMON_ERR_INTERNAL;
    }

    fprintf(f, "%d\n", (int)getpid());
    fclose(f);

    mnemon_log(MNEMON_LOG_INFO, "PID file created: %s", path);
    return MNEMON_OK;
}

mnemon_err_t mnemon_pidfile_remove(const char *path)
{
    if (!path)
        return MNEMON_ERR_INVALID_INPUT;

    if (unlink(path) != 0) {
        mnemon_log(MNEMON_LOG_WARNING, "cannot remove PID file: %s", path);
    }

    return MNEMON_OK;
}

mnemon_err_t mnemon_sd_notify_ready(void)
{
#ifdef ENABLE_SYSTEMD
#ifdef HAVE_LIBSYSTEMD
    extern int sd_notify(int unset_environment, const char *state);
    sd_notify(0, "READY=1");
    mnemon_log(MNEMON_LOG_INFO, "systemd: notified READY");
#endif
#endif
    return MNEMON_OK;
}

mnemon_err_t mnemon_sd_notify_stopping(void)
{
#ifdef ENABLE_SYSTEMD
#ifdef HAVE_LIBSYSTEMD
    extern int sd_notify(int unset_environment, const char *state);
    sd_notify(0, "STOPPING=1");
    mnemon_log(MNEMON_LOG_INFO, "systemd: notified STOPPING");
#endif
#endif
    return MNEMON_OK;
}
