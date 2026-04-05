/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * audit.h -- Append-only audit log with alert severity levels
 */

#ifndef MNEMON_AUDIT_H
#define MNEMON_AUDIT_H

#include "mnemon.h"

typedef struct mnemon_audit mnemon_audit_t;

typedef enum {
    ALERT_INFO = 0,     /* Informational */
    ALERT_MEDIUM,       /* Suspicious pattern, warrants review */
    ALERT_HIGH,         /* Likely malicious */
    ALERT_CRITICAL,     /* Active attack, consider session termination */
} mnemon_alert_level_t;

mnemon_err_t mnemon_audit_open(mnemon_audit_t **out, const char *path);
void         mnemon_audit_close(mnemon_audit_t *a);

/* Log a normal operation */
mnemon_err_t mnemon_audit_log(mnemon_audit_t *a, const char *tool_name,
                              const char *params_json,
                              const char *result_summary);

/* Log a security alert */
mnemon_err_t mnemon_audit_alert(mnemon_audit_t *a,
                                const char *alert_type,
                                mnemon_alert_level_t level,
                                const char *session_id,
                                const char *tool_name,
                                const char *detail);

const char *mnemon_alert_level_name(mnemon_alert_level_t level);

#endif /* MNEMON_AUDIT_H */
