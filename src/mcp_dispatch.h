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

/* Notification callback for server-initiated events (e.g. SSE push) */
typedef void (*mnemon_notify_fn)(void *ctx, const char *event_type,
                                 const char *json_data);

mnemon_err_t mnemon_dispatch_init(mnemon_dispatch_t **out,
                                  mnemon_storage_t *storage);
void         mnemon_dispatch_free(mnemon_dispatch_t *d);
cJSON       *mnemon_dispatch_request(mnemon_dispatch_t *d, const cJSON *request);

/* Register a notification callback (e.g. to push SSE events).
 * ctx is passed as first argument to fn. */
void         mnemon_dispatch_set_notifier(mnemon_dispatch_t *d,
                                          mnemon_notify_fn fn, void *ctx);

#endif /* MNEMON_MCP_DISPATCH_H */
