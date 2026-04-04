/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hardware.c -- CPU, GPU, NPU, NUMA, and SIMD detection
 *
 * Detects hardware via:
 *   CPU:  /proc/cpuinfo, sysconf, __builtin_cpu_supports
 *   NVIDIA GPU: dlopen("libnvidia-ml.so.1") for NVML API
 *   AMD GPU:    /sys/class/drm/cardN/device/ (vendor, VRAM, GTT, model)
 *   AMD NPU:    /dev/accelN (XDNA AI accelerator)
 *   Intel GPU:  /sys/class/drm vendor=0x8086
 *   Memory:     /proc/meminfo
 *   NUMA:       /sys/devices/system/node/
 *   NVMe:       /sys/block/nvme*
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef HAVE_DLFCN_H
# include <dlfcn.h>
#endif

#include "hardware.h"
#include "distance.h"

mnemon_simd_ops_t g_simd_ops;

/* ---- CPU ---- */

static void detect_cpu(mnemon_hardware_t *hw)
{
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *colon = strchr(line, ':');
                if (colon) {
                    colon++;
                    while (*colon == ' ') colon++;
                    char *nl = strchr(colon, '\n');
                    if (nl) *nl = '\0';
                    snprintf(hw->cpu_model, sizeof(hw->cpu_model), "%s", colon);
                }
                break;
            }
        }
        fclose(fp);
    }

#ifdef _SC_NPROCESSORS_ONLN
    hw->cpu_cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
#else
    hw->cpu_cores = 1;
#endif

#ifdef __GNUC__
    __builtin_cpu_init();
    hw->has_avx2 = __builtin_cpu_supports("avx2");
    hw->has_avx512f = __builtin_cpu_supports("avx512f");
    hw->has_avx512bw = __builtin_cpu_supports("avx512bw");
#endif
}

/* ---- Memory ---- */

static void detect_memory(mnemon_hardware_t *hw)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        unsigned long val;
        if (sscanf(line, "MemTotal: %lu kB", &val) == 1)
            hw->ram_total_bytes = (uint64_t)val * 1024;
        else if (sscanf(line, "MemAvailable: %lu kB", &val) == 1)
            hw->ram_available_bytes = (uint64_t)val * 1024;
    }
    fclose(fp);
}

/* ---- AMD GPU via sysfs ---- */

static void detect_amd_gpu(mnemon_hardware_t *hw)
{
    DIR *dir = opendir("/sys/class/drm");
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Look for card directories (card0, card1, ...) */
        if (strncmp(entry->d_name, "card", 4) != 0) continue;
        /* Skip sub-connectors like card1-DP-1 */
        if (strchr(entry->d_name + 4, '-') != NULL) continue;

        char vendor_path[512], vram_path[512], gtt_path[512], vbios_path[512];
        snprintf(vendor_path, sizeof(vendor_path),
                 "/sys/class/drm/%s/device/vendor", entry->d_name);

        FILE *fp = fopen(vendor_path, "r");
        if (!fp) continue;

        char vendor[16] = {0};
        if (fgets(vendor, sizeof(vendor), fp)) {
            /* Trim newline */
            char *nl = strchr(vendor, '\n');
            if (nl) *nl = '\0';
        }
        fclose(fp);

        /* AMD vendor ID = 0x1002 */
        if (strcmp(vendor, "0x1002") != 0) {
            /* Check Intel (0x8086) */
            if (strcmp(vendor, "0x8086") == 0 && hw->gpu_vendor == MNEMON_GPU_NONE) {
                hw->gpu_vendor = MNEMON_GPU_INTEL;
                snprintf(hw->gpu_model, sizeof(hw->gpu_model), "Intel integrated");
            }
            continue;
        }

        hw->gpu_vendor = MNEMON_GPU_AMD;

        /* Read VRAM total */
        snprintf(vram_path, sizeof(vram_path),
                 "/sys/class/drm/%s/device/mem_info_vram_total", entry->d_name);
        fp = fopen(vram_path, "r");
        if (fp) {
            unsigned long vram;
            if (fscanf(fp, "%lu", &vram) == 1)
                hw->gpu_vram_bytes = (uint64_t)vram;
            fclose(fp);
        }

        /* Read GTT total (GPU-accessible system RAM, important for APUs) */
        snprintf(gtt_path, sizeof(gtt_path),
                 "/sys/class/drm/%s/device/mem_info_gtt_total", entry->d_name);
        fp = fopen(gtt_path, "r");
        if (fp) {
            unsigned long gtt;
            if (fscanf(fp, "%lu", &gtt) == 1)
                hw->gpu_gtt_bytes = (uint64_t)gtt;
            fclose(fp);
        }

        /* Read VBIOS version for model identification */
        snprintf(vbios_path, sizeof(vbios_path),
                 "/sys/class/drm/%s/device/vbios_version", entry->d_name);
        fp = fopen(vbios_path, "r");
        if (fp) {
            char vbios[128] = {0};
            if (fgets(vbios, sizeof(vbios), fp)) {
                char *nl = strchr(vbios, '\n');
                if (nl) *nl = '\0';
            }
            fclose(fp);

            /* Build GPU model string from CPU model (APU) + VBIOS */
            if (strstr(hw->cpu_model, "Radeon") != NULL) {
                /* CPU model already contains GPU name for APUs */
                char *radeon = strstr(hw->cpu_model, "Radeon");
                snprintf(hw->gpu_model, sizeof(hw->gpu_model),
                         "AMD %s", radeon);
            } else {
                snprintf(hw->gpu_model, sizeof(hw->gpu_model),
                         "AMD GPU (%s)", vbios);
            }
        }

        /* Check for ROCm */
        struct stat st;
        if (stat("/dev/kfd", &st) == 0)
            hw->has_rocm = true;

        break; /* Use first AMD GPU found */
    }
    closedir(dir);
}

/* ---- NVIDIA GPU via NVML dlopen ---- */

static void detect_nvidia_gpu(mnemon_hardware_t *hw)
{
    /* Skip if AMD GPU already found */
    if (hw->gpu_vendor == MNEMON_GPU_AMD)
        return;

#ifdef HAVE_DLOPEN
    void *nvml = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
    if (!nvml) return;

    typedef int (*nvmlInit_fn)(void);
    typedef int (*nvmlShutdown_fn)(void);
    typedef int (*nvmlDeviceGetCount_fn)(unsigned int *);
    typedef int (*nvmlDeviceGetHandleByIndex_fn)(unsigned int, void **);
    typedef int (*nvmlDeviceGetName_fn)(void *, char *, unsigned int);

    nvmlInit_fn init = (nvmlInit_fn)dlsym(nvml, "nvmlInit_v2");
    nvmlShutdown_fn shutdown_fn = (nvmlShutdown_fn)dlsym(nvml, "nvmlShutdown");
    nvmlDeviceGetCount_fn get_count =
        (nvmlDeviceGetCount_fn)dlsym(nvml, "nvmlDeviceGetCount_v2");
    nvmlDeviceGetHandleByIndex_fn get_handle =
        (nvmlDeviceGetHandleByIndex_fn)dlsym(nvml, "nvmlDeviceGetHandleByIndex_v2");
    nvmlDeviceGetName_fn get_name =
        (nvmlDeviceGetName_fn)dlsym(nvml, "nvmlDeviceGetName");

    if (!init || !get_count || !get_handle || !get_name) {
        dlclose(nvml);
        return;
    }

    if (init() != 0) { dlclose(nvml); return; }

    unsigned int count = 0;
    get_count(&count);
    if (count > 0) {
        void *device = NULL;
        get_handle(0, &device);
        if (device) {
            hw->gpu_vendor = MNEMON_GPU_NVIDIA;
            get_name(device, hw->gpu_model, sizeof(hw->gpu_model));
        }
    }

    if (shutdown_fn) shutdown_fn();
    dlclose(nvml);
#endif
}

/* ---- NPU (AMD XDNA, Intel NPU) ---- */

static void detect_npu(mnemon_hardware_t *hw)
{
    struct stat st;

    /* Check /sys/class/accel for any accelerator device */
    if (stat("/sys/class/accel/accel0", &st) != 0 &&
        stat("/dev/accel0", &st) != 0)
        return;

    hw->has_npu = true;

    /* Determine NPU vendor from the parent GPU vendor or CPU model */
    if (hw->gpu_vendor == MNEMON_GPU_AMD ||
        strstr(hw->cpu_model, "AMD") != NULL ||
        strstr(hw->cpu_model, "Ryzen") != NULL) {
        snprintf(hw->npu_model, sizeof(hw->npu_model), "AMD XDNA");
    } else if (strstr(hw->cpu_model, "Intel") != NULL) {
        snprintf(hw->npu_model, sizeof(hw->npu_model), "Intel NPU");
    } else {
        snprintf(hw->npu_model, sizeof(hw->npu_model), "AI accelerator");
    }
}

/* ---- NUMA ---- */

static void detect_numa(mnemon_hardware_t *hw)
{
    struct stat st;
    hw->numa_nodes = 1;
    if (stat("/sys/devices/system/node/node1", &st) == 0)
        hw->numa_nodes = 2;
    if (stat("/sys/devices/system/node/node3", &st) == 0)
        hw->numa_nodes = 4;
}

/* ---- NVMe ---- */

static void detect_nvme(mnemon_hardware_t *hw)
{
    struct stat st;
    hw->has_nvme = (stat("/sys/block/nvme0n1", &st) == 0);
}

/* ---- Public API ---- */

mnemon_err_t mnemon_hardware_detect(mnemon_hardware_t *out)
{
    if (!out) return MNEMON_ERR_INVALID_INPUT;
    memset(out, 0, sizeof(*out));

    detect_cpu(out);
    detect_memory(out);
    detect_amd_gpu(out);      /* Check AMD first (most common on AMD CPUs) */
    detect_nvidia_gpu(out);    /* Check NVIDIA if no AMD found */
    detect_npu(out);
    detect_numa(out);
    detect_nvme(out);

    return MNEMON_OK;
}

void mnemon_simd_init(const mnemon_hardware_t *hw)
{
#ifdef HAVE_AVX512
    if (hw && hw->has_avx512f) {
        g_simd_ops.cosine_distance = mnemon_cosine_avx512;
        g_simd_ops.dot_product     = mnemon_dot_avx512;
        g_simd_ops.l2_distance     = mnemon_l2_avx512;
        g_simd_ops.name            = "avx512";
        return;
    }
#endif
#ifdef HAVE_AVX2
    if (hw && hw->has_avx2) {
        g_simd_ops.cosine_distance = mnemon_cosine_avx2;
        g_simd_ops.dot_product     = mnemon_dot_avx2;
        g_simd_ops.l2_distance     = mnemon_l2_avx2;
        g_simd_ops.name            = "avx2";
        return;
    }
#endif
    g_simd_ops.cosine_distance = mnemon_cosine_scalar;
    g_simd_ops.dot_product     = mnemon_dot_scalar;
    g_simd_ops.l2_distance     = mnemon_l2_scalar;
    g_simd_ops.name            = "scalar";
}
