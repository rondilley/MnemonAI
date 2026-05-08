/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * config_parse.h -- INI configuration parser interface
 */

#ifndef MNEMON_CONFIG_PARSE_H
#define MNEMON_CONFIG_PARSE_H

#include "mnemon.h"

typedef struct mnemon_config {
    /* [general] */
    char    *data_dir;
    char    *log_level;
    bool     foreground;
    /* [lmdb] */
    int      map_size_gb;
    int      max_readers;
    /* [embedding] */
    char    *model_path;
    int      dimensions;
    int      batch_size;
    int      gpu_layers;
    char    *model_sha256;
    /* [extraction] */
    bool     extraction_enabled;
    char    *extraction_endpoint;
    int      extraction_timeout_ms;
    char    *extraction_model;
    /* [search] */
    int      default_top_k;
    int      max_top_k;
    int      rrf_k;
    /* [consolidation] */
    int      consolidation_threshold;
    int      consolidation_interval_sec;
    float    similarity_threshold;
    /* [decay] */
    int      half_life_days;
    float    min_importance;
    /* [import] */
    char    *allowed_paths;
    int      max_batch_size;
    int      default_chunk_size;
    int      writer_batch_size;
    /* [security] */
    bool     detect_secrets;
    int      max_memory_size_kb;
    /* [threads] */
    int      reader_pool_size;
    int      write_queue_depth;
    /* [http] */
    bool     http_enabled;
    char    *http_bind;
    int      http_port;
    int      http_max_connections;
    int      http_connection_timeout;     /* seconds idle before MHD reaps; 0 = no timeout */
    int      http_per_ip_connection_limit;/* per-IP cap; 0 = unlimited */
    int      http_session_idle_timeout;   /* seconds idle before MCP session is reaped; 0 = never */
    char    *http_auth_token;
    char    *http_allow_ips;     /* comma-separated CIDR list, NULL = allow all */
    char    *tls_cert;
    char    *tls_key;
    /* [diag] */
    int      diag_heartbeat_secs;   /* 0 = disabled */
} mnemon_config_t;

mnemon_err_t mnemon_config_load(const char *path, mnemon_config_t **out);
mnemon_err_t mnemon_config_validate(const mnemon_config_t *cfg);
void         mnemon_config_free(mnemon_config_t *cfg);

#endif /* MNEMON_CONFIG_PARSE_H */
