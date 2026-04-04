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

#endif /* MNEMON_TEMPORAL_H */
