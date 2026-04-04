/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * memory.c -- Memory lifecycle: Hebbian importance and exponential decay
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif


#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "memory.h"

#define LEARNING_RATE 0.1f

float mnemon_importance_update(float current, uint32_t access_count)
{
    (void)access_count;
    float updated = current + LEARNING_RATE * (1.0f - current);
    if (updated > 1.0f) updated = 1.0f;
    if (updated < 0.0f) updated = 0.0f;
    return updated;
}

float mnemon_importance_decay(float importance, int64_t last_accessed_ms,
                              int64_t now_ms, int half_life_days)
{
    if (half_life_days <= 0 || last_accessed_ms <= 0 || now_ms <= last_accessed_ms)
        return importance;
    double elapsed_days = (double)(now_ms - last_accessed_ms) / 86400000.0;
    double factor = pow(0.5, elapsed_days / (double)half_life_days);
    return (float)(importance * factor);
}

bool mnemon_should_prune(float importance, int64_t last_accessed_ms,
                         int64_t now_ms, int half_life_days,
                         float min_importance)
{
    float decayed = mnemon_importance_decay(importance, last_accessed_ms,
                                            now_ms, half_life_days);
    return decayed < min_importance;
}

int64_t mnemon_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

int64_t mnemon_parse_iso8601(const char *str)
{
    int y, mo, d, h, mi, s;
    struct tm tm;

    if (!str) return 0;
    if (sscanf(str, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 6)
        return 0;

    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;

    time_t t = timegm(&tm);
    if (t == (time_t)-1) return 0;
    return (int64_t)t * 1000;
}

void mnemon_format_iso8601(int64_t ms, char *buf, size_t len)
{
    time_t sec = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&sec, &tm);
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             (int)(ms % 1000));
}
