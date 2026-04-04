/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * memory.h -- Memory lifecycle: importance, decay, time utilities
 */

#ifndef MNEMON_MEMORY_H
#define MNEMON_MEMORY_H

#include "mnemon.h"

float   mnemon_importance_update(float current, uint32_t access_count);
float   mnemon_importance_decay(float importance, int64_t last_accessed_ms,
                                int64_t now_ms, int half_life_days);
bool    mnemon_should_prune(float importance, int64_t last_accessed_ms,
                            int64_t now_ms, int half_life_days,
                            float min_importance);
int64_t mnemon_time_ms(void);
int64_t mnemon_parse_iso8601(const char *str);
void    mnemon_format_iso8601(int64_t ms, char *buf, size_t len);

#endif /* MNEMON_MEMORY_H */
