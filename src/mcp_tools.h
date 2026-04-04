/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mcp_tools.h -- MCP tool handler definitions
 */

#ifndef MNEMON_MCP_TOOLS_H
#define MNEMON_MCP_TOOLS_H

#include <cJSON.h>
#include "storage.h"

typedef struct {
    const char  *name;
    cJSON      *(*handler)(mnemon_storage_t *, const cJSON *);
    const char  *description;
    const char  *input_schema;
} mnemon_tool_def_t;

int mnemon_get_tool_defs(const mnemon_tool_def_t **out);

#endif /* MNEMON_MCP_TOOLS_H */
