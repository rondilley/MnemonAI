/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * audit.h -- Append-only audit log of MCP operations
 */

#ifndef MNEMON_AUDIT_H
#define MNEMON_AUDIT_H

#include "mnemon.h"

typedef struct mnemon_audit mnemon_audit_t;

mnemon_err_t mnemon_audit_open(mnemon_audit_t **out, const char *path);
void         mnemon_audit_close(mnemon_audit_t *a);

/* Log an operation. tool_name and params_json are human-readable strings. */
mnemon_err_t mnemon_audit_log(mnemon_audit_t *a, const char *tool_name,
                              const char *params_json, const char *result_summary);

#endif /* MNEMON_AUDIT_H */
