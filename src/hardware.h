/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hardware.h -- Hardware detection interface (CPU, GPU, NPU, SIMD, NUMA)
 */

#ifndef MNEMON_HARDWARE_H
#define MNEMON_HARDWARE_H

#include "mnemon.h"

typedef enum {
    MNEMON_GPU_NONE = 0,
    MNEMON_GPU_NVIDIA,
    MNEMON_GPU_AMD,
    MNEMON_GPU_INTEL,
} mnemon_gpu_vendor_t;

typedef struct {
    /* CPU */
    char     cpu_model[128];
    int      cpu_cores;
    bool     has_avx2;
    bool     has_avx512f;
    bool     has_avx512bw;
    bool     has_avx512vnni;
    bool     has_amx;

    /* GPU */
    mnemon_gpu_vendor_t gpu_vendor;
    char     gpu_model[128];
    uint64_t gpu_vram_bytes;     /* Dedicated VRAM */
    uint64_t gpu_gtt_bytes;      /* GPU-accessible system RAM (AMD APU) */
    int      gpu_compute_capability; /* NVIDIA SM, AMD GFX version */
    bool     has_rocm;           /* AMD ROCm runtime available */

    /* NPU */
    bool     has_npu;            /* AI accelerator present */
    char     npu_model[64];      /* e.g., "AMD XDNA" */

    /* Memory */
    uint64_t ram_total_bytes;
    uint64_t ram_available_bytes;
    int      numa_nodes;

    /* Storage */
    bool     has_nvme;
} mnemon_hardware_t;

mnemon_err_t mnemon_hardware_detect(mnemon_hardware_t *out);

typedef struct {
    float (*cosine_distance)(const float *a, const float *b, size_t n);
    float (*dot_product)(const float *a, const float *b, size_t n);
    float (*l2_distance)(const float *a, const float *b, size_t n);
    const char *name;
} mnemon_simd_ops_t;

void mnemon_simd_init(const mnemon_hardware_t *hw);
extern mnemon_simd_ops_t g_simd_ops;

#endif /* MNEMON_HARDWARE_H */
