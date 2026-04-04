/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mcp_stdio.c -- MCP stdio transport
 *
 * Reads newline-delimited JSON from stdin, writes JSON to stdout.
 * stderr is used for logging (not part of the MCP protocol).
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

#include "mcp_stdio.h"
#include "mcp_dispatch.h"
#include "threads.h"
#include "log.h"

#define READ_BUF_SIZE (1024 * 1024)  /* 1MB line buffer */

typedef struct {
    char   *buf;
    size_t  len;
    size_t  cap;
} line_buf_t;

static int stdio_read_request(void *ctx, cJSON **out)
{
    line_buf_t *lb = (line_buf_t *)ctx;
    lb->len = 0;

    /* Read until newline or EOF */
    while (1) {
        if (lb->len >= lb->cap - 1) return -1; /* overflow */

        int c = fgetc(stdin);
        if (c == EOF) return -1;
        if (c == '\n') break;
        lb->buf[lb->len++] = (char)c;
    }
    lb->buf[lb->len] = '\0';

    if (lb->len == 0) return 0; /* empty line, skip */

    *out = cJSON_Parse(lb->buf);
    if (!*out) {
        mnemon_log(MNEMON_LOG_ERROR, "JSON parse error: %.80s", lb->buf);
        return -1;
    }

    return 0;
}

static int stdio_write_response(void *ctx, const cJSON *response)
{
    (void)ctx;
    char *json = cJSON_PrintUnformatted(response);
    if (!json) return -1;
    fprintf(stdout, "%s\n", json);
    fflush(stdout);
    cJSON_free(json);
    return 0;
}

static void stdio_close(void *ctx)
{
    line_buf_t *lb = (line_buf_t *)ctx;
    free(lb->buf);
    free(lb);
}

mnemon_err_t mnemon_mcp_stdio_init(mnemon_transport_t *transport)
{
    if (!transport) return MNEMON_ERR_INVALID_INPUT;

    /* Set stdout to line-buffered */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    line_buf_t *lb = calloc(1, sizeof(*lb));
    if (!lb) return MNEMON_ERR_OOM;
    lb->buf = malloc(READ_BUF_SIZE);
    if (!lb->buf) { free(lb); return MNEMON_ERR_OOM; }
    lb->cap = READ_BUF_SIZE;

    transport->read_request = stdio_read_request;
    transport->write_response = stdio_write_response;
    transport->close = stdio_close;

    return MNEMON_OK;
}

mnemon_err_t mnemon_mcp_stdio_run(mnemon_transport_t *transport,
                                   void *dispatch_ctx)
{
    mnemon_dispatch_t *dispatch = (mnemon_dispatch_t *)dispatch_ctx;
    line_buf_t *lb = calloc(1, sizeof(*lb));
    if (!lb) return MNEMON_ERR_OOM;
    lb->buf = malloc(READ_BUF_SIZE);
    if (!lb->buf) { free(lb); return MNEMON_ERR_OOM; }
    lb->cap = READ_BUF_SIZE;

    while (!mnemon_shutdown_requested()) {
        cJSON *request = NULL;
        int rc = stdio_read_request(lb, &request);

        if (feof(stdin)) {
            mnemon_log(MNEMON_LOG_INFO, "stdin EOF, shutting down");
            break;
        }

        /* Skip lines that fail to parse (malformed JSON, empty, overflow) */
        if (rc != 0 || !request)
            continue;

        /* Dispatch and get response */
        cJSON *response = mnemon_dispatch_request(dispatch, request);
        cJSON_Delete(request);

        if (response) {
            stdio_write_response(NULL, response);
            cJSON_Delete(response);
        }
    }

    free(lb->buf);
    free(lb);
    return MNEMON_OK;
}
