/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mcp_dispatch.c -- JSON-RPC 2.0 dispatch and MCP protocol handling
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcp_dispatch.h"
#include "mcp_tools.h"
#include "log.h"

/* Upper bound on registered tools. Must be >= the number of entries in
 * tool_defs[] (mcp_tools.c); excess registrations are silently dropped from
 * tools/list and dispatch. Keep headroom so adding tools doesn't truncate. */
#define MAX_TOOLS 64

typedef struct {
    const char           *name;
    mnemon_tool_handler_t handler;
    const char           *description;
    const char           *input_schema;
} tool_entry_t;

struct mnemon_dispatch {
    mnemon_storage_t *storage;
    tool_entry_t      tools[MAX_TOOLS];
    int               tool_count;
    mnemon_notify_fn  notify_fn;
    void             *notify_ctx;
};

/* JSON-RPC error response */
static cJSON *make_error(const cJSON *id, int code, const char *message)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id && !cJSON_IsNull(id))
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    else
        cJSON_AddNullToObject(resp, "id");

    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(resp, "error", err);

    return resp;
}

/* JSON-RPC success response */
static cJSON *make_result(const cJSON *id, cJSON *result)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id && !cJSON_IsNull(id))
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    else
        cJSON_AddNullToObject(resp, "id");
    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

/* MCP content wrapper for tool results (per MCP spec Section "Tools") */
static cJSON *wrap_content(cJSON *tool_result)
{
    /* Check if the tool returned an error object */
    bool is_error = false;
    const cJSON *err_field = cJSON_GetObjectItemCaseSensitive(tool_result, "error");
    if (err_field && cJSON_IsString(err_field))
        is_error = true;

    char *json_str = cJSON_PrintUnformatted(tool_result);
    cJSON_Delete(tool_result);

    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "type", "text");
    cJSON_AddStringToObject(block, "text", json_str ? json_str : "{}");
    cJSON_AddItemToArray(content, block);
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddBoolToObject(result, "isError", is_error);

    cJSON_free(json_str);
    return result;
}

/* Handle MCP initialize */
static cJSON *handle_initialize(void)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");

    cJSON *caps = cJSON_CreateObject();
    cJSON *tools_cap = cJSON_CreateObject();
    cJSON_AddBoolToObject(tools_cap, "listChanged", false);
    cJSON_AddItemToObject(caps, "tools", tools_cap);
    cJSON_AddItemToObject(result, "capabilities", caps);

    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", PACKAGE_NAME);
    cJSON_AddStringToObject(info, "version", PACKAGE_VERSION);
    cJSON_AddItemToObject(result, "serverInfo", info);

    return result;
}

/* Handle tools/list */
static cJSON *handle_tools_list(mnemon_dispatch_t *d)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateArray();

    for (int i = 0; i < d->tool_count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", d->tools[i].name);
        cJSON_AddStringToObject(tool, "description", d->tools[i].description);
        if (d->tools[i].input_schema) {
            cJSON *schema = cJSON_Parse(d->tools[i].input_schema);
            if (schema)
                cJSON_AddItemToObject(tool, "inputSchema", schema);
        }
        cJSON_AddItemToArray(tools, tool);
    }

    cJSON_AddItemToObject(result, "tools", tools);
    return result;
}

/* Handle tools/call */
static cJSON *handle_tools_call(mnemon_dispatch_t *d, const cJSON *params)
{
    const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");

    if (!cJSON_IsString(name_item))
        return NULL;

    /* Guarantee handlers always receive a valid cJSON object, never NULL */
    cJSON *empty_args = NULL;
    if (!args || !cJSON_IsObject(args)) {
        empty_args = cJSON_CreateObject();
        args = empty_args;
    }

    const char *name = name_item->valuestring;

    for (int i = 0; i < d->tool_count; i++) {
        if (strcmp(d->tools[i].name, name) == 0) {
            mnemon_log(MNEMON_LOG_DEBUG, "calling tool: %s", name);
            cJSON *tool_result = d->tools[i].handler(d->storage, args);
            cJSON_Delete(empty_args);
            if (tool_result)
                return wrap_content(tool_result);
            return wrap_content(cJSON_CreateObject());
        }
    }

    cJSON_Delete(empty_args);

    return NULL;
}

mnemon_err_t mnemon_dispatch_init(mnemon_dispatch_t **out,
                                  mnemon_storage_t *storage)
{
    mnemon_dispatch_t *d;
    const mnemon_tool_def_t *defs;
    int count;

    if (!out || !storage) return MNEMON_ERR_INVALID_INPUT;

    d = calloc(1, sizeof(*d));
    if (!d) return MNEMON_ERR_OOM;
    d->storage = storage;

    /* Register tools from mcp_tools.c */
    count = mnemon_get_tool_defs(&defs);
    if (count > MAX_TOOLS)
        mnemon_log(MNEMON_LOG_WARNING,
                   "tool registry has %d tools but MAX_TOOLS is %d -- %d tool(s) "
                   "will be DROPPED; raise MAX_TOOLS in mcp_dispatch.c",
                   count, MAX_TOOLS, count - MAX_TOOLS);
    for (int i = 0; i < count && i < MAX_TOOLS; i++) {
        d->tools[i].name = defs[i].name;
        d->tools[i].handler = defs[i].handler;
        d->tools[i].description = defs[i].description;
        d->tools[i].input_schema = defs[i].input_schema;
    }
    d->tool_count = count < MAX_TOOLS ? count : MAX_TOOLS;

    mnemon_log(MNEMON_LOG_INFO, "MCP dispatch initialized with %d tools",
               d->tool_count);

    *out = d;
    return MNEMON_OK;
}

void mnemon_dispatch_free(mnemon_dispatch_t *d)
{
    free(d);
}

void mnemon_dispatch_set_notifier(mnemon_dispatch_t *d,
                                  mnemon_notify_fn fn, void *ctx)
{
    if (!d) return;
    d->notify_fn = fn;
    d->notify_ctx = ctx;
}

cJSON *mnemon_dispatch_request(mnemon_dispatch_t *d, const cJSON *request)
{
    const cJSON *method, *id, *params;

    if (!d || !request) return NULL;

    /* Validate JSON-RPC envelope */
    method = cJSON_GetObjectItemCaseSensitive(request, "method");
    id = cJSON_GetObjectItemCaseSensitive(request, "id");
    params = cJSON_GetObjectItemCaseSensitive(request, "params");

    if (!cJSON_IsString(method)) {
        return make_error(id, -32600, "Invalid request: missing method");
    }

    const char *m = method->valuestring;

    /* MCP protocol methods */
    if (strcmp(m, "initialize") == 0) {
        return make_result(id, handle_initialize());
    }

    if (strcmp(m, "notifications/initialized") == 0) {
        return NULL; /* No response for notifications */
    }

    if (strcmp(m, "tools/list") == 0) {
        return make_result(id, handle_tools_list(d));
    }

    if (strcmp(m, "tools/call") == 0) {
        if (!params) {
            return make_error(id, -32602, "Invalid params: missing params");
        }
        cJSON *result = handle_tools_call(d, params);
        if (!result) {
            const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(
                params, "name");
            char msg[128];
            snprintf(msg, sizeof(msg), "Tool not found: %s",
                     cJSON_IsString(name_item) ? name_item->valuestring : "?");
            return make_error(id, -32602, msg);
        }
        return make_result(id, result);
    }

    /* Unknown method */
    char msg[128];
    snprintf(msg, sizeof(msg), "Method not found: %s", m);
    return make_error(id, -32601, msg);
}
