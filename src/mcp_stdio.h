/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mcp_stdio.h -- MCP stdio transport interface
 */

#ifndef MNEMON_MCP_STDIO_H
#define MNEMON_MCP_STDIO_H

#include "mnemon.h"
#include <cJSON.h>

typedef struct {
    int (*read_request)(void *ctx, cJSON **out);
    int (*write_response)(void *ctx, const cJSON *response);
    void (*close)(void *ctx);
} mnemon_transport_t;

mnemon_err_t mnemon_mcp_stdio_init(mnemon_transport_t *transport);
mnemon_err_t mnemon_mcp_stdio_run(mnemon_transport_t *transport,
                                   void *dispatch_ctx);

#endif /* MNEMON_MCP_STDIO_H */
