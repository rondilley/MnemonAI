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
#include <pthread.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#ifdef HAVE_GETOPT_H
# include <getopt.h>
#endif

#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

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
#include "honeypot.h"
#include "audit.h"
#include "embed.h"

/* Long options */
static struct option long_options[] = {
    {"stdio",           no_argument,       NULL, 's'},
    {"daemon",          no_argument,       NULL, 'd'},
    {"foreground",      no_argument,       NULL, 'f'},
    {"config",          required_argument, NULL, 'c'},
    {"rebuild-indexes", no_argument,       NULL, 'r'},
    {"no-gpu",          no_argument,       NULL, 'G'},
    {"check-config",    no_argument,       NULL, 'C'},
    {"gen-key",         no_argument,       NULL, 'k'},
    {"warmup",          no_argument,       NULL, 'W'},
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
        "  --warmup            Load model, run one embedding, then exit\n"
        "                      (triggers ROCm/CUDA JIT compilation)\n"
        "  --check-config      Validate config and exit\n"
        "  --gen-key           Generate MCP auth token and write to config\n"
        "  --version           Print version and exit\n"
        "  --help              Show this help\n"
        "\n"
        "Default mode: --stdio\n",
        progname);
}

/* Signal flags (set by handlers, acted on in main loop) */
static volatile sig_atomic_t sig_reload = 0;
static volatile sig_atomic_t sig_stats = 0;
static volatile sig_atomic_t sig_term_count = 0;

#define SHUTDOWN_TIMEOUT_SEC 10

static void signal_handler(int sig)
{
    if (sig == SIGHUP)
        sig_reload = 1;
    else if (sig == SIGUSR1)
        sig_stats = 1;
    else {
        sig_term_count++;
        mnemon_request_shutdown();
        /* Second SIGTERM/SIGINT = force exit after timeout.
         * Third = immediate hard exit. */
        if (sig_term_count >= 3)
            _exit(128 + sig);
    }
}

/* Shutdown watchdog: runs in a detached thread, calls _exit() if
 * graceful shutdown exceeds SHUTDOWN_TIMEOUT_SEC. */
static void *shutdown_watchdog(void *arg)
{
    (void)arg;
    sleep(SHUTDOWN_TIMEOUT_SEC);
    /* If we get here, graceful shutdown is stuck */
    fprintf(stderr, "mnemond: shutdown timed out after %ds, forcing exit\n",
            SHUTDOWN_TIMEOUT_SEC);
    _exit(EXIT_FAILURE);
    return NULL;
}

/* Heartbeat: periodically log runtime gauges so a hang or capacity issue
 * leaves a trail in the logs even when nobody is watching live. */
typedef struct {
    int                     interval_secs;
    mnemon_http_t          *http;
    mnemon_reader_pool_t   *reader_pool;
} heartbeat_ctx_t;

static void *heartbeat_thread(void *arg)
{
    heartbeat_ctx_t *ctx = (heartbeat_ctx_t *)arg;
    int interval = ctx->interval_secs > 0 ? ctx->interval_secs : 60;

    while (!mnemon_shutdown_requested()) {
        for (int i = 0; i < interval && !mnemon_shutdown_requested(); i++)
            sleep(1);
        if (mnemon_shutdown_requested()) break;

        if (ctx->http) {
            mnemon_http_stats_t hs;
            mnemon_http_get_stats(ctx->http, &hs);
            mnemon_log(MNEMON_LOG_INFO,
                       "HEARTBEAT http: sessions=%d connections=%d/%d "
                       "in_flight=%d total=%llu slow=%llu",
                       hs.sessions, hs.open_connections,
                       hs.max_connections, hs.in_flight_requests,
                       (unsigned long long)hs.total_requests,
                       (unsigned long long)hs.slow_requests);
        }
        if (ctx->reader_pool) {
            int qd = 0, busy = 0, sz = 0;
            mnemon_reader_pool_stats(ctx->reader_pool, &qd, &busy, &sz);
            mnemon_log(MNEMON_LOG_INFO,
                       "HEARTBEAT reader_pool: size=%d busy=%d queued=%d",
                       sz, busy, qd);
        }
    }
    return NULL;
}

/* Install signal handlers */
static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    /* SIGTERM, SIGINT -> graceful shutdown.
     * SA_RESTART is NOT set so that blocking I/O (fgetc, read, pause)
     * returns EINTR, allowing the main loop to check g_shutdown. */
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* No SA_RESTART -- must interrupt blocking calls */
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

/* SSE broadcast notifier (adapts mnemon_notify_fn to mnemon_http_broadcast_event) */
static void sse_notify_fn(void *ctx, const char *event_type,
                           const char *json_data)
{
    mnemon_http_broadcast_event((mnemon_http_t *)ctx, event_type, json_data);
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

/* Generate a 64-character hex token from 32 bytes of /dev/urandom */
static int generate_auth_token(char *out, size_t outlen)
{
    if (outlen < 65) return -1;

    unsigned char buf[32];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;

    size_t got = 0;
    while (got < sizeof(buf)) {
        ssize_t n = read(fd, buf + got, sizeof(buf) - got);
        if (n <= 0) { close(fd); return -1; }
        got += (size_t)n;
    }
    close(fd);

    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(buf); i++) {
        out[i * 2]     = hex[buf[i] >> 4];
        out[i * 2 + 1] = hex[buf[i] & 0x0f];
    }
    out[64] = '\0';
    return 0;
}

/* Resolve the config file path using the same search order as config_load */
static char *resolve_config_path(const char *explicit_path)
{
    if (explicit_path)
        return strdup(explicit_path);

    const char *xdg = getenv("XDG_CONFIG_HOME");
    char path[512];
    struct stat st;

    if (xdg) {
        snprintf(path, sizeof(path), "%s/mnemond/mnemond.conf", xdg);
        if (stat(path, &st) == 0)
            return strdup(path);
    }

    const char *home = getenv("HOME");
    if (home) {
        snprintf(path, sizeof(path), "%s/.config/mnemond/mnemond.conf", home);
        if (stat(path, &st) == 0)
            return strdup(path);
    }

    /* Check compiled-in SYSCONFDIR (e.g., ~/.local/etc/mnemond) */
    snprintf(path, sizeof(path), "%s/mnemond.conf", SYSCONFDIR);
    if (stat(path, &st) == 0)
        return strdup(path);

    if (stat("/etc/mnemond/mnemond.conf", &st) == 0)
        return strdup("/etc/mnemond/mnemond.conf");

    /* No existing config; default to SYSCONFDIR/mnemond.conf */
    snprintf(path, sizeof(path), "%s/mnemond.conf", SYSCONFDIR);
    return strdup(path);

    return NULL;
}

/* Ensure parent directories exist for a file path */
static int mkdirs(const char *filepath)
{
    char *tmp = strdup(filepath);
    if (!tmp) return -1;

    /* Find last slash to get directory portion */
    char *slash = strrchr(tmp, '/');
    if (!slash) { free(tmp); return 0; }
    *slash = '\0';

    /* Walk path components and mkdir as needed */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
    free(tmp);
    return 0;
}

/*
 * Write the auth_token into the config file.
 * If the file exists, replace the auth_token line in [http].
 * If no [http] section or no file, append/create as needed.
 */
static int write_key_to_config(const char *config_path, const char *token)
{
    char *content = NULL;
    size_t content_len = 0;

    /* Read existing config if present */
    FILE *fp = fopen(config_path, "r");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz > 0) {
            content = malloc((size_t)sz + 1);
            if (!content) { fclose(fp); return -1; }
            content_len = fread(content, 1, (size_t)sz, fp);
            content[content_len] = '\0';
        }
        fclose(fp);
    }

    /* Build the new auth_token line */
    char new_line[128];
    snprintf(new_line, sizeof(new_line), "auth_token = %s", token);

    if (content && content_len > 0) {
        /* Look for existing auth_token in [http] section */
        char *http_section = strstr(content, "[http]");
        if (http_section) {
            char *auth_key = strstr(http_section, "auth_token");
            /* Make sure we don't match past the next section */
            char *next_section = strchr(http_section + 1, '[');
            if (auth_key && (!next_section || auth_key < next_section)) {
                /* Find end of line */
                char *eol = strchr(auth_key, '\n');
                size_t old_line_len = eol ? (size_t)(eol - auth_key) : strlen(auth_key);
                size_t new_line_len = strlen(new_line);

                size_t new_size = content_len - old_line_len + new_line_len + 1;
                char *result = malloc(new_size);
                if (!result) { free(content); return -1; }

                size_t prefix_len = (size_t)(auth_key - content);
                memcpy(result, content, prefix_len);
                memcpy(result + prefix_len, new_line, new_line_len);
                if (eol) {
                    size_t suffix_len = content_len - prefix_len - old_line_len;
                    memcpy(result + prefix_len + new_line_len,
                           eol, suffix_len);
                }
                result[new_size - 1] = '\0';

                mkdirs(config_path);
                fp = fopen(config_path, "w");
                if (!fp) { free(result); free(content); return -1; }
                fputs(result, fp);
                fclose(fp);
                free(result);
                free(content);
                return 0;
            }
            /* [http] exists but no auth_token line -- insert before next section or at end */
            size_t insert_pos;
            if (next_section)
                insert_pos = (size_t)(next_section - content);
            else
                insert_pos = content_len;

            /* Back up past trailing whitespace to insert cleanly */
            while (insert_pos > 0 && (content[insert_pos - 1] == '\n' || content[insert_pos - 1] == ' '))
                insert_pos--;
            insert_pos++; /* keep one newline */

            size_t tail_len = content_len - insert_pos;
            size_t new_line_len = strlen(new_line);
            size_t new_size = insert_pos + new_line_len + 1 + tail_len + (next_section ? 1 : 0) + 1;
            char *result = malloc(new_size);
            if (!result) { free(content); return -1; }

            size_t pos = 0;
            memcpy(result + pos, content, insert_pos); pos += insert_pos;
            memcpy(result + pos, new_line, new_line_len); pos += new_line_len;
            result[pos++] = '\n';
            if (next_section && tail_len > 0) {
                result[pos++] = '\n';
            }
            if (tail_len > 0) {
                memcpy(result + pos, content + insert_pos, tail_len);
                pos += tail_len;
            }
            result[pos] = '\0';

            mkdirs(config_path);
            fp = fopen(config_path, "w");
            if (!fp) { free(result); free(content); return -1; }
            fputs(result, fp);
            fclose(fp);
            free(result);
            free(content);
            return 0;
        }
        /* No [http] section -- append one */
        mkdirs(config_path);
        fp = fopen(config_path, "w");
        if (!fp) { free(content); return -1; }
        fputs(content, fp);
        if (content_len > 0 && content[content_len - 1] != '\n')
            fputc('\n', fp);
        fprintf(fp, "\n[http]\n%s\n", new_line);
        fclose(fp);
        free(content);
        return 0;
    }

    /* No existing file or empty -- create minimal config */
    free(content);
    mkdirs(config_path);
    fp = fopen(config_path, "w");
    if (!fp) return -1;
    fprintf(fp,
        "# mnemond.conf -- generated by mnemond --gen-key\n"
        "\n"
        "[http]\n"
        "enabled = true\n"
        "%s\n", new_line);
    fclose(fp);
    return 0;
}

int main(int argc, char *argv[])
{
    const char *config_path = NULL;
    run_mode_t mode = MODE_STDIO;
    bool rebuild_indexes = false;
    bool check_config = false;
    bool gen_key = false;
    bool no_gpu = false;
    bool warmup = false;
    int opt;

    mnemon_config_t *cfg = NULL;
    mnemon_http_t *http = NULL;
    mnemon_storage_t *storage = NULL;
    mnemon_dispatch_t *dispatch = NULL;
    mnemon_honeypot_t *honeypot = NULL;
    mnemon_reader_pool_t reader_pool_inst;
    mnemon_reader_pool_t *reader_pool = NULL;
    mnemon_hardware_t hw;
    mnemon_err_t err;
    int exit_code = EXIT_SUCCESS;

    /* Parse command-line arguments */
    while ((opt = getopt_long(argc, argv, "sdfc:rGWCkvh", long_options, NULL)) != -1) {
        switch (opt) {
        case 's': mode = MODE_STDIO; break;
        case 'd': mode = MODE_DAEMON; break;
        case 'f': mode = MODE_FOREGROUND; break;
        case 'c': config_path = optarg; break;
        case 'r': rebuild_indexes = true; break;
        case 'G': no_gpu = true; break;
        case 'W': warmup = true; break;
        case 'C': check_config = true; break;
        case 'k': gen_key = true; break;
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

    /* Generate MCP auth key and write to config */
    if (gen_key) {
        char token[65];
        if (generate_auth_token(token, sizeof(token)) != 0) {
            fprintf(stderr, "error: failed to generate random token\n");
            mnemon_config_free(cfg);
            return EXIT_FAILURE;
        }

        char *conf_path = resolve_config_path(config_path);
        if (!conf_path) {
            fprintf(stderr, "error: cannot determine config file path\n");
            mnemon_config_free(cfg);
            return EXIT_FAILURE;
        }

        if (write_key_to_config(conf_path, token) != 0) {
            fprintf(stderr, "error: failed to write token to %s: %s\n",
                    conf_path, strerror(errno));
            free(conf_path);
            mnemon_config_free(cfg);
            return EXIT_FAILURE;
        }

        fprintf(stdout, "%s\n", token);
        fprintf(stderr, "Auth token written to %s\n", conf_path);
        free(conf_path);
        mnemon_config_free(cfg);
        return EXIT_SUCCESS;
    }

    /* Initialize logging based on run mode:
     *   --stdio:      stderr only (stdout is MCP JSON-RPC channel)
     *   --foreground: stdout for status, stderr for errors
     *   --daemon:     foreground until fork, then syslog */
    {
        mnemon_log_mode_t log_mode = MNEMON_LOG_MODE_STDERR;
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

    /* Handle --warmup: load model, run one embedding, exit.
     * This triggers ROCm/CUDA JIT kernel compilation so subsequent
     * starts are fast. Can take 5-15 minutes on first run with a
     * new GPU target (e.g., AMD gfx1151). */
    if (warmup) {
        mnemon_embed_t *embed = mnemon_storage_embed(storage);
        if (!embed) {
            mnemon_log(MNEMON_LOG_ERROR, "warmup: no embedding model loaded");
            exit_code = EXIT_FAILURE;
        } else {
            mnemon_log(MNEMON_LOG_INFO,
                       "warmup: running test embedding (this may take several "
                       "minutes on first run with GPU -- ROCm/CUDA JIT compilation)...");
            float *test_emb = malloc((size_t)mnemon_embed_dimensions(embed) * sizeof(float));
            if (test_emb) {
                err = mnemon_embed_text(embed, "warmup test", 11,
                                        test_emb, mnemon_embed_dimensions(embed),
                                        false);
                if (err == MNEMON_OK) {
                    mnemon_log(MNEMON_LOG_INFO,
                               "warmup: complete (%d dimensions). "
                               "GPU kernels are compiled and cached.",
                               mnemon_embed_dimensions(embed));
                } else {
                    mnemon_log(MNEMON_LOG_ERROR, "warmup: embedding failed: %s",
                               mnemon_err_msg());
                    exit_code = EXIT_FAILURE;
                }
                free(test_emb);
            } else {
                mnemon_log(MNEMON_LOG_ERROR, "warmup: OOM");
                exit_code = EXIT_FAILURE;
            }
        }
        goto cleanup_storage;
    }

    /* Initialize honeypot abuse detection */
    err = mnemon_honeypot_init(&honeypot, NULL);
    if (err == MNEMON_OK)
        mnemon_storage_set_honeypot(storage, honeypot);

    /* Initialize reader pool if configured */
    if (cfg->reader_pool_size > 0) {
        memset(&reader_pool_inst, 0, sizeof(reader_pool_inst));
        err = mnemon_reader_pool_start(&reader_pool_inst, cfg->reader_pool_size);
        if (err == MNEMON_OK) {
            reader_pool = &reader_pool_inst;
            mnemon_storage_set_reader_pool(storage, reader_pool);
        }
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
            .bind_address           = cfg->http_bind,
            .port                   = cfg->http_port,
            .max_connections        = cfg->http_max_connections,
            .connection_timeout     = cfg->http_connection_timeout,
            .per_ip_connection_limit= cfg->http_per_ip_connection_limit,
            .session_idle_timeout   = cfg->http_session_idle_timeout,
            .auth_token             = cfg->http_auth_token,
            .allow_ips              = cfg->http_allow_ips,
            .tls_cert_path          = cfg->tls_cert,
            .tls_key_path           = cfg->tls_key,
            .mcp_path               = "/mcp",
        };

        err = mnemon_http_start(&http, &http_cfg, dispatch);
        if (err != MNEMON_OK) {
            mnemon_log(MNEMON_LOG_ERROR, "HTTP transport failed: %s",
                       mnemon_err_msg());
            exit_code = EXIT_FAILURE;
            goto cleanup_dispatch;
        }

        /* Register SSE notifier so dispatch can push events to HTTP clients */
        mnemon_dispatch_set_notifier(dispatch, sse_notify_fn, http);

        /* Wire honeypot for auth brute-force detection */
        if (honeypot)
            mnemon_http_set_honeypot(http, honeypot);
    } else if (mode != MODE_STDIO && !cfg->http_enabled) {
        mnemon_log(MNEMON_LOG_WARNING,
                   "running in %s mode but [http] enabled = false -- "
                   "no network listener. Set enabled = true in config.",
                   mode == MODE_DAEMON ? "daemon" : "foreground");
    }

    /* Notify systemd if in foreground mode */
    if (mode == MODE_FOREGROUND)
        mnemon_sd_notify_ready();

    /* Optional diagnostic heartbeat thread */
    pthread_t hb_thread;
    bool hb_started = false;
    heartbeat_ctx_t hb_ctx = {
        .interval_secs = cfg->diag_heartbeat_secs,
        .http          = http,
        .reader_pool   = reader_pool,
    };
    if (mode != MODE_STDIO && cfg->diag_heartbeat_secs > 0) {
        if (pthread_create(&hb_thread, NULL, heartbeat_thread, &hb_ctx) == 0) {
            hb_started = true;
            mnemon_log(MNEMON_LOG_INFO,
                       "diagnostic heartbeat enabled (every %ds)",
                       cfg->diag_heartbeat_secs);
        } else {
            mnemon_log(MNEMON_LOG_WARNING,
                       "failed to start heartbeat thread");
        }
    }

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

                if (http) {
                    mnemon_http_stats_t hs;
                    mnemon_http_get_stats(http, &hs);
                    mnemon_log(MNEMON_LOG_INFO,
                               "STATS http: sessions=%d connections=%d/%d "
                               "in_flight=%d total=%llu slow=%llu rejected=%llu",
                               hs.sessions, hs.open_connections,
                               hs.max_connections, hs.in_flight_requests,
                               (unsigned long long)hs.total_requests,
                               (unsigned long long)hs.slow_requests,
                               (unsigned long long)hs.rejected_connections);
                }

                if (reader_pool) {
                    int qd = 0, busy = 0, sz = 0;
                    mnemon_reader_pool_stats(reader_pool, &qd, &busy, &sz);
                    mnemon_log(MNEMON_LOG_INFO,
                               "STATS reader_pool: size=%d busy=%d queued=%d",
                               sz, busy, qd);
                }
            }
        }
    }

    /* --- Shutdown sequence --- */
    mnemon_log(MNEMON_LOG_INFO, "shutting down (timeout %ds)...",
               SHUTDOWN_TIMEOUT_SEC);

    /* Start watchdog thread to force-exit if shutdown hangs */
    {
        pthread_t wd;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&wd, &attr, shutdown_watchdog, NULL);
        pthread_attr_destroy(&attr);
    }

    if (mode == MODE_FOREGROUND || mode == MODE_DAEMON)
        mnemon_sd_notify_stopping();

    /* Stop heartbeat thread (it polls shutdown flag every 1s) */
    if (hb_started)
        pthread_join(hb_thread, NULL);

    /* Stop HTTP transport */
    if (http)
        mnemon_http_stop(http);

cleanup_dispatch:
    if (dispatch)
        mnemon_dispatch_free(dispatch);

cleanup_storage:
    if (reader_pool)
        mnemon_reader_pool_stop(reader_pool);
    if (honeypot)
        mnemon_honeypot_free(honeypot);
    if (storage)
        mnemon_storage_close(storage);

cleanup_config:
    mnemon_config_free(cfg);

    mnemon_log(MNEMON_LOG_INFO, "%s shutdown complete", PACKAGE_NAME);
    mnemon_log_shutdown();

    return exit_code;
}
