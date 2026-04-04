/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * distance_avx512.c -- AVX-512 distance functions
 *
 * Compiled with: -mavx512f -mavx512bw
 * Processes 16 floats per iteration (512-bit registers).
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "distance.h"

#ifdef HAVE_AVX512

#include <immintrin.h>
#include <math.h>

float mnemon_cosine_avx512(const float *a, const float *b, size_t n)
{
    __m512 sum_ab = _mm512_setzero_ps();
    __m512 sum_aa = _mm512_setzero_ps();
    __m512 sum_bb = _mm512_setzero_ps();
    size_t i = 0;

    for (; i + 16 <= n; i += 16) {
        __m512 va = _mm512_loadu_ps(a + i);
        __m512 vb = _mm512_loadu_ps(b + i);
        sum_ab = _mm512_fmadd_ps(va, vb, sum_ab);
        sum_aa = _mm512_fmadd_ps(va, va, sum_aa);
        sum_bb = _mm512_fmadd_ps(vb, vb, sum_bb);
    }

    float dot = _mm512_reduce_add_ps(sum_ab);
    float na  = _mm512_reduce_add_ps(sum_aa);
    float nb  = _mm512_reduce_add_ps(sum_bb);

    /* Scalar tail */
    for (; i < n; i++) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }

    float denom = sqrtf(na) * sqrtf(nb);
    if (denom < 1e-8f)
        return 1.0f;

    return 1.0f - (dot / denom);
}

float mnemon_dot_avx512(const float *a, const float *b, size_t n)
{
    __m512 sum = _mm512_setzero_ps();
    size_t i = 0;

    for (; i + 16 <= n; i += 16) {
        __m512 va = _mm512_loadu_ps(a + i);
        __m512 vb = _mm512_loadu_ps(b + i);
        sum = _mm512_fmadd_ps(va, vb, sum);
    }

    float result = _mm512_reduce_add_ps(sum);

    for (; i < n; i++)
        result += a[i] * b[i];

    return result;
}

float mnemon_l2_avx512(const float *a, const float *b, size_t n)
{
    __m512 sum = _mm512_setzero_ps();
    size_t i = 0;

    for (; i + 16 <= n; i += 16) {
        __m512 va = _mm512_loadu_ps(a + i);
        __m512 vb = _mm512_loadu_ps(b + i);
        __m512 diff = _mm512_sub_ps(va, vb);
        sum = _mm512_fmadd_ps(diff, diff, sum);
    }

    float result = _mm512_reduce_add_ps(sum);

    for (; i < n; i++) {
        float d = a[i] - b[i];
        result += d * d;
    }

    return sqrtf(result);
}

#else /* !HAVE_AVX512 */

float mnemon_cosine_avx512(const float *a, const float *b, size_t n)
{
    return mnemon_cosine_scalar(a, b, n);
}

float mnemon_dot_avx512(const float *a, const float *b, size_t n)
{
    return mnemon_dot_scalar(a, b, n);
}

float mnemon_l2_avx512(const float *a, const float *b, size_t n)
{
    return mnemon_l2_scalar(a, b, n);
}

#endif /* HAVE_AVX512 */
