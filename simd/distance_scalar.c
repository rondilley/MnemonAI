/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * distance_scalar.c -- Scalar (portable) distance functions
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "distance.h"

#include <math.h>

float mnemon_cosine_scalar(const float *a, const float *b, size_t n)
{
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    size_t i;

    for (i = 0; i < n; i++) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }

    float denom = sqrtf(na) * sqrtf(nb);
    if (denom < 1e-8f)
        return 1.0f;

    return 1.0f - (dot / denom);
}

float mnemon_dot_scalar(const float *a, const float *b, size_t n)
{
    float dot = 0.0f;
    size_t i;

    for (i = 0; i < n; i++)
        dot += a[i] * b[i];

    return dot;
}

float mnemon_l2_scalar(const float *a, const float *b, size_t n)
{
    float sum = 0.0f;
    size_t i;

    for (i = 0; i < n; i++) {
        float d = a[i] - b[i];
        sum += d * d;
    }

    return sqrtf(sum);
}
