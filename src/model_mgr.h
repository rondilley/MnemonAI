/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * model_mgr.h -- Embedding model management: detection, recommendation, download
 */

#ifndef MNEMON_MODEL_MGR_H
#define MNEMON_MODEL_MGR_H

#include "mnemon.h"
#include "hardware.h"

/* Model recommendation based on hardware */
typedef struct {
    const char *name;           /* Human-readable name */
    const char *filename;       /* GGUF filename */
    const char *url;            /* Download URL */
    int         dimensions;     /* Embedding dimensions */
    size_t      size_bytes;     /* Approximate file size */
    int         min_ram_gb;     /* Minimum RAM required */
} mnemon_model_rec_t;

/* Get the recommended model for the detected hardware.
 * Returns a pointer to a static struct -- do not free. */
const mnemon_model_rec_t *mnemon_model_recommend(const mnemon_hardware_t *hw);

/* Ensure a model exists at the given path.
 * If the file doesn't exist, downloads the recommended model.
 * Returns the path to the model (may differ from input if auto-downloaded).
 * out_path must be at least 4096 bytes.
 * Returns MNEMON_OK if model is ready, MNEMON_ERR_EMBED if download fails. */
mnemon_err_t mnemon_model_ensure(const char *data_dir,
                                 const char *configured_path,
                                 const mnemon_hardware_t *hw,
                                 char *out_path, size_t out_path_len);

#endif /* MNEMON_MODEL_MGR_H */
