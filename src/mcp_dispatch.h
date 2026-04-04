/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mcp_dispatch.h -- JSON-RPC 2.0 dispatch and tool registry
 */

#ifndef MNEMON_MCP_DISPATCH_H
#define MNEMON_MCP_DISPATCH_H

#include "mnemon.h"
#include "storage.h"
#include <cJSON.h>

typedef struct mnemon_dispatch mnemon_dispatch_t;
typedef cJSON *(*mnemon_tool_handler_t)(mnemon_storage_t *s, const cJSON *params);

mnemon_err_t mnemon_dispatch_init(mnemon_dispatch_t **out,
                                  mnemon_storage_t *storage);
void         mnemon_dispatch_free(mnemon_dispatch_t *d);
cJSON       *mnemon_dispatch_request(mnemon_dispatch_t *d, const cJSON *request);

#endif /* MNEMON_MCP_DISPATCH_H */
