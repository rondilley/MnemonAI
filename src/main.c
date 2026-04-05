/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * main.c -- Entry point and startup/shutdown orchestration for mnemond
 *
 * Handles CLI argument parsing, configuration loading, component
 * initialization, and the main MCP event loop.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#ifdef HAVE_GETOPT_H
# include <getopt.h>
#endif

#include "mnemon.h"
#include "config_parse.h"
#include "log.h"
#include "daemon.h"
#include "hardware.h"
#include "storage.h"
#include "mcp_stdio.h"
#include "mcp_http.h"
#include "mcp_dispatch.h"
#include "threads.h"

/* Long options */
static struct option long_options[] = {
    {"stdio",           no_argument,       NULL, 's'},
    {"daemon",          no_argument,       NULL, 'd'},
    {"foreground",      no_argument,       NULL, 'f'},
    {"config",          required_argument, NULL, 'c'},
    {"rebuild-indexes", no_argument,       NULL, 'r'},
    {"no-gpu",          no_argument,       NULL, 'G'},
    {"check-config",    no_argument,       NULL, 'C'},
    {"version",         no_argument,       NULL, 'v'},
    {"help",            no_argument,       NULL, 'h'},
    {NULL,              0,                 NULL, 0}
};

typedef enum {
    MODE_STDIO,
    MODE_DAEMON,
    MODE_FOREGROUND,
} run_mode_t;

static void print_version(void)
{
    if (PACKAGE_GIT_COMMIT[0] != '\0')
        fprintf(stderr, "%s v%s (%s)\n", PACKAGE_NAME, PACKAGE_VERSION, PACKAGE_GIT_COMMIT);
    else
        fprintf(stderr, "%s v%s\n", PACKAGE_NAME, PACKAGE_VERSION);
}

static void print_usage(const char *progname)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --stdio             Run in foreground, MCP over stdin/stdout\n"
        "  --daemon            Daemonize (fork, setsid)\n"
        "  --foreground        Run in foreground without stdio\n"
        "  --config FILE       Configuration file path\n"
        "  --rebuild-indexes   Rebuild FTS5 and usearch from LMDB, then exit\n"
        "  --no-gpu            Skip GPU detection\n"
        "  --check-config      Validate config and exit\n"
        "  --version           Print version and exit\n"
        "  --help              Show this help\n"
        "\n"
        "Default mode: --stdio\n",
        progname);
}

/* Signal flags (set by handlers, acted on in main loop) */
static volatile sig_atomic_t sig_reload = 0;
static volatile sig_atomic_t sig_stats = 0;

static void signal_handler(int sig)
{
    if (sig == SIGHUP)
        sig_reload = 1;
    else if (sig == SIGUSR1)
        sig_stats = 1;
    else
        mnemon_request_shutdown();
}

/* Install signal handlers */
static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    /* SIGTERM, SIGINT -> graceful shutdown */
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* SIGHUP -> reload config (log level, decay params) */
    sigaction(SIGHUP, &sa, NULL);

    /* SIGUSR1 -> dump stats to log */
    sigaction(SIGUSR1, &sa, NULL);

    /* SIGPIPE -> ignore (stdio writes handle errors) */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

/* Parse log level string to enum */
static mnemon_log_level_t parse_log_level(const char *str)
{
    if (!str) return MNEMON_LOG_INFO;
    if (strcmp(str, "debug") == 0) return MNEMON_LOG_DEBUG;
    if (strcmp(str, "info") == 0)  return MNEMON_LOG_INFO;
    if (strcmp(str, "warn") == 0 || strcmp(str, "warning") == 0)
        return MNEMON_LOG_WARNING;
    if (strcmp(str, "error") == 0) return MNEMON_LOG_ERROR;
    return MNEMON_LOG_INFO;
}

/* MCP stdio dispatch callback */
static int stdio_read_request(void *ctx, cJSON **out)
{
    (void)ctx;
    (void)out;
    /* Implemented in mcp_stdio.c */
    return -1;
}

static int stdio_write_response(void *ctx, const cJSON *response)
{
    (void)ctx;
    (void)response;
    /* Implemented in mcp_stdio.c */
    return -1;
}

int main(int argc, char *argv[])
{
    const char *config_path = NULL;
    run_mode_t mode = MODE_STDIO;
    bool rebuild_indexes = false;
    bool check_config = false;
    bool no_gpu = false;
    int opt;

    mnemon_config_t *cfg = NULL;
    mnemon_http_t *http = NULL;
    mnemon_storage_t *storage = NULL;
    mnemon_dispatch_t *dispatch = NULL;
    mnemon_hardware_t hw;
    mnemon_err_t err;
    int exit_code = EXIT_SUCCESS;

    /* Parse command-line arguments */
    while ((opt = getopt_long(argc, argv, "sdfc:rGCvh", long_options, NULL)) != -1) {
        switch (opt) {
        case 's': mode = MODE_STDIO; break;
        case 'd': mode = MODE_DAEMON; break;
        case 'f': mode = MODE_FOREGROUND; break;
        case 'c': config_path = optarg; break;
        case 'r': rebuild_indexes = true; break;
        case 'G': no_gpu = true; break;
        case 'C': check_config = true; break;
        case 'v':
            print_version();
            return EXIT_SUCCESS;
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    /* Load configuration */
    err = mnemon_config_load(config_path, &cfg);
    if (err != MNEMON_OK) {
        fprintf(stderr, "error: failed to load config: %s\n",
                mnemon_err_msg());
        return EXIT_FAILURE;
    }

    /* Validate configuration */
    err = mnemon_config_validate(cfg);
    if (err != MNEMON_OK) {
        fprintf(stderr, "error: invalid config: %s\n", mnemon_err_msg());
        mnemon_config_free(cfg);
        return EXIT_FAILURE;
    }

    if (check_config) {
        fprintf(stderr, "Configuration is valid.\n");
        mnemon_config_free(cfg);
        return EXIT_SUCCESS;
    }

    /* Initialize logging based on run mode:
     *   --stdio:      stderr only (stdout is MCP JSON-RPC channel)
     *   --foreground: stdout for status, stderr for errors
     *   --daemon:     foreground until fork, then syslog */
    {
        mnemon_log_mode_t log_mode;
        switch (mode) {
        case MODE_STDIO:      log_mode = MNEMON_LOG_MODE_STDERR;     break;
        case MODE_FOREGROUND: log_mode = MNEMON_LOG_MODE_FOREGROUND; break;
        case MODE_DAEMON:     log_mode = MNEMON_LOG_MODE_FOREGROUND; break;
        }
        mnemon_log_init(log_mode, parse_log_level(cfg->log_level));
    }
    mnemon_log(MNEMON_LOG_INFO, "%s %s starting", PACKAGE_NAME, PACKAGE_VERSION);

    /* Install signal handlers */
    setup_signals();

    /* Hardware detection */
    memset(&hw, 0, sizeof(hw));
    if (!no_gpu) {
        err = mnemon_hardware_detect(&hw);
        if (err != MNEMON_OK)
            mnemon_log(MNEMON_LOG_WARNING, "hardware detection failed: %s",
                       mnemon_err_msg());
    }
    mnemon_simd_init(&hw);
    mnemon_log(MNEMON_LOG_INFO, "CPU: %s (%d cores) SIMD: %s",
               hw.cpu_model, hw.cpu_cores, g_simd_ops.name);

    /* Daemonize if requested */
    if (mode == MODE_DAEMON) {
        /* Log pre-fork status to foreground (stdout/stderr still attached) */
        mnemon_log(MNEMON_LOG_INFO, "daemonizing...");

        err = mnemon_daemonize();
        if (err != MNEMON_OK) {
            mnemon_log(MNEMON_LOG_ERROR, "daemonize failed: %s",
                       mnemon_err_msg());
            exit_code = EXIT_FAILURE;
            goto cleanup_config;
        }

        /* After fork: stdout/stderr are /dev/null, switch to syslog */
        mnemon_log_shutdown();
        mnemon_log_init(MNEMON_LOG_MODE_SYSLOG, parse_log_level(cfg->log_level));
        mnemon_log(MNEMON_LOG_INFO, "%s %s daemon started (pid %d)",
                   PACKAGE_NAME, PACKAGE_VERSION, (int)getpid());
    }

    /* Open storage (LMDB + FTS5 + usearch + embedding model) */
    err = mnemon_storage_open(&storage, cfg);
    if (err != MNEMON_OK) {
        mnemon_log(MNEMON_LOG_ERROR, "failed to open storage: %s",
                   mnemon_err_msg());
        exit_code = EXIT_FAILURE;
        goto cleanup_config;
    }

    mnemon_log(MNEMON_LOG_INFO, "storage opened: %s", cfg->data_dir);

    /* Handle --rebuild-indexes */
    if (rebuild_indexes) {
        mnemon_log(MNEMON_LOG_INFO, "rebuilding indexes...");
        err = mnemon_rebuild_indexes(storage, "all");
        if (err != MNEMON_OK) {
            mnemon_log(MNEMON_LOG_ERROR, "index rebuild failed: %s",
                       mnemon_err_msg());
            exit_code = EXIT_FAILURE;
        } else {
            mnemon_log(MNEMON_LOG_INFO, "index rebuild complete");
        }
        goto cleanup_storage;
    }

    /* Initialize MCP dispatch */
    err = mnemon_dispatch_init(&dispatch, storage);
    if (err != MNEMON_OK) {
        mnemon_log(MNEMON_LOG_ERROR, "failed to init MCP dispatch: %s",
                   mnemon_err_msg());
        exit_code = EXIT_FAILURE;
        goto cleanup_storage;
    }

    /* Start HTTP transport for daemon/foreground modes */
    if (mode != MODE_STDIO && cfg->http_enabled) {
        mnemon_http_config_t http_cfg = {
            .bind_address    = cfg->http_bind,
            .port            = cfg->http_port,
            .max_connections = cfg->http_max_connections,
            .auth_token      = cfg->http_auth_token,
            .tls_cert_path   = NULL, /* TODO: add tls_cert/tls_key to config */
            .tls_key_path    = NULL,
            .mcp_path        = "/mcp",
        };

        err = mnemon_http_start(&http, &http_cfg, dispatch);
        if (err != MNEMON_OK) {
            mnemon_log(MNEMON_LOG_ERROR, "HTTP transport failed: %s",
                       mnemon_err_msg());
            exit_code = EXIT_FAILURE;
            goto cleanup_dispatch;
        }
    } else if (mode != MODE_STDIO && !cfg->http_enabled) {
        mnemon_log(MNEMON_LOG_WARNING,
                   "running in %s mode but [http] enabled = false -- "
                   "no network listener. Set enabled = true in config.",
                   mode == MODE_DAEMON ? "daemon" : "foreground");
    }

    /* Notify systemd if in foreground mode */
    if (mode == MODE_FOREGROUND)
        mnemon_sd_notify_ready();

    /* --- Main event loop --- */
    if (mode == MODE_STDIO) {
        mnemon_log(MNEMON_LOG_INFO, "entering stdio MCP loop");

        mnemon_transport_t transport;
        memset(&transport, 0, sizeof(transport));
        transport.read_request = stdio_read_request;
        transport.write_response = stdio_write_response;

        err = mnemon_mcp_stdio_init(&transport);
        if (err != MNEMON_OK) {
            mnemon_log(MNEMON_LOG_ERROR, "stdio init failed: %s",
                       mnemon_err_msg());
            exit_code = EXIT_FAILURE;
            goto cleanup_dispatch;
        }

        /* The run loop reads from stdin, dispatches, writes to stdout */
        err = mnemon_mcp_stdio_run(&transport, dispatch);
        if (err != MNEMON_OK && !mnemon_shutdown_requested()) {
            mnemon_log(MNEMON_LOG_ERROR, "stdio loop error: %s",
                       mnemon_err_msg());
            exit_code = EXIT_FAILURE;
        }
    } else {
        /* Daemon/foreground mode: wait for signals */
        mnemon_log(MNEMON_LOG_INFO, "running in %s mode, waiting for signals",
                   mode == MODE_DAEMON ? "daemon" : "foreground");
        while (!mnemon_shutdown_requested()) {
            pause(); /* Wait for any signal */

            /* SIGHUP: reload configuration */
            if (sig_reload) {
                sig_reload = 0;
                mnemon_log(MNEMON_LOG_INFO, "SIGHUP received, reloading config");
                mnemon_config_t *newcfg = NULL;
                if (mnemon_config_load(config_path, &newcfg) == MNEMON_OK) {
                    /* Apply safe runtime changes */
                    mnemon_log_set_level(parse_log_level(newcfg->log_level));
                    mnemon_log(MNEMON_LOG_INFO, "config reloaded: log_level=%s",
                               newcfg->log_level);
                    mnemon_config_free(newcfg);
                } else {
                    mnemon_log(MNEMON_LOG_ERROR, "config reload failed: %s",
                               mnemon_err_msg());
                }
            }

            /* SIGUSR1: dump stats to log */
            if (sig_stats) {
                sig_stats = 0;
                mnemon_stats_t st = {0};
                mnemon_get_stats(storage, &st);
                mnemon_log(MNEMON_LOG_INFO,
                           "STATS: memories=%zu entities=%zu edges=%zu "
                           "fts=%zu mem_vectors=%zu ent_vectors=%zu",
                           st.total_memories, st.total_entities,
                           st.total_edges, st.fts_indexed,
                           st.memory_vectors, st.entity_vectors);
            }
        }
    }

    /* --- Shutdown sequence --- */
    mnemon_log(MNEMON_LOG_INFO, "shutting down...");

    if (mode == MODE_FOREGROUND || mode == MODE_DAEMON)
        mnemon_sd_notify_stopping();

    /* Stop HTTP transport */
    if (http)
        mnemon_http_stop(http);

cleanup_dispatch:
    if (dispatch)
        mnemon_dispatch_free(dispatch);

cleanup_storage:
    if (storage)
        mnemon_storage_close(storage);

cleanup_config:
    mnemon_config_free(cfg);

    mnemon_log(MNEMON_LOG_INFO, "%s shutdown complete", PACKAGE_NAME);
    mnemon_log_shutdown();

    return exit_code;
}
