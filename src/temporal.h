/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * temporal.h -- Bi-temporal query interface
 */

#ifndef MNEMON_TEMPORAL_H
#define MNEMON_TEMPORAL_H

#include "mnemon.h"
#include "storage.h"

/* Time-filtered search: returns memories/entities within a time range */
mnemon_err_t mnemon_search_temporal(mnemon_storage_t *s,
                                    const uint8_t *entity_id,
                                    int64_t since, int64_t until,
                                    int top_k,
                                    mnemon_result_set_t *out);

/* Get entity state at a specific point in time (bi-temporal query) */
mnemon_err_t mnemon_get_state_at_time(mnemon_storage_t *s,
                                      const uint8_t entity_id[16],
                                      int64_t timestamp,
                                      mnemon_entity_t *out);

/* Get version history for an entity */
mnemon_err_t mnemon_get_history(mnemon_storage_t *s,
                                const uint8_t entity_id[16],
                                int64_t since, int64_t until,
                                mnemon_version_list_t *out);

/* Get change feed since a given time */
mnemon_err_t mnemon_get_changes_since(mnemon_storage_t *s,
                                      int64_t since,
                                      const char *entity_type,
                                      int top_k,
                                      mnemon_result_set_t *out);

/* ---- Event date extraction and temporal event queries ---- */

/* Extracted event: a description + date found in text */
typedef struct {
    char   *description;    /* e.g., "workshop on Effective Communication" */
    int64_t event_date;     /* ms since epoch */
} mnemon_event_t;

typedef struct {
    mnemon_event_t *events;
    int              count;
} mnemon_event_list_t;

void mnemon_event_list_free(mnemon_event_list_t *list);

/* Parse natural language dates from text.
 * context_year: default year for dates without explicit year (e.g., 2023).
 * Returns extracted events with their dates. */
mnemon_err_t mnemon_extract_events(const char *text, int context_year,
                                   mnemon_event_list_t *out);

/* Parse a single natural language date string.
 * Handles: "January 10th", "March 15, 2023", "Feb 27", "2023-01-10".
 * Returns ms since epoch, or 0 on failure. */
int64_t mnemon_parse_natural_date(const char *str, int context_year);

/* Search entities by event_date range */
mnemon_err_t mnemon_search_events(mnemon_storage_t *s,
                                  int64_t since, int64_t until,
                                  const char *name_filter,
                                  int top_k,
                                  mnemon_result_set_t *out);

/* Calculate duration in days between two timestamps */
int mnemon_duration_days(int64_t from_ms, int64_t to_ms);

#endif /* MNEMON_TEMPORAL_H */
