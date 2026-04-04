/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * consolidate.h -- Memory consolidation interface
 *
 * Phase 2: Episodic-to-semantic consolidation pipeline.
 * Phase 1 provides the interface stubs.
 */

#ifndef MNEMON_CONSOLIDATE_H
#define MNEMON_CONSOLIDATE_H

#include "mnemon.h"
#include "storage.h"

typedef struct {
    int   consolidated_count;
    int   new_entities;
    int   new_relations;
    int64_t duration_ms;
} mnemon_consolidation_result_t;

/* Trigger consolidation. topic and entity_id are optional filters. */
mnemon_err_t mnemon_consolidate(mnemon_storage_t *s,
                                const char *topic,
                                const uint8_t *entity_id,
                                bool dry_run,
                                mnemon_consolidation_result_t *result);

#endif /* MNEMON_CONSOLIDATE_H */
