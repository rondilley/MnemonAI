/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * distance.h -- SIMD dispatch table for vector distance functions
 */

#ifndef MNEMON_DISTANCE_H
#define MNEMON_DISTANCE_H

#include <stddef.h>

typedef float (*mnemon_distance_fn)(const float *a, const float *b, size_t n);

typedef struct {
    mnemon_distance_fn cosine;
    mnemon_distance_fn dot;
    mnemon_distance_fn l2;
    const char *name;
} mnemon_distance_vtable_t;

const mnemon_distance_vtable_t *mnemon_distance_select(void);

/* Scalar (always available) */
float mnemon_cosine_scalar(const float *a, const float *b, size_t n);
float mnemon_dot_scalar(const float *a, const float *b, size_t n);
float mnemon_l2_scalar(const float *a, const float *b, size_t n);

/* AVX2 (compiled with -mavx2 -mfma) */
float mnemon_cosine_avx2(const float *a, const float *b, size_t n);
float mnemon_dot_avx2(const float *a, const float *b, size_t n);
float mnemon_l2_avx2(const float *a, const float *b, size_t n);

/* AVX-512 (compiled with -mavx512f -mavx512bw) */
float mnemon_cosine_avx512(const float *a, const float *b, size_t n);
float mnemon_dot_avx512(const float *a, const float *b, size_t n);
float mnemon_l2_avx512(const float *a, const float *b, size_t n);

#endif /* MNEMON_DISTANCE_H */
