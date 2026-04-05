/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * honeypot.h -- Abuse detection: canaries, behavioral analysis, decoy tools
 */

#ifndef MNEMON_HONEYPOT_H
#define MNEMON_HONEYPOT_H

#include "mnemon.h"
#include "audit.h"

typedef struct mnemon_honeypot mnemon_honeypot_t;

/* Initialize the honeypot module. Seeds canary records on first run. */
mnemon_err_t mnemon_honeypot_init(mnemon_honeypot_t **out,
                                  mnemon_audit_t *audit);
void         mnemon_honeypot_free(mnemon_honeypot_t *hp);

/* Check content for prompt injection patterns.
 * Returns a suspicion score (0.0 = clean, higher = more suspicious).
 * Score >= 3.0 triggers ALERT_MEDIUM, >= 7.0 triggers ALERT_HIGH. */
float mnemon_honeypot_scan_injection(mnemon_honeypot_t *hp,
                                     const char *content, size_t len);

/* Track auth attempts. Returns true if the IP should be rate-limited. */
bool mnemon_honeypot_auth_attempt(mnemon_honeypot_t *hp,
                                  const char *ip, bool success);

/* Track search frequency per session. Returns true if suspicious. */
bool mnemon_honeypot_track_search(mnemon_honeypot_t *hp,
                                  const char *session_id);

/* Track enumeration patterns. Returns true if suspicious. */
bool mnemon_honeypot_track_enum(mnemon_honeypot_t *hp,
                                const char *session_id, int offset);

/* Check if a query looks like credential harvesting */
bool mnemon_honeypot_suspicious_query(mnemon_honeypot_t *hp,
                                      const char *query);

/* Check a tool result for canary UUIDs. Returns true if canary accessed. */
bool mnemon_honeypot_check_canary(mnemon_honeypot_t *hp,
                                  const char *result_json,
                                  const char *session_id,
                                  const char *tool_name);

/* Add a canary UUID to the tracking set */
void mnemon_honeypot_add_canary(mnemon_honeypot_t *hp,
                                const char *uuid_str);

#endif /* MNEMON_HONEYPOT_H */
