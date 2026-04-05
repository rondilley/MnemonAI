/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * config.c -- INI configuration parser
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include "config_parse.h"
#include "log.h"

static char *tilde_expand(const char *path)
{
    if (!path || path[0] != '~') return path ? strdup(path) : NULL;
    const char *home = getenv("HOME");
    if (!home) return strdup(path);
    size_t hlen = strlen(home);
    size_t plen = strlen(path + 1);
    char *buf = malloc(hlen + plen + 1);
    if (!buf) return NULL;
    memcpy(buf, home, hlen);
    memcpy(buf + hlen, path + 1, plen + 1);
    return buf;
}

static char *strip(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

static void set_defaults(mnemon_config_t *cfg)
{
    cfg->data_dir = tilde_expand("~/.local/share/mnemond");
    cfg->log_level = strdup("info");
    cfg->foreground = false;
    cfg->map_size_gb = 10;
    cfg->max_readers = 64;
    cfg->model_path = tilde_expand(
        "~/.local/share/mnemond/models/nomic-embed-text-v1.5.Q8_0.gguf");
    cfg->dimensions = 768;
    cfg->batch_size = 32;
    cfg->gpu_layers = 99;
    cfg->model_sha256 = NULL;
    cfg->extraction_enabled = false;
    cfg->extraction_endpoint = strdup(
        "http://127.0.0.1:8080/v1/chat/completions");
    cfg->extraction_timeout_ms = 10000;
    cfg->extraction_model = NULL;
    cfg->default_top_k = 10;
    cfg->max_top_k = 50;
    cfg->rrf_k = 60;
    cfg->consolidation_threshold = 100;
    cfg->consolidation_interval_sec = 3600;
    cfg->similarity_threshold = 0.7f;
    cfg->half_life_days = 90;
    cfg->min_importance = 0.01f;
    cfg->allowed_paths = strdup("~");
    cfg->max_batch_size = 1000;
    cfg->default_chunk_size = 4096;
    cfg->writer_batch_size = 50;
    cfg->detect_secrets = true;
    cfg->max_memory_size_kb = 64;
    cfg->reader_pool_size = 0;
    cfg->write_queue_depth = 1024;
    cfg->http_enabled = false;
    cfg->http_bind = strdup("127.0.0.1");
    cfg->http_port = 3847;
    cfg->http_max_connections = 32;
    cfg->http_auth_token = NULL;
    cfg->tls_cert = NULL;
    cfg->tls_key = NULL;
}

static void set_value(mnemon_config_t *cfg, const char *section,
                      const char *key, const char *value)
{
    #define STR_SET(field) do { free(cfg->field); cfg->field = tilde_expand(value); } while(0)
    #define INT_SET(field) do { cfg->field = atoi(value); } while(0)
    #define BOOL_SET(field) do { cfg->field = (strcmp(value,"true")==0 || strcmp(value,"1")==0 || strcmp(value,"yes")==0); } while(0)
    #define FLOAT_SET(field) do { cfg->field = (float)atof(value); } while(0)

    if (strcmp(section, "general") == 0) {
        if (strcmp(key, "data_dir") == 0) STR_SET(data_dir);
        else if (strcmp(key, "log_level") == 0) { free(cfg->log_level); cfg->log_level = strdup(value); }
        else if (strcmp(key, "foreground") == 0) BOOL_SET(foreground);
    } else if (strcmp(section, "lmdb") == 0) {
        if (strcmp(key, "map_size_gb") == 0) INT_SET(map_size_gb);
        else if (strcmp(key, "max_readers") == 0) INT_SET(max_readers);
    } else if (strcmp(section, "embedding") == 0) {
        if (strcmp(key, "model_path") == 0) STR_SET(model_path);
        else if (strcmp(key, "dimensions") == 0) INT_SET(dimensions);
        else if (strcmp(key, "batch_size") == 0) INT_SET(batch_size);
        else if (strcmp(key, "gpu_layers") == 0) INT_SET(gpu_layers);
        else if (strcmp(key, "model_sha256") == 0 && strlen(value) > 0) {
            free(cfg->model_sha256);
            cfg->model_sha256 = strdup(value);
        }
    } else if (strcmp(section, "extraction") == 0) {
        if (strcmp(key, "enabled") == 0) BOOL_SET(extraction_enabled);
        else if (strcmp(key, "endpoint") == 0) { free(cfg->extraction_endpoint); cfg->extraction_endpoint = strdup(value); }
        else if (strcmp(key, "timeout_ms") == 0) INT_SET(extraction_timeout_ms);
        else if (strcmp(key, "model") == 0 && strlen(value) > 0) {
            free(cfg->extraction_model);
            cfg->extraction_model = strdup(value);
        }
    } else if (strcmp(section, "search") == 0) {
        if (strcmp(key, "default_top_k") == 0) INT_SET(default_top_k);
        else if (strcmp(key, "max_top_k") == 0) INT_SET(max_top_k);
        else if (strcmp(key, "rrf_k") == 0) INT_SET(rrf_k);
    } else if (strcmp(section, "consolidation") == 0) {
        if (strcmp(key, "threshold") == 0) INT_SET(consolidation_threshold);
        else if (strcmp(key, "interval_sec") == 0) INT_SET(consolidation_interval_sec);
        else if (strcmp(key, "similarity_threshold") == 0) FLOAT_SET(similarity_threshold);
    } else if (strcmp(section, "decay") == 0) {
        if (strcmp(key, "half_life_days") == 0) INT_SET(half_life_days);
        else if (strcmp(key, "min_importance") == 0) FLOAT_SET(min_importance);
    } else if (strcmp(section, "import") == 0) {
        if (strcmp(key, "allowed_paths") == 0) { free(cfg->allowed_paths); cfg->allowed_paths = strdup(value); }
        else if (strcmp(key, "max_batch_size") == 0) INT_SET(max_batch_size);
        else if (strcmp(key, "default_chunk_size") == 0) INT_SET(default_chunk_size);
        else if (strcmp(key, "writer_batch_size") == 0) INT_SET(writer_batch_size);
    } else if (strcmp(section, "security") == 0) {
        if (strcmp(key, "detect_secrets") == 0) BOOL_SET(detect_secrets);
        else if (strcmp(key, "max_memory_size_kb") == 0) INT_SET(max_memory_size_kb);
    } else if (strcmp(section, "threads") == 0) {
        if (strcmp(key, "reader_pool_size") == 0) INT_SET(reader_pool_size);
        else if (strcmp(key, "write_queue_depth") == 0) INT_SET(write_queue_depth);
    } else if (strcmp(section, "http") == 0) {
        if (strcmp(key, "enabled") == 0) BOOL_SET(http_enabled);
        else if (strcmp(key, "bind") == 0) { free(cfg->http_bind); cfg->http_bind = strdup(value); }
        else if (strcmp(key, "port") == 0) INT_SET(http_port);
        else if (strcmp(key, "max_connections") == 0) INT_SET(http_max_connections);
        else if (strcmp(key, "auth_token") == 0 && strlen(value) > 0) {
            free(cfg->http_auth_token);
            cfg->http_auth_token = strdup(value);
        }
        else if (strcmp(key, "tls_cert") == 0) STR_SET(tls_cert);
        else if (strcmp(key, "tls_key") == 0) STR_SET(tls_key);
    }

    #undef STR_SET
    #undef INT_SET
    #undef BOOL_SET
    #undef FLOAT_SET
}

static mnemon_err_t parse_ini(mnemon_config_t *cfg, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return MNEMON_ERR_NOT_FOUND;

    char line[1024];
    char section[64] = "general";

    while (fgets(line, sizeof(line), fp)) {
        char *s = strip(line);
        if (s[0] == '#' || s[0] == ';' || s[0] == '\0')
            continue;
        if (s[0] == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", s + 1);
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = strip(s);
        char *val = strip(eq + 1);
        set_value(cfg, section, key, val);
    }

    fclose(fp);
    return MNEMON_OK;
}

mnemon_err_t mnemon_config_load(const char *path, mnemon_config_t **out)
{
    if (!out) return MNEMON_ERR_INVALID_INPUT;

    mnemon_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) return MNEMON_ERR_OOM;

    set_defaults(cfg);

    if (path) {
        mnemon_err_t err = parse_ini(cfg, path);
        if (err != MNEMON_OK && err != MNEMON_ERR_NOT_FOUND) {
            mnemon_config_free(cfg);
            return err;
        }
    } else {
        /* Search path */
        const char *xdg = getenv("XDG_CONFIG_HOME");
        char search[512];
        bool found = false;

        if (xdg) {
            snprintf(search, sizeof(search), "%s/mnemond/mnemond.conf", xdg);
            if (parse_ini(cfg, search) == MNEMON_OK) found = true;
        }
        if (!found) {
            char *expanded = tilde_expand("~/.config/mnemond/mnemond.conf");
            if (expanded) {
                if (parse_ini(cfg, expanded) == MNEMON_OK) found = true;
                free(expanded);
            }
        }
        if (!found) {
            snprintf(search, sizeof(search), "%s/mnemond.conf", SYSCONFDIR);
            if (parse_ini(cfg, search) == MNEMON_OK) found = true;
        }
        if (!found) {
            parse_ini(cfg, "/etc/mnemond/mnemond.conf");
        }
    }

    *out = cfg;
    return MNEMON_OK;
}

mnemon_err_t mnemon_config_validate(const mnemon_config_t *cfg)
{
    if (!cfg) return MNEMON_ERR_INVALID_INPUT;
    if (cfg->map_size_gb < 1 || cfg->map_size_gb > 1024) {
        mnemon_err_set(MNEMON_ERR_INVALID_INPUT, 0,
                       "map_size_gb must be 1-1024");
        return MNEMON_ERR_INVALID_INPUT;
    }
    if (cfg->dimensions < 1 || cfg->dimensions > 4096) {
        mnemon_err_set(MNEMON_ERR_INVALID_INPUT, 0,
                       "dimensions must be 1-4096");
        return MNEMON_ERR_INVALID_INPUT;
    }
    if (cfg->max_top_k < 1 || cfg->max_top_k > 200) {
        mnemon_err_set(MNEMON_ERR_INVALID_INPUT, 0,
                       "max_top_k must be 1-200");
        return MNEMON_ERR_INVALID_INPUT;
    }
    if ((cfg->tls_cert && !cfg->tls_key) || (!cfg->tls_cert && cfg->tls_key)) {
        mnemon_err_set(MNEMON_ERR_INVALID_INPUT, 0,
                       "tls_cert and tls_key must both be set or both unset");
        return MNEMON_ERR_INVALID_INPUT;
    }
    return MNEMON_OK;
}

void mnemon_config_free(mnemon_config_t *cfg)
{
    if (!cfg) return;

    /* Zero each pointer after freeing to prevent double-free */
    #define FREE_FIELD(f) do { if (cfg->f) { free(cfg->f); cfg->f = NULL; } } while(0)
    FREE_FIELD(data_dir);
    FREE_FIELD(log_level);
    FREE_FIELD(model_path);
    FREE_FIELD(model_sha256);
    FREE_FIELD(extraction_endpoint);
    FREE_FIELD(extraction_model);
    FREE_FIELD(allowed_paths);
    FREE_FIELD(http_bind);
    FREE_FIELD(http_auth_token);
    FREE_FIELD(tls_cert);
    FREE_FIELD(tls_key);
    #undef FREE_FIELD

    free(cfg);
}
