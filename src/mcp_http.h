/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mcp_http.h -- MCP Streamable HTTP transport interface
 *
 * Implements the MCP 2025-03-26 Streamable HTTP transport specification:
 *   - Single /mcp endpoint (POST, GET, DELETE)
 *   - POST: JSON-RPC request -> JSON or SSE response
 *   - GET: open SSE stream for server-initiated messages
 *   - DELETE: terminate session
 *   - Mcp-Session-Id header for session management
 *   - Bearer token authentication
 *   - Origin header validation
 *   - TLS support via libmicrohttpd
 */

#ifndef MNEMON_MCP_HTTP_H
#define MNEMON_MCP_HTTP_H

#include "mnemon.h"
#include "mcp_dispatch.h"

typedef struct mnemon_honeypot mnemon_honeypot_t;

typedef struct mnemon_http mnemon_http_t;

typedef struct {
    const char *bind_address;    /* "127.0.0.1" or "0.0.0.0" */
    int         port;            /* default 3847 */
    int         max_connections; /* default 32 */
    const char *auth_token;      /* Bearer token (NULL = no auth) */
    const char *allow_ips;       /* comma-separated CIDR allow list (NULL = allow all) */
    const char *tls_cert_path;   /* PEM cert file (NULL = no TLS) */
    const char *tls_key_path;    /* PEM key file */
    const char *mcp_path;        /* endpoint path, default "/mcp" */
} mnemon_http_config_t;

/* Start the HTTP server. Returns immediately; server runs on internal threads. */
mnemon_err_t mnemon_http_start(mnemon_http_t **out,
                               const mnemon_http_config_t *cfg,
                               mnemon_dispatch_t *dispatch);

/* Stop the HTTP server and free resources. */
void mnemon_http_stop(mnemon_http_t *h);

/* Get the number of active sessions. */
int mnemon_http_session_count(const mnemon_http_t *h);

/* Push an SSE event to a specific session's stream.
 * No-op if session has no active SSE connection. */
mnemon_err_t mnemon_http_push_event(mnemon_http_t *h, const char *session_id,
                                    const char *event_type,
                                    const char *json_data);

/* Push an SSE event to all sessions with active SSE streams. */
void mnemon_http_broadcast_event(mnemon_http_t *h, const char *event_type,
                                 const char *json_data);

/* Set the honeypot module for auth brute-force detection. */
void mnemon_http_set_honeypot(mnemon_http_t *h, mnemon_honeypot_t *hp);

#endif /* MNEMON_MCP_HTTP_H */
