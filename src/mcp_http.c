/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mcp_http.c -- MCP Streamable HTTP transport (MCP spec 2025-03-26)
 *
 * Uses libmicrohttpd to serve a single /mcp endpoint:
 *   POST: receive JSON-RPC requests, return JSON responses
 *   GET:  open SSE stream for server-initiated messages (optional)
 *   DELETE: terminate a session
 *
 * Each client gets a session (Mcp-Session-Id header).
 * All sessions share the same mnemon_dispatch_t (and thus the same storage).
 *
 * Security:
 *   - Origin header validated on all requests
 *   - Bearer token auth when configured
 *   - Refuses to bind to non-localhost without auth_token
 *   - TLS via libmicrohttpd when cert/key configured
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef ENABLE_HTTP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <microhttpd.h>

#include <cJSON.h>

#include "mcp_http.h"
#include "mcp_dispatch.h"
#include "sse.h"
#include "honeypot.h"
#include "id.h"
#include "memory.h"
#include "log.h"
#include "threads.h"

#define MAX_SESSIONS 256
#define MAX_POST_SIZE (2 * 1024 * 1024) /* 2MB max request body */

/* ---- Session management ---- */

typedef struct {
    char id[64];          /* Mcp-Session-Id (UUID-based) */
    bool initialized;     /* Has completed MCP initialize handshake */
    int64_t created_at;
    int64_t last_active;
    sse_queue_t *sse_queue;  /* Non-NULL when client has an active SSE stream */
} http_session_t;

struct mnemon_http {
    struct MHD_Daemon  *daemon;
    mnemon_dispatch_t  *dispatch;
    http_session_t      sessions[MAX_SESSIONS];
    int                 session_count;
    pthread_mutex_t     session_mutex;
    char               *auth_token;
    char               *mcp_path;
    char               *tls_cert_mem;   /* PEM cert kept alive for MHD */
    char               *tls_key_mem;    /* PEM key kept alive for MHD */
    mnemon_honeypot_t  *honeypot;      /* Abuse detection (may be NULL) */
};

/* Per-request upload accumulator */
typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} upload_buf_t;

/* ---- Session helpers ---- */

static http_session_t *find_session(mnemon_http_t *h, const char *id)
{
    for (int i = 0; i < h->session_count; i++) {
        if (strcmp(h->sessions[i].id, id) == 0)
            return &h->sessions[i];
    }
    return NULL;
}

static http_session_t *create_session(mnemon_http_t *h)
{
    if (h->session_count >= MAX_SESSIONS)
        return NULL;

    http_session_t *s = &h->sessions[h->session_count];
    mnemon_uuid_t u;
    mnemon_uuid_generate(&u);
    mnemon_uuid_to_string(&u, s->id, sizeof(s->id));
    s->initialized = false;
    s->created_at = mnemon_time_ms();
    s->last_active = s->created_at;
    h->session_count++;
    return s;
}

static void remove_session(mnemon_http_t *h, const char *id)
{
    for (int i = 0; i < h->session_count; i++) {
        if (strcmp(h->sessions[i].id, id) == 0) {
            if (h->sessions[i].sse_queue) {
                sse_queue_close(h->sessions[i].sse_queue);
                sse_queue_destroy(h->sessions[i].sse_queue);
                free(h->sessions[i].sse_queue);
                h->sessions[i].sse_queue = NULL;
            }
            h->sessions[i] = h->sessions[h->session_count - 1];
            h->session_count--;
            return;
        }
    }
}

/* ---- Auth + validation ---- */

/* Get client IP string from connection (best-effort) */
static const char *get_client_ip(struct MHD_Connection *conn)
{
    const union MHD_ConnectionInfo *ci =
        MHD_get_connection_info(conn, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    if (!ci || !ci->client_addr)
        return "unknown";
    /* For simplicity, use the raw sockaddr; inet_ntop would be ideal but
     * the honeypot only needs a consistent key per client */
    static _Thread_local char ip_buf[48];
    const struct sockaddr *sa = ci->client_addr;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        snprintf(ip_buf, sizeof(ip_buf), "%u.%u.%u.%u",
                 (unsigned)(ntohl(sin->sin_addr.s_addr) >> 24) & 0xFF,
                 (unsigned)(ntohl(sin->sin_addr.s_addr) >> 16) & 0xFF,
                 (unsigned)(ntohl(sin->sin_addr.s_addr) >> 8) & 0xFF,
                 (unsigned)(ntohl(sin->sin_addr.s_addr)) & 0xFF);
        return ip_buf;
    }
    return "unknown";
}

static bool check_auth(mnemon_http_t *h, struct MHD_Connection *conn)
{
    if (!h->auth_token)
        return true; /* no auth configured */

    const char *auth = MHD_lookup_connection_value(conn, MHD_HEADER_KIND,
                                                    "Authorization");
    bool ok = false;
    if (auth && strncmp(auth, "Bearer ", 7) == 0)
        ok = (strcmp(auth + 7, h->auth_token) == 0);

    /* Track auth attempts for brute-force detection */
    if (h->honeypot) {
        const char *ip = get_client_ip(conn);
        if (mnemon_honeypot_auth_attempt(h->honeypot, ip, ok))
            return false; /* Rate-limited: reject even valid tokens */
    }

    return ok;
}

static bool check_origin(struct MHD_Connection *conn)
{
    const char *origin = MHD_lookup_connection_value(conn, MHD_HEADER_KIND,
                                                      "Origin");
    /* No Origin header is fine (curl, non-browser clients) */
    if (!origin) return true;

    /* Block suspicious origins that aren't localhost */
    if (strstr(origin, "localhost") || strstr(origin, "127.0.0.1") ||
        strstr(origin, "[::1]"))
        return true;

    /* For non-localhost origins, block to prevent DNS rebinding */
    mnemon_log(MNEMON_LOG_WARNING, "HTTP: blocked request from origin: %s",
               origin);
    return false;
}

/* ---- HTTP error responses ---- */

static enum MHD_Result respond_error(struct MHD_Connection *conn,
                                      unsigned int status, const char *msg)
{
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        strlen(body), body, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(resp, "Content-Type", "application/json");
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    enum MHD_Result ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* ---- POST handler: JSON-RPC request -> JSON response ---- */

static enum MHD_Result handle_post(mnemon_http_t *h,
                                    struct MHD_Connection *conn,
                                    upload_buf_t *upload)
{
    if (!upload->data || upload->len == 0)
        return respond_error(conn, MHD_HTTP_BAD_REQUEST, "empty body");

    /* Parse JSON-RPC */
    cJSON *request = cJSON_Parse(upload->data);
    if (!request)
        return respond_error(conn, MHD_HTTP_BAD_REQUEST, "invalid JSON");

    /* Check if this is an initialize request (creates session) */
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(request, "method");
    bool is_init = (cJSON_IsString(method) &&
                    strcmp(method->valuestring, "initialize") == 0);

    /* Get or validate session */
    const char *session_id = MHD_lookup_connection_value(
        conn, MHD_HEADER_KIND, "Mcp-Session-Id");

    http_session_t *session = NULL;

    pthread_mutex_lock(&h->session_mutex);

    if (is_init) {
        /* Create new session */
        session = create_session(h);
        if (!session) {
            pthread_mutex_unlock(&h->session_mutex);
            cJSON_Delete(request);
            return respond_error(conn, MHD_HTTP_SERVICE_UNAVAILABLE,
                                 "max sessions reached");
        }
        mnemon_log(MNEMON_LOG_INFO, "HTTP: new session %s", session->id);
    } else if (session_id) {
        session = find_session(h, session_id);
        if (!session) {
            pthread_mutex_unlock(&h->session_mutex);
            cJSON_Delete(request);
            return respond_error(conn, MHD_HTTP_NOT_FOUND, "session expired");
        }
        session->last_active = mnemon_time_ms();
    }
    /* No session_id and not initialize: allow for notifications/responses */

    pthread_mutex_unlock(&h->session_mutex);

    /* Check for notification (no id field -> no response needed) */
    const cJSON *id_field = cJSON_GetObjectItemCaseSensitive(request, "id");
    if (!id_field && !is_init) {
        /* Notification: dispatch but return 202 Accepted */
        cJSON *resp_json = mnemon_dispatch_request(h->dispatch, request);
        cJSON_Delete(request);
        if (resp_json) cJSON_Delete(resp_json);

        struct MHD_Response *resp = MHD_create_response_from_buffer(
            0, NULL, MHD_RESPMEM_PERSISTENT);
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_ACCEPTED, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* Dispatch the request */
    cJSON *response = mnemon_dispatch_request(h->dispatch, request);
    cJSON_Delete(request);

    if (!response)
        return respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             "dispatch returned null");

    /* Serialize response */
    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (!json_str)
        return respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             "JSON serialization failed");

    /* Build HTTP response */
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        strlen(json_str), json_str, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(resp, "Content-Type", "application/json");
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");

    /* Include session ID on initialize response */
    if (is_init && session)
        MHD_add_response_header(resp, "Mcp-Session-Id", session->id);

    enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* ---- DELETE handler: terminate session ---- */

static enum MHD_Result handle_delete(mnemon_http_t *h,
                                      struct MHD_Connection *conn)
{
    const char *session_id = MHD_lookup_connection_value(
        conn, MHD_HEADER_KIND, "Mcp-Session-Id");
    if (!session_id)
        return respond_error(conn, MHD_HTTP_BAD_REQUEST, "missing session ID");

    pthread_mutex_lock(&h->session_mutex);
    http_session_t *session = find_session(h, session_id);
    if (session) {
        mnemon_log(MNEMON_LOG_INFO, "HTTP: session terminated: %s",
                   session_id);
        remove_session(h, session_id);
    }
    pthread_mutex_unlock(&h->session_mutex);

    struct MHD_Response *resp = MHD_create_response_from_buffer(
        0, NULL, MHD_RESPMEM_PERSISTENT);
    enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* ---- OPTIONS handler: CORS preflight ---- */

static enum MHD_Result handle_options(struct MHD_Connection *conn)
{
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        0, NULL, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(resp, "Access-Control-Allow-Methods",
                            "GET, POST, DELETE, OPTIONS");
    MHD_add_response_header(resp, "Access-Control-Allow-Headers",
                            "Content-Type, Authorization, Mcp-Session-Id, Accept");
    MHD_add_response_header(resp, "Access-Control-Max-Age", "86400");
    enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_NO_CONTENT, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* ---- SSE content reader callback for MHD streaming ---- */

static ssize_t sse_content_reader(void *cls, uint64_t pos, char *buf,
                                  size_t max)
{
    (void)pos;
    sse_queue_t *q = (sse_queue_t *)cls;
    sse_event_t event;

    mnemon_err_t err = sse_queue_pop(q, &event, 200);
    if (err == MNEMON_ERR_SHUTDOWN)
        return MHD_CONTENT_READER_END_OF_STREAM;
    if (err == MNEMON_ERR_NOT_FOUND)
        return 0; /* No data yet -- MHD will call again */

    /* Format SSE: "event: <type>\ndata: <json>\n\n" */
    int n = snprintf(buf, max, "event: %s\ndata: %s\n\n",
                     event.event_type, event.data);
    free(event.event_type);
    free(event.data);

    if (n < 0) return MHD_CONTENT_READER_END_WITH_ERROR;
    return (ssize_t)(n < (int)max ? n : (int)max);
}

static void sse_content_free(void *cls)
{
    sse_queue_t *q = (sse_queue_t *)cls;
    if (q)
        sse_queue_close(q);
}

/* ---- GET handler: SSE stream for server-initiated messages ---- */

static enum MHD_Result handle_get(mnemon_http_t *h,
                                  struct MHD_Connection *conn)
{
    const char *session_id = MHD_lookup_connection_value(
        conn, MHD_HEADER_KIND, "Mcp-Session-Id");
    if (!session_id)
        return respond_error(conn, MHD_HTTP_BAD_REQUEST,
                             "missing Mcp-Session-Id");

    pthread_mutex_lock(&h->session_mutex);
    http_session_t *session = find_session(h, session_id);
    if (!session) {
        pthread_mutex_unlock(&h->session_mutex);
        return respond_error(conn, MHD_HTTP_NOT_FOUND, "session not found");
    }

    /* Create SSE queue if not already present */
    if (!session->sse_queue) {
        session->sse_queue = calloc(1, sizeof(sse_queue_t));
        if (!session->sse_queue) {
            pthread_mutex_unlock(&h->session_mutex);
            return respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "OOM");
        }
        if (sse_queue_init(session->sse_queue) != MNEMON_OK) {
            free(session->sse_queue);
            session->sse_queue = NULL;
            pthread_mutex_unlock(&h->session_mutex);
            return respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                 "queue init failed");
        }
    }
    sse_queue_t *q = session->sse_queue;
    pthread_mutex_unlock(&h->session_mutex);

    /* Create streaming response */
    struct MHD_Response *resp = MHD_create_response_from_callback(
        MHD_SIZE_UNKNOWN, 4096, sse_content_reader, q, sse_content_free);
    if (!resp)
        return respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             "failed to create SSE response");

    MHD_add_response_header(resp, "Content-Type", "text/event-stream");
    MHD_add_response_header(resp, "Cache-Control", "no-cache");
    MHD_add_response_header(resp, "Connection", "keep-alive");
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(resp, "Mcp-Session-Id", session->id);

    mnemon_log(MNEMON_LOG_INFO, "HTTP: SSE stream opened for session %s",
               session->id);

    enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* ---- Main request handler (libmicrohttpd callback) ---- */

static enum MHD_Result request_handler(void *cls,
                                        struct MHD_Connection *conn,
                                        const char *url,
                                        const char *method,
                                        const char *version,
                                        const char *upload_data,
                                        size_t *upload_data_size,
                                        void **con_cls)
{
    mnemon_http_t *h = (mnemon_http_t *)cls;
    (void)version;

    /* CORS preflight */
    if (strcmp(method, "OPTIONS") == 0)
        return handle_options(conn);

    /* Validate endpoint path */
    if (strcmp(url, h->mcp_path) != 0)
        return respond_error(conn, MHD_HTTP_NOT_FOUND, "not found");

    /* Origin validation */
    if (!check_origin(conn))
        return respond_error(conn, MHD_HTTP_FORBIDDEN, "origin blocked");

    /* Auth check */
    if (!check_auth(h, conn))
        return respond_error(conn, MHD_HTTP_UNAUTHORIZED, "unauthorized");

    /* GET: SSE stream for server-initiated messages */
    if (strcmp(method, "GET") == 0)
        return handle_get(h, conn);

    /* DELETE: terminate session */
    if (strcmp(method, "DELETE") == 0)
        return handle_delete(h, conn);

    /* POST: JSON-RPC request */
    if (strcmp(method, "POST") != 0)
        return respond_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED,
                             "method not allowed");

    /* Accumulate upload data across multiple callbacks */
    upload_buf_t *upload = (upload_buf_t *)*con_cls;
    if (!upload) {
        upload = calloc(1, sizeof(upload_buf_t));
        if (!upload)
            return MHD_NO;
        *con_cls = upload;
        return MHD_YES; /* First call: just set up the context */
    }

    if (*upload_data_size > 0) {
        /* Accumulate POST body */
        if (upload->len + *upload_data_size > MAX_POST_SIZE) {
            return respond_error(conn, MHD_HTTP_CONTENT_TOO_LARGE,
                                 "request body too large");
        }
        if (upload->len + *upload_data_size >= upload->cap) {
            size_t newcap = upload->cap ? upload->cap * 2 : 4096;
            while (newcap < upload->len + *upload_data_size + 1) newcap *= 2;
            char *p = realloc(upload->data, newcap);
            if (!p) return MHD_NO;
            upload->data = p;
            upload->cap = newcap;
        }
        memcpy(upload->data + upload->len, upload_data, *upload_data_size);
        upload->len += *upload_data_size;
        upload->data[upload->len] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }

    /* All data received -- process the request */
    enum MHD_Result ret = handle_post(h, conn, upload);

    /* Cleanup upload buffer */
    free(upload->data);
    free(upload);
    *con_cls = NULL;

    return ret;
}

/* ---- Cleanup callback (called when connection closes) ---- */

static void request_completed(void *cls, struct MHD_Connection *conn,
                               void **con_cls,
                               enum MHD_RequestTerminationCode toe)
{
    (void)cls;
    (void)conn;
    (void)toe;

    upload_buf_t *upload = (upload_buf_t *)*con_cls;
    if (upload) {
        free(upload->data);
        free(upload);
        *con_cls = NULL;
    }
}

/* ---- Public API ---- */

mnemon_err_t mnemon_http_start(mnemon_http_t **out,
                               const mnemon_http_config_t *cfg,
                               mnemon_dispatch_t *dispatch)
{
    if (!out || !cfg || !dispatch)
        return MNEMON_ERR_INVALID_INPUT;

    /* Security: refuse non-localhost binding without auth token */
    if (cfg->bind_address &&
        strcmp(cfg->bind_address, "127.0.0.1") != 0 &&
        strcmp(cfg->bind_address, "localhost") != 0 &&
        strcmp(cfg->bind_address, "::1") != 0 &&
        (!cfg->auth_token || cfg->auth_token[0] == '\0')) {
        mnemon_err_set(MNEMON_ERR_INVALID_INPUT, 0,
                       "HTTP: refusing to bind to %s without auth_token -- "
                       "set [http] auth_token in config", cfg->bind_address);
        return MNEMON_ERR_INVALID_INPUT;
    }

    mnemon_http_t *h = calloc(1, sizeof(*h));
    if (!h) return MNEMON_ERR_OOM;

    h->dispatch = dispatch;
    h->auth_token = cfg->auth_token && cfg->auth_token[0]
                    ? strdup(cfg->auth_token) : NULL;
    h->mcp_path = strdup(cfg->mcp_path ? cfg->mcp_path : "/mcp");
    pthread_mutex_init(&h->session_mutex, NULL);

    int port = cfg->port > 0 ? cfg->port : 3847;

    unsigned int flags = MHD_USE_INTERNAL_POLLING_THREAD |
                         MHD_USE_AUTO | MHD_ALLOW_SUSPEND_RESUME;

    /* Start the daemon */
    if (cfg->tls_cert_path && cfg->tls_key_path) {
        /* Read TLS cert and key files */
        FILE *cf = fopen(cfg->tls_cert_path, "r");
        FILE *kf = fopen(cfg->tls_key_path, "r");
        if (!cf || !kf) {
            if (cf) fclose(cf);
            if (kf) fclose(kf);
            free(h->auth_token);
            free(h->mcp_path);
            free(h);
            mnemon_err_set(MNEMON_ERR_IO, 0,
                           "cannot read TLS cert/key files");
            return MNEMON_ERR_IO;
        }

        fseek(cf, 0, SEEK_END); long cert_len = ftell(cf); fseek(cf, 0, SEEK_SET);
        fseek(kf, 0, SEEK_END); long key_len = ftell(kf); fseek(kf, 0, SEEK_SET);

        h->tls_cert_mem = malloc((size_t)cert_len + 1);
        h->tls_key_mem = malloc((size_t)key_len + 1);
        if (!h->tls_cert_mem || !h->tls_key_mem) {
            fclose(cf); fclose(kf);
            free(h->tls_cert_mem); free(h->tls_key_mem);
            free(h->auth_token); free(h->mcp_path);
            pthread_mutex_destroy(&h->session_mutex);
            free(h);
            return MNEMON_ERR_OOM;
        }
        if (fread(h->tls_cert_mem, 1, (size_t)cert_len, cf) != (size_t)cert_len ||
            fread(h->tls_key_mem, 1, (size_t)key_len, kf) != (size_t)key_len) {
            fclose(cf); fclose(kf);
            free(h->tls_cert_mem); free(h->tls_key_mem);
            free(h->auth_token); free(h->mcp_path);
            pthread_mutex_destroy(&h->session_mutex);
            free(h);
            mnemon_err_set(MNEMON_ERR_IO, 0, "short read on TLS cert/key files");
            return MNEMON_ERR_IO;
        }
        h->tls_cert_mem[cert_len] = '\0';
        h->tls_key_mem[key_len] = '\0';
        fclose(cf);
        fclose(kf);

        flags |= MHD_USE_TLS;
        h->daemon = MHD_start_daemon(
            flags, (uint16_t)port, NULL, NULL,
            request_handler, h,
            MHD_OPTION_NOTIFY_COMPLETED, request_completed, NULL,
            MHD_OPTION_CONNECTION_LIMIT, (unsigned int)cfg->max_connections,
            MHD_OPTION_HTTPS_MEM_KEY, h->tls_key_mem,
            MHD_OPTION_HTTPS_MEM_CERT, h->tls_cert_mem,
            MHD_OPTION_END);
    } else {
        h->daemon = MHD_start_daemon(
            flags, (uint16_t)port, NULL, NULL,
            request_handler, h,
            MHD_OPTION_NOTIFY_COMPLETED, request_completed, NULL,
            MHD_OPTION_CONNECTION_LIMIT, (unsigned int)cfg->max_connections,
            MHD_OPTION_END);
    }

    if (!h->daemon) {
        mnemon_err_set(MNEMON_ERR_INTERNAL, 0,
                       "MHD_start_daemon failed on port %d", port);
        free(h->auth_token);
        free(h->mcp_path);
        free(h->tls_cert_mem);
        free(h->tls_key_mem);
        pthread_mutex_destroy(&h->session_mutex);
        free(h);
        return MNEMON_ERR_INTERNAL;
    }

    mnemon_log(MNEMON_LOG_INFO,
               "HTTP transport started: %s:%d%s (auth=%s, tls=%s)",
               cfg->bind_address ? cfg->bind_address : "0.0.0.0",
               port, h->mcp_path,
               h->auth_token ? "yes" : "no",
               (cfg->tls_cert_path && cfg->tls_key_path) ? "yes" : "no");

    *out = h;
    return MNEMON_OK;
}

void mnemon_http_stop(mnemon_http_t *h)
{
    if (!h) return;

    mnemon_log(MNEMON_LOG_INFO, "HTTP transport stopping (%d active sessions)",
               h->session_count);

    /* Close all SSE queues first (unblocks streaming callbacks) */
    for (int i = 0; i < h->session_count; i++) {
        if (h->sessions[i].sse_queue) {
            sse_queue_close(h->sessions[i].sse_queue);
        }
    }

    if (h->daemon)
        MHD_stop_daemon(h->daemon);

    /* Now destroy and free SSE queues (after daemon stopped) */
    for (int i = 0; i < h->session_count; i++) {
        if (h->sessions[i].sse_queue) {
            sse_queue_destroy(h->sessions[i].sse_queue);
            free(h->sessions[i].sse_queue);
        }
    }

    free(h->auth_token);
    free(h->mcp_path);
    free(h->tls_cert_mem);
    free(h->tls_key_mem);
    pthread_mutex_destroy(&h->session_mutex);
    free(h);
}

int mnemon_http_session_count(const mnemon_http_t *h)
{
    return h ? h->session_count : 0;
}

mnemon_err_t mnemon_http_push_event(mnemon_http_t *h, const char *session_id,
                                    const char *event_type,
                                    const char *json_data)
{
    if (!h || !session_id || !event_type || !json_data)
        return MNEMON_ERR_INVALID_INPUT;

    pthread_mutex_lock(&h->session_mutex);
    http_session_t *session = find_session(h, session_id);
    if (!session || !session->sse_queue) {
        pthread_mutex_unlock(&h->session_mutex);
        return MNEMON_ERR_NOT_FOUND;
    }
    sse_queue_t *q = session->sse_queue;
    pthread_mutex_unlock(&h->session_mutex);

    return sse_queue_push(q, event_type, json_data);
}

void mnemon_http_broadcast_event(mnemon_http_t *h, const char *event_type,
                                 const char *json_data)
{
    if (!h || !event_type || !json_data) return;

    pthread_mutex_lock(&h->session_mutex);
    for (int i = 0; i < h->session_count; i++) {
        if (h->sessions[i].sse_queue)
            sse_queue_push(h->sessions[i].sse_queue, event_type, json_data);
    }
    pthread_mutex_unlock(&h->session_mutex);
}

void mnemon_http_set_honeypot(mnemon_http_t *h, mnemon_honeypot_t *hp)
{
    if (h) h->honeypot = hp;
}

#else /* !ENABLE_HTTP */

/* Stubs when libmicrohttpd is not available */

mnemon_err_t mnemon_http_start(mnemon_http_t **out,
                               const mnemon_http_config_t *cfg,
                               mnemon_dispatch_t *dispatch)
{
    (void)out; (void)cfg; (void)dispatch;
    mnemon_err_set(MNEMON_ERR_INTERNAL, 0,
                   "HTTP transport not available (build with -DENABLE_HTTP=ON "
                   "and install libmicrohttpd-dev)");
    return MNEMON_ERR_INTERNAL;
}

void mnemon_http_stop(mnemon_http_t *h) { (void)h; }
int mnemon_http_session_count(const mnemon_http_t *h) { (void)h; return 0; }
mnemon_err_t mnemon_http_push_event(mnemon_http_t *h, const char *session_id,
                                    const char *event_type,
                                    const char *json_data)
{ (void)h; (void)session_id; (void)event_type; (void)json_data; return MNEMON_ERR_INTERNAL; }
void mnemon_http_broadcast_event(mnemon_http_t *h, const char *event_type,
                                 const char *json_data)
{ (void)h; (void)event_type; (void)json_data; }
void mnemon_http_set_honeypot(mnemon_http_t *h, mnemon_honeypot_t *hp)
{ (void)h; (void)hp; }

#endif /* ENABLE_HTTP */
