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
#include <stdatomic.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
#define SLOW_REQUEST_MS 1000
#define CONN_WARN_COOLDOWN_MS 60000

/* ---- Session management ---- */

typedef struct {
    char id[64];          /* Mcp-Session-Id (UUID-based) */
    bool initialized;     /* Has completed MCP initialize handshake */
    int64_t created_at;
    int64_t last_active;
    sse_queue_t *sse_queue;  /* Non-NULL when client has an active SSE stream */
} http_session_t;

/* Parsed CIDR entry for IP allow list */
typedef struct {
    uint32_t network;   /* network address in host byte order */
    uint32_t mask;      /* netmask in host byte order */
} cidr_entry_t;

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
    cidr_entry_t       *allow_list;    /* parsed CIDR entries (NULL = allow all) */
    int                 allow_count;
    int                 max_connections;
    int                 session_idle_timeout_ms;  /* 0 = no idle reaping */

    /* Diagnostic gauges (lock-free) */
    atomic_int          open_connections;
    atomic_int          in_flight_requests;
    atomic_uint_fast64_t total_requests;
    atomic_uint_fast64_t slow_requests;
    atomic_uint_fast64_t rejected_connections;
    atomic_int_fast64_t  last_conn_warn_ms;  /* throttles near-limit warnings */
    int                 last_warn_pct;       /* highest pct already warned */
};

/* Per-request lifecycle + POST upload buffer */
typedef struct {
    /* Upload accumulator (POST only) */
    char   *data;
    size_t  len;
    size_t  cap;
    /* Lifecycle */
    int64_t  started_ms;
    char     method[12];
    char     ip[48];
    char     tool_name[64];     /* set by handle_post when known */
    char     rpc_id[40];        /* JSON-RPC id (string/number) for correlation */
    bool     in_flight_counted; /* did we increment in_flight_requests? */
} request_state_t;

/* ---- Session helpers ---- */

static http_session_t *find_session(mnemon_http_t *h, const char *id)
{
    for (int i = 0; i < h->session_count; i++) {
        if (strcmp(h->sessions[i].id, id) == 0)
            return &h->sessions[i];
    }
    return NULL;
}

static void remove_session(mnemon_http_t *h, const char *id);

/* Reap any session whose last_active is older than session_idle_timeout_ms.
 * Sessions with an open SSE stream are protected -- the SSE TCP close
 * callback (sse_content_free) is what clears their sse_queue pointer, after
 * which they become eligible here.
 * Caller must hold session_mutex. Returns count reaped. */
static int reap_idle_sessions_locked(mnemon_http_t *h)
{
    if (h->session_idle_timeout_ms <= 0) return 0;
    int64_t cutoff = mnemon_time_ms() - h->session_idle_timeout_ms;
    int reaped = 0;
    /* Iterate from end -- remove_session compacts by swapping the last
     * element into the freed slot, so reverse iteration is index-stable. */
    for (int i = h->session_count - 1; i >= 0; i--) {
        if (h->sessions[i].sse_queue) continue;
        if (h->sessions[i].last_active < cutoff) {
            char id[64];
            snprintf(id, sizeof(id), "%s", h->sessions[i].id);
            remove_session(h, id);
            reaped++;
        }
    }
    if (reaped > 0)
        mnemon_log(MNEMON_LOG_INFO,
                   "HTTP: reaped %d idle session(s) (idle > %d s)",
                   reaped, h->session_idle_timeout_ms / 1000);
    return reaped;
}

/* Evict the least-recently-active session, skipping any with an open SSE
 * stream (those are still in active use even if not generating POSTs).
 * Caller must hold session_mutex. Returns true if a session was evicted. */
static bool evict_lru_session_locked(mnemon_http_t *h)
{
    int candidate = -1;
    for (int i = 0; i < h->session_count; i++) {
        if (h->sessions[i].sse_queue)
            continue;  /* protected: active SSE stream */
        if (candidate < 0 ||
            h->sessions[i].last_active < h->sessions[candidate].last_active)
            candidate = i;
    }
    if (candidate < 0)
        return false;  /* every slot has an active SSE stream */

    char id[64];
    int64_t age_ms = mnemon_time_ms() - h->sessions[candidate].last_active;
    snprintf(id, sizeof(id), "%s", h->sessions[candidate].id);
    remove_session(h, id);
    mnemon_log(MNEMON_LOG_INFO,
               "HTTP: evicted idle session %s (idle %lld s) -- "
               "session table at capacity, MCP clients should send DELETE on shutdown",
               id, (long long)(age_ms / 1000));
    return true;
}

static http_session_t *create_session(mnemon_http_t *h)
{
    /* First sweep idle sessions opportunistically. This is the primary
     * mechanism for releasing sessions whose client has gone away (no
     * traffic for session_idle_timeout). LRU eviction below is a
     * last-resort safety net. */
    reap_idle_sessions_locked(h);

    if (h->session_count >= MAX_SESSIONS) {
        if (!evict_lru_session_locked(h))
            return NULL;  /* truly out of capacity (all sessions have SSE) */
    }

    http_session_t *s = &h->sessions[h->session_count];
    mnemon_uuid_t u;
    mnemon_uuid_generate(&u);
    mnemon_uuid_to_string(&u, s->id, sizeof(s->id));
    s->initialized = false;
    s->created_at = mnemon_time_ms();
    s->last_active = s->created_at;
    s->sse_queue = NULL;
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

/* ---- IP allow list (CIDR matching) ---- */

/* Parse "192.168.1.0/24,10.0.0.0/8,172.16.5.10" into cidr_entry_t array.
 * A bare IP without /prefix is treated as /32. */
static int parse_allow_ips(const char *spec, cidr_entry_t **out)
{
    if (!spec || !spec[0]) { *out = NULL; return 0; }

    /* Count commas to estimate array size */
    int capacity = 1;
    for (const char *p = spec; *p; p++)
        if (*p == ',') capacity++;

    cidr_entry_t *list = calloc((size_t)capacity, sizeof(cidr_entry_t));
    if (!list) return 0;

    char *tmp = strdup(spec);
    if (!tmp) { free(list); return 0; }

    int count = 0;
    char *saveptr = NULL;
    for (char *tok = strtok_r(tmp, ",", &saveptr);
         tok && count < capacity;
         tok = strtok_r(NULL, ",", &saveptr))
    {
        /* Strip whitespace */
        while (*tok == ' ') tok++;
        char *end = tok + strlen(tok) - 1;
        while (end > tok && *end == ' ') *end-- = '\0';

        int prefix = 32;
        char *slash = strchr(tok, '/');
        if (slash) {
            *slash = '\0';
            prefix = atoi(slash + 1);
            if (prefix < 0 || prefix > 32) prefix = 32;
        }

        struct in_addr addr;
        if (inet_pton(AF_INET, tok, &addr) == 1) {
            uint32_t mask = prefix == 0 ? 0 : ~((uint32_t)0) << (32 - prefix);
            list[count].network = ntohl(addr.s_addr) & mask;
            list[count].mask = mask;
            count++;
        } else {
            mnemon_log(MNEMON_LOG_WARNING,
                       "HTTP: ignoring invalid allow_ips entry: %s", tok);
        }
    }

    free(tmp);
    *out = list;
    return count;
}

/* Check if a client IP is in the allow list.
 * Returns true if allowed (no list configured, or IP matches an entry). */
static bool check_ip_allowed(mnemon_http_t *h, struct MHD_Connection *conn)
{
    if (!h->allow_list || h->allow_count == 0)
        return true;  /* no allow list = allow all */

    const union MHD_ConnectionInfo *ci =
        MHD_get_connection_info(conn, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    if (!ci || !ci->client_addr)
        return false;

    const struct sockaddr *sa = ci->client_addr;
    if (sa->sa_family != AF_INET)
        return false;  /* IPv6 not in allow list = deny */

    uint32_t client_ip = ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr);

    for (int i = 0; i < h->allow_count; i++) {
        if ((client_ip & h->allow_list[i].mask) == h->allow_list[i].network)
            return true;
    }
    return false;
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

static bool check_origin(mnemon_http_t *h, struct MHD_Connection *conn)
{
    const char *origin = MHD_lookup_connection_value(conn, MHD_HEADER_KIND,
                                                      "Origin");
    /* No Origin header is fine (curl, non-browser clients) */
    if (!origin) return true;

    /* Localhost origins are always allowed */
    if (strstr(origin, "localhost") || strstr(origin, "127.0.0.1") ||
        strstr(origin, "[::1]"))
        return true;

    /* If auth or IP allow list is configured, allow any origin since
     * access control is already enforced. Without either, restrict to
     * localhost to prevent DNS rebinding attacks. */
    if (h->auth_token || h->allow_count > 0)
        return true;

    mnemon_log(MNEMON_LOG_WARNING, "HTTP: blocked request from origin: %s "
               "(set auth_token or allow_ips to allow remote origins)", origin);
    return false;
}

/* ---- HTTP error responses ---- */

/* Map HTTP status to JSON-RPC error code */
static int http_to_jsonrpc_code(unsigned int status)
{
    switch (status) {
    case MHD_HTTP_BAD_REQUEST:        return -32600; /* Invalid Request */
    case MHD_HTTP_NOT_FOUND:          return -32601; /* Method not found */
    case MHD_HTTP_METHOD_NOT_ALLOWED: return -32601;
    case MHD_HTTP_CONTENT_TOO_LARGE:  return -32600;
    default:                          return -32000; /* Server error */
    }
}

static enum MHD_Result respond_error(struct MHD_Connection *conn,
                                      unsigned int status, const char *msg)
{
    char body[512];
    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":null,"
             "\"error\":{\"code\":%d,\"message\":\"%s\"}}",
             http_to_jsonrpc_code(status), msg);
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
                                    request_state_t *rs)
{
    if (!rs->data || rs->len == 0)
        return respond_error(conn, MHD_HTTP_BAD_REQUEST, "empty body");

    /* Parse JSON-RPC */
    cJSON *request = cJSON_Parse(rs->data);
    if (!request)
        return respond_error(conn, MHD_HTTP_BAD_REQUEST, "invalid JSON");

    /* Check if this is an initialize request (creates session) */
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(request, "method");
    bool is_init = (cJSON_IsString(method) &&
                    strcmp(method->valuestring, "initialize") == 0);

    /* Capture tool name for completion logging when this is tools/call */
    if (cJSON_IsString(method) && strcmp(method->valuestring, "tools/call") == 0) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(request, "params");
        const cJSON *name_item = params
            ? cJSON_GetObjectItemCaseSensitive(params, "name") : NULL;
        if (cJSON_IsString(name_item))
            snprintf(rs->tool_name, sizeof(rs->tool_name), "%s",
                     name_item->valuestring);
    } else if (cJSON_IsString(method)) {
        snprintf(rs->tool_name, sizeof(rs->tool_name), "%s",
                 method->valuestring);
    }

    /* Capture the JSON-RPC id (string or number) so an arrival line can be
     * correlated with its completion line -- the id survives even on frames
     * where the tool name does not. */
    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(request, "id");
    if (cJSON_IsString(id_item) && id_item->valuestring)
        snprintf(rs->rpc_id, sizeof(rs->rpc_id), "%s", id_item->valuestring);
    else if (cJSON_IsNumber(id_item))
        snprintf(rs->rpc_id, sizeof(rs->rpc_id), "%lld",
                 (long long)id_item->valuedouble);
    /* else: notification (no id) -- leave rpc_id empty */

    /* Arrival log: confirms the request reached mnemond and when, before any
     * dispatch work. Pairs with the completion line via method/tool/id. */
    mnemon_log(MNEMON_LOG_INFO,
               "HTTP: request recv method=%s tool=%s id=%s ip=%s body=%zu",
               rs->method,
               rs->tool_name[0] ? rs->tool_name : "-",
               rs->rpc_id[0] ? rs->rpc_id : "-",
               get_client_ip(conn), rs->len);

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
        mnemon_log(MNEMON_LOG_INFO, "HTTP: new session %s from %s",
                   session->id, get_client_ip(conn));
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
        mnemon_log(MNEMON_LOG_INFO, "HTTP: session terminated: %s from %s",
                   session_id, get_client_ip(conn));
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

/* Per-stream context passed to the SSE reader/free callbacks. Lets us tie
 * the queue back to its session on TCP close so we can fully reap it. */
typedef struct {
    mnemon_http_t *h;
    sse_queue_t   *q;
    char           session_id[64];
} sse_stream_ctx_t;

static ssize_t sse_content_reader(void *cls, uint64_t pos, char *buf,
                                  size_t max)
{
    (void)pos;
    sse_stream_ctx_t *ctx = (sse_stream_ctx_t *)cls;
    sse_event_t event;

    mnemon_err_t err = sse_queue_pop(ctx->q, &event, 200);
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

/* Called by MHD when the SSE response is destroyed (TCP closed by either
 * side, or daemon stopping). MHD guarantees the reader callback has fully
 * returned before this fires, so it is safe to destroy the queue. We take
 * ownership only if the session still references THIS queue -- if the
 * session was already removed (DELETE, idle reap, LRU eviction), that path
 * already destroyed the queue and ctx->q is dangling; do not touch it. */
static void sse_content_free(void *cls)
{
    sse_stream_ctx_t *ctx = (sse_stream_ctx_t *)cls;
    if (!ctx) return;

    pthread_mutex_lock(&ctx->h->session_mutex);
    http_session_t *session = find_session(ctx->h, ctx->session_id);
    if (session && session->sse_queue == ctx->q) {
        sse_queue_close(ctx->q);
        sse_queue_destroy(ctx->q);
        free(ctx->q);
        session->sse_queue = NULL;
        mnemon_log(MNEMON_LOG_INFO,
                   "HTTP: SSE stream closed for session %s -- queue reaped",
                   ctx->session_id);
    }
    pthread_mutex_unlock(&ctx->h->session_mutex);
    free(ctx);
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
    sse_stream_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        pthread_mutex_unlock(&h->session_mutex);
        return respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "OOM");
    }
    ctx->h = h;
    ctx->q = q;
    snprintf(ctx->session_id, sizeof(ctx->session_id), "%s", session->id);
    pthread_mutex_unlock(&h->session_mutex);

    /* Create streaming response */
    struct MHD_Response *resp = MHD_create_response_from_callback(
        MHD_SIZE_UNKNOWN, 4096, sse_content_reader, ctx, sse_content_free);
    if (!resp) {
        /* sse_content_free will not be called -- clean up ctx ourselves.
         * Leave session->sse_queue intact; it'll be reused on next GET or
         * destroyed when the session is reaped/removed. */
        free(ctx);
        return respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             "failed to create SSE response");
    }

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

/* ---- Connection-level notify callback (TCP open/close) ---- */

/* Enable TCP keepalive on an accepted socket so the kernel detects dead peers
 * (client crashed, NAT mapping rotated, route flap) within ~2 minutes instead
 * of the default ~2 hours. Also acts as belt-and-suspenders alongside the
 * MHD idle timeout: keepalive catches half-open TCP, the MHD timeout catches
 * idle-but-live HTTP keep-alive sockets. */
static void enable_tcp_keepalive(struct MHD_Connection *conn)
{
    const union MHD_ConnectionInfo *ci =
        MHD_get_connection_info(conn, MHD_CONNECTION_INFO_CONNECTION_FD);
    if (!ci) return;
    int fd = ci->connect_fd;
    if (fd < 0) return;

    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) < 0)
        return;  /* not fatal -- MHD timeout still applies */

#ifdef TCP_KEEPIDLE
    int idle = 60;     /* start probing after 60s of idle */
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
    int intvl = 15;    /* probe every 15s */
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#ifdef TCP_KEEPCNT
    int cnt = 4;       /* drop after 4 missed probes (60 + 4*15 = ~120s) */
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
}

static void notify_connection_cb(void *cls, struct MHD_Connection *conn,
                                 void **socket_context,
                                 enum MHD_ConnectionNotificationCode toe)
{
    mnemon_http_t *h = (mnemon_http_t *)cls;
    (void)socket_context;
    if (!h) return;

    if (toe == MHD_CONNECTION_NOTIFY_STARTED) {
        enable_tcp_keepalive(conn);
        int now = atomic_fetch_add(&h->open_connections, 1) + 1;
        int max = h->max_connections;
        if (max > 0) {
            int pct = (int)((int64_t)now * 100 / max);
            int64_t now_ms = mnemon_time_ms();
            int64_t last = atomic_load(&h->last_conn_warn_ms);
            /* Warn when crossing 75/90/100% thresholds, throttled */
            int threshold = 0;
            if (pct >= 100)      threshold = 100;
            else if (pct >= 90)  threshold = 90;
            else if (pct >= 75)  threshold = 75;
            if (threshold && (threshold > h->last_warn_pct ||
                              now_ms - last > CONN_WARN_COOLDOWN_MS)) {
                mnemon_log(MNEMON_LOG_WARNING,
                    "HTTP: connections at %d/%d (%d%%) -- nearing max_connections",
                    now, max, pct);
                atomic_store(&h->last_conn_warn_ms, now_ms);
                h->last_warn_pct = threshold;
            }
        }
    } else if (toe == MHD_CONNECTION_NOTIFY_CLOSED) {
        int now = atomic_fetch_sub(&h->open_connections, 1) - 1;
        if (now < 0) {
            /* Should not happen; clamp and log once */
            atomic_store(&h->open_connections, 0);
        }
        if (h->max_connections > 0 && now * 4 < h->max_connections * 3)
            h->last_warn_pct = 0;  /* below 75% again -- re-arm warnings */
    }
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

    request_state_t *rs = (request_state_t *)*con_cls;

    /* First call: validate, then allocate request state. We must validate
     * BEFORE returning MHD_YES on POST -- queuing a response after MHD has
     * begun delivering upload_data is undefined. */
    if (!rs) {
        if (strcmp(method, "OPTIONS") == 0)
            return handle_options(conn);

        if (strcmp(url, h->mcp_path) != 0)
            return respond_error(conn, MHD_HTTP_NOT_FOUND, "not found");

        if (!check_ip_allowed(h, conn)) {
            mnemon_log(MNEMON_LOG_WARNING,
                       "HTTP: denied connection from %s (not in allow_ips)",
                       get_client_ip(conn));
            return respond_error(conn, MHD_HTTP_FORBIDDEN, "ip not allowed");
        }

        if (!check_origin(h, conn))
            return respond_error(conn, MHD_HTTP_FORBIDDEN, "origin blocked");

        if (!check_auth(h, conn)) {
            mnemon_log(MNEMON_LOG_WARNING,
                       "HTTP: auth failed from %s", get_client_ip(conn));
            return respond_error(conn, MHD_HTTP_UNAUTHORIZED, "unauthorized");
        }

        rs = calloc(1, sizeof(*rs));
        if (rs == NULL) return MHD_NO;
        rs->started_ms = mnemon_time_ms();
        snprintf(rs->method, sizeof(rs->method), "%s", method);
        snprintf(rs->ip, sizeof(rs->ip), "%s", get_client_ip(conn));
        atomic_fetch_add(&h->in_flight_requests, 1);
        atomic_fetch_add(&h->total_requests, 1);
        rs->in_flight_counted = true;
        *con_cls = rs;

        if (strcmp(method, "GET") == 0)
            return handle_get(h, conn);
        if (strcmp(method, "DELETE") == 0)
            return handle_delete(h, conn);
        if (strcmp(method, "POST") != 0)
            return respond_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED,
                                 "method not allowed");

        /* POST: defer until upload data is delivered on the next call */
        return MHD_YES;
    }

    /* Subsequent calls: only POST gets here, accumulating upload data */
    if (*upload_data_size > 0) {
        if (rs->len + *upload_data_size > MAX_POST_SIZE) {
            return respond_error(conn, MHD_HTTP_CONTENT_TOO_LARGE,
                                 "request body too large");
        }
        if (rs->len + *upload_data_size >= rs->cap) {
            size_t newcap = rs->cap ? rs->cap * 2 : 4096;
            while (newcap < rs->len + *upload_data_size + 1) newcap *= 2;
            char *p = realloc(rs->data, newcap);
            if (p == NULL) return MHD_NO;
            rs->data = p;
            rs->cap = newcap;
        }
        memcpy(rs->data + rs->len, upload_data, *upload_data_size);
        rs->len += *upload_data_size;
        rs->data[rs->len] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }

    return handle_post(h, conn, rs);
}

/* ---- Cleanup callback (called when connection closes) ---- */

static void request_completed(void *cls, struct MHD_Connection *conn,
                               void **con_cls,
                               enum MHD_RequestTerminationCode toe)
{
    mnemon_http_t *h = (mnemon_http_t *)cls;
    (void)conn;

    request_state_t *rs = (request_state_t *)*con_cls;
    if (!rs) return;

    int after = -1;
    if (rs->in_flight_counted && h)
        after = atomic_fetch_sub(&h->in_flight_requests, 1) - 1;

    int64_t duration = mnemon_time_ms() - rs->started_ms;
    const char *tool = rs->tool_name[0] ? rs->tool_name : "-";
    const char *id = rs->rpc_id[0] ? rs->rpc_id : "-";

    if (h && duration >= SLOW_REQUEST_MS) {
        atomic_fetch_add(&h->slow_requests, 1);
        mnemon_log(MNEMON_LOG_WARNING,
            "HTTP: slow request method=%s tool=%s id=%s ip=%s duration=%" PRId64
            "ms in_flight=%d toe=%d",
            rs->method, tool, id, rs->ip, duration, after, (int)toe);
    } else if (toe != MHD_REQUEST_TERMINATED_COMPLETED_OK) {
        mnemon_log(MNEMON_LOG_WARNING,
            "HTTP: request aborted method=%s tool=%s id=%s ip=%s duration=%" PRId64
            "ms in_flight=%d toe=%d",
            rs->method, tool, id, rs->ip, duration, after, (int)toe);
    } else {
        mnemon_log(MNEMON_LOG_DEBUG,
            "HTTP: request method=%s tool=%s id=%s ip=%s duration=%" PRId64
            "ms in_flight=%d",
            rs->method, tool, id, rs->ip, duration, after);
    }

    free(rs->data);
    free(rs);
    *con_cls = NULL;
}

/* ---- Public API ---- */

mnemon_err_t mnemon_http_start(mnemon_http_t **out,
                               const mnemon_http_config_t *cfg,
                               mnemon_dispatch_t *dispatch)
{
    if (!out || !cfg || !dispatch)
        return MNEMON_ERR_INVALID_INPUT;

    /* Security advisory: warn if binding to non-localhost without auth or IP filtering */
    if (cfg->bind_address &&
        strcmp(cfg->bind_address, "127.0.0.1") != 0 &&
        strcmp(cfg->bind_address, "localhost") != 0 &&
        strcmp(cfg->bind_address, "::1") != 0 &&
        (!cfg->auth_token || cfg->auth_token[0] == '\0') &&
        (!cfg->allow_ips || cfg->allow_ips[0] == '\0')) {
        mnemon_log(MNEMON_LOG_WARNING,
                   "HTTP: binding to %s with no auth_token and no allow_ips -- "
                   "server is open to all clients on the network",
                   cfg->bind_address);
    }

    mnemon_http_t *h = calloc(1, sizeof(*h));
    if (!h) return MNEMON_ERR_OOM;

    h->dispatch = dispatch;
    h->auth_token = cfg->auth_token && cfg->auth_token[0]
                    ? strdup(cfg->auth_token) : NULL;
    h->mcp_path = strdup(cfg->mcp_path ? cfg->mcp_path : "/mcp");
    h->allow_count = parse_allow_ips(cfg->allow_ips, &h->allow_list);
    h->max_connections = cfg->max_connections;
    h->session_idle_timeout_ms = cfg->session_idle_timeout > 0
        ? cfg->session_idle_timeout * 1000 : 0;
    atomic_init(&h->open_connections, 0);
    atomic_init(&h->in_flight_requests, 0);
    atomic_init(&h->total_requests, 0);
    atomic_init(&h->slow_requests, 0);
    atomic_init(&h->rejected_connections, 0);
    atomic_init(&h->last_conn_warn_ms, 0);
    h->last_warn_pct = 0;
    pthread_mutex_init(&h->session_mutex, NULL);

    int port = cfg->port > 0 ? cfg->port : 3847;

    unsigned int flags = MHD_USE_INTERNAL_POLLING_THREAD |
                         MHD_USE_AUTO | MHD_ALLOW_SUSPEND_RESUME;

    /* Resolve bind address to sockaddr for MHD_OPTION_SOCK_ADDR */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons((uint16_t)port);
    const char *bind_str = cfg->bind_address ? cfg->bind_address : "127.0.0.1";
    if (strcmp(bind_str, "localhost") == 0)
        bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else if (inet_pton(AF_INET, bind_str, &bind_addr.sin_addr) != 1) {
        mnemon_err_set(MNEMON_ERR_INVALID_INPUT, 0,
                       "HTTP: invalid bind address: %s", bind_str);
        free(h->auth_token);
        free(h->mcp_path);
        pthread_mutex_destroy(&h->session_mutex);
        free(h);
        return MNEMON_ERR_INVALID_INPUT;
    }

    unsigned int conn_timeout = cfg->connection_timeout > 0
        ? (unsigned int)cfg->connection_timeout : 0;
    unsigned int per_ip_limit = cfg->per_ip_connection_limit > 0
        ? (unsigned int)cfg->per_ip_connection_limit : 0;

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
            MHD_OPTION_NOTIFY_COMPLETED, request_completed, h,
            MHD_OPTION_NOTIFY_CONNECTION, notify_connection_cb, h,
            MHD_OPTION_CONNECTION_LIMIT, (unsigned int)cfg->max_connections,
            MHD_OPTION_PER_IP_CONNECTION_LIMIT, per_ip_limit,
            MHD_OPTION_CONNECTION_TIMEOUT, conn_timeout,
            MHD_OPTION_SOCK_ADDR, (struct sockaddr *)&bind_addr,
            MHD_OPTION_HTTPS_MEM_KEY, h->tls_key_mem,
            MHD_OPTION_HTTPS_MEM_CERT, h->tls_cert_mem,
            MHD_OPTION_END);
        if (h->daemon == NULL) {
            mnemon_err_set(MNEMON_ERR_INTERNAL, 0,
                           "MHD_start_daemon (TLS) failed on port %d", port);
        }
    } else {
        h->daemon = MHD_start_daemon(
            flags, (uint16_t)port, NULL, NULL,
            request_handler, h,
            MHD_OPTION_NOTIFY_COMPLETED, request_completed, h,
            MHD_OPTION_NOTIFY_CONNECTION, notify_connection_cb, h,
            MHD_OPTION_CONNECTION_LIMIT, (unsigned int)cfg->max_connections,
            MHD_OPTION_PER_IP_CONNECTION_LIMIT, per_ip_limit,
            MHD_OPTION_CONNECTION_TIMEOUT, conn_timeout,
            MHD_OPTION_SOCK_ADDR, (struct sockaddr *)&bind_addr,
            MHD_OPTION_END);
        if (h->daemon == NULL) {
            mnemon_err_set(MNEMON_ERR_INTERNAL, 0,
                           "MHD_start_daemon failed on port %d", port);
        }
    }

    if (h->daemon == NULL) {
        free(h->auth_token);
        free(h->mcp_path);
        free(h->tls_cert_mem);
        free(h->tls_key_mem);
        pthread_mutex_destroy(&h->session_mutex);
        free(h);
        return MNEMON_ERR_INTERNAL;
    }

    mnemon_log(MNEMON_LOG_INFO,
               "HTTP transport started: %s:%d%s (auth=%s, tls=%s, allow_ips=%s, "
               "max_conn=%d, idle_timeout=%us, per_ip_limit=%u, "
               "session_idle=%ds)",
               cfg->bind_address ? cfg->bind_address : "0.0.0.0",
               port, h->mcp_path,
               h->auth_token ? "yes" : "no",
               (cfg->tls_cert_path && cfg->tls_key_path) ? "yes" : "no",
               h->allow_count > 0 ? cfg->allow_ips : "all",
               cfg->max_connections, conn_timeout, per_ip_limit,
               cfg->session_idle_timeout);

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
    free(h->allow_list);
    free(h->tls_cert_mem);
    free(h->tls_key_mem);
    pthread_mutex_destroy(&h->session_mutex);
    free(h);
}

int mnemon_http_session_count(const mnemon_http_t *h)
{
    return h ? h->session_count : 0;
}

void mnemon_http_get_stats(mnemon_http_t *h, mnemon_http_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!h) return;
    out->sessions             = h->session_count;
    out->open_connections     = atomic_load(&h->open_connections);
    out->max_connections      = h->max_connections;
    out->in_flight_requests   = atomic_load(&h->in_flight_requests);
    out->total_requests       = atomic_load(&h->total_requests);
    out->slow_requests        = atomic_load(&h->slow_requests);
    out->rejected_connections = atomic_load(&h->rejected_connections);
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
void mnemon_http_get_stats(mnemon_http_t *h, mnemon_http_stats_t *out)
{ (void)h; if (out) memset(out, 0, sizeof(*out)); }
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
