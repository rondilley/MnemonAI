/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * distance_avx2.c -- AVX2+FMA distance functions
 *
 * Compiled with: -mavx2 -mfma
 * Processes 8 floats per iteration (256-bit registers).
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "distance.h"

#ifdef HAVE_AVX2

#include <immintrin.h>
#include <math.h>

float mnemon_cosine_avx2(const float *a, const float *b, size_t n)
{
    __m256 sum_ab = _mm256_setzero_ps();
    __m256 sum_aa = _mm256_setzero_ps();
    __m256 sum_bb = _mm256_setzero_ps();
    size_t i = 0;

    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        sum_ab = _mm256_fmadd_ps(va, vb, sum_ab);
        sum_aa = _mm256_fmadd_ps(va, va, sum_aa);
        sum_bb = _mm256_fmadd_ps(vb, vb, sum_bb);
    }

    /* Horizontal reduction */
    __m128 hi, lo;

    lo = _mm256_castps256_ps128(sum_ab);
    hi = _mm256_extractf128_ps(sum_ab, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_hadd_ps(lo, lo);
    lo = _mm_hadd_ps(lo, lo);
    float dot = _mm_cvtss_f32(lo);

    lo = _mm256_castps256_ps128(sum_aa);
    hi = _mm256_extractf128_ps(sum_aa, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_hadd_ps(lo, lo);
    lo = _mm_hadd_ps(lo, lo);
    float na = _mm_cvtss_f32(lo);

    lo = _mm256_castps256_ps128(sum_bb);
    hi = _mm256_extractf128_ps(sum_bb, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_hadd_ps(lo, lo);
    lo = _mm_hadd_ps(lo, lo);
    float nb = _mm_cvtss_f32(lo);

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

float mnemon_dot_avx2(const float *a, const float *b, size_t n)
{
    __m256 sum = _mm256_setzero_ps();
    size_t i = 0;

    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        sum = _mm256_fmadd_ps(va, vb, sum);
    }

    __m128 lo = _mm256_castps256_ps128(sum);
    __m128 hi = _mm256_extractf128_ps(sum, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_hadd_ps(lo, lo);
    lo = _mm_hadd_ps(lo, lo);
    float result = _mm_cvtss_f32(lo);

    for (; i < n; i++)
        result += a[i] * b[i];

    return result;
}

float mnemon_l2_avx2(const float *a, const float *b, size_t n)
{
    __m256 sum = _mm256_setzero_ps();
    size_t i = 0;

    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        __m256 diff = _mm256_sub_ps(va, vb);
        sum = _mm256_fmadd_ps(diff, diff, sum);
    }

    __m128 lo = _mm256_castps256_ps128(sum);
    __m128 hi = _mm256_extractf128_ps(sum, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_hadd_ps(lo, lo);
    lo = _mm_hadd_ps(lo, lo);
    float result = _mm_cvtss_f32(lo);

    for (; i < n; i++) {
        float d = a[i] - b[i];
        result += d * d;
    }

    return sqrtf(result);
}

#else /* !HAVE_AVX2 */

/* Provide symbols that won't be called but satisfy the linker */
float mnemon_cosine_avx2(const float *a, const float *b, size_t n)
{
    return mnemon_cosine_scalar(a, b, n);
}

float mnemon_dot_avx2(const float *a, const float *b, size_t n)
{
    return mnemon_dot_scalar(a, b, n);
}

float mnemon_l2_avx2(const float *a, const float *b, size_t n)
{
    return mnemon_l2_scalar(a, b, n);
}

#endif /* HAVE_AVX2 */
