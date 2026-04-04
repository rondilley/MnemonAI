/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * model_mgr.c -- Embedding model management
 *
 * When no model is configured, detects system hardware (RAM, GPU)
 * and recommends the best embedding model. Downloads it via libcurl
 * from HuggingFace if not already present.
 *
 * Model selection logic:
 *   >= 4GB RAM:  nomic-embed-text-v1.5 Q8_0 (768-dim, ~150MB) -- best quality
 *   >= 2GB RAM:  nomic-embed-text-v1.5 Q4_K_M (768-dim, ~80MB) -- good quality
 *   <  2GB RAM:  all-MiniLM-L6-v2 Q8_0 (384-dim, ~25MB) -- smallest viable
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ENABLE_CURL
# include <curl/curl.h>
#endif

#include "model_mgr.h"
#include "log.h"

/* Model catalog -- static recommendations */
static const mnemon_model_rec_t models[] = {
    {
        "nomic-embed-text-v1.5 Q8_0 (recommended)",
        "nomic-embed-text-v1.5.Q8_0.gguf",
        "https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/main/nomic-embed-text-v1.5.Q8_0.gguf",
        768,
        157286400,  /* ~150MB */
        4
    },
    {
        "nomic-embed-text-v1.5 Q4_K_M (compact)",
        "nomic-embed-text-v1.5.Q4_K_M.gguf",
        "https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/main/nomic-embed-text-v1.5.Q4_K_M.gguf",
        768,
        83886080,   /* ~80MB */
        2
    },
};

#define MODEL_COUNT (sizeof(models) / sizeof(models[0]))

const mnemon_model_rec_t *mnemon_model_recommend(const mnemon_hardware_t *hw)
{
    int ram_gb = 4; /* default assumption */
    if (hw)
        ram_gb = (int)(hw->ram_total_bytes / (1024ULL * 1024 * 1024));

    /* Select the best model that fits in available RAM */
    for (size_t i = 0; i < MODEL_COUNT; i++) {
        if (ram_gb >= models[i].min_ram_gb)
            return &models[i];
    }

    /* Fallback to smallest */
    return &models[MODEL_COUNT - 1];
}

#ifdef ENABLE_CURL
/* Progress callback for curl download */
static int download_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow)
{
    (void)clientp;
    (void)ultotal;
    (void)ulnow;

    if (dltotal > 0) {
        int pct = (int)(dlnow * 100 / dltotal);
        double mb_done = (double)dlnow / (1024 * 1024);
        double mb_total = (double)dltotal / (1024 * 1024);
        /* Print progress to stderr (log goes there in stdio mode) */
        fprintf(stderr, "\r  downloading: %.1f / %.1f MB (%d%%)",
                mb_done, mb_total, pct);
        fflush(stderr);
    }
    return 0;
}

static mnemon_err_t download_model(const char *url, const char *dest_path)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        mnemon_err_set(MNEMON_ERR_EMBED, 0, "curl_easy_init failed");
        return MNEMON_ERR_EMBED;
    }

    /* Write to temp file, rename on success (atomic) */
    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dest_path);

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        mnemon_err_set(MNEMON_ERR_IO, 0, "cannot create: %s", tmp_path);
        return MNEMON_ERR_IO;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L); /* 10 minute timeout */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, download_progress);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    CURLcode res = curl_easy_perform(curl);
    fclose(fp);
    fprintf(stderr, "\n"); /* newline after progress */

    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        unlink(tmp_path);
        mnemon_err_set(MNEMON_ERR_EMBED, (int)res,
                       "download failed: %s", curl_easy_strerror(res));
        return MNEMON_ERR_EMBED;
    }

    /* Atomic rename */
    if (rename(tmp_path, dest_path) != 0) {
        unlink(tmp_path);
        mnemon_err_set(MNEMON_ERR_IO, 0, "rename failed: %s", dest_path);
        return MNEMON_ERR_IO;
    }

    return MNEMON_OK;
}
#endif /* ENABLE_CURL */

mnemon_err_t mnemon_model_ensure(const char *data_dir,
                                 const char *configured_path,
                                 const mnemon_hardware_t *hw,
                                 char *out_path, size_t out_path_len)
{
    /* "none" or "disable" explicitly disables embedding -- no download */
    if (configured_path &&
        (strcmp(configured_path, "none") == 0 ||
         strcmp(configured_path, "disable") == 0 ||
         strcmp(configured_path, "off") == 0)) {
        out_path[0] = '\0';
        return MNEMON_ERR_EMBED; /* Deliberately disabled */
    }

    /* If a path is explicitly configured and the file exists, use it */
    if (configured_path && configured_path[0] != '\0') {
        struct stat st;
        if (stat(configured_path, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out_path, out_path_len, "%s", configured_path);
            return MNEMON_OK;
        }
        mnemon_log(MNEMON_LOG_WARNING,
                   "configured model not found: %s -- will try auto-download",
                   configured_path);
    }

    /* Select the best model for this hardware */
    const mnemon_model_rec_t *rec = mnemon_model_recommend(hw);

    mnemon_log(MNEMON_LOG_INFO,
               "hardware: %dGB RAM, %s GPU",
               hw ? (int)(hw->ram_total_bytes / (1024ULL * 1024 * 1024)) : 0,
               (hw && hw->gpu_vendor != MNEMON_GPU_NONE) ? hw->gpu_model : "none");
    mnemon_log(MNEMON_LOG_INFO,
               "recommended embedding model: %s (%dMB, %d dimensions)",
               rec->name,
               (int)(rec->size_bytes / (1024 * 1024)),
               rec->dimensions);

    /* Build the expected model path */
    char models_dir[4096];
    snprintf(models_dir, sizeof(models_dir), "%s/models", data_dir);
    mkdir(models_dir, 0700);

    snprintf(out_path, out_path_len, "%s/%s", models_dir, rec->filename);

    /* Check if already downloaded */
    struct stat st;
    if (stat(out_path, &st) == 0 && S_ISREG(st.st_mode) &&
        (size_t)st.st_size > rec->size_bytes / 2) {
        mnemon_log(MNEMON_LOG_INFO, "model already present: %s", out_path);
        return MNEMON_OK;
    }

    /* Download the model */
#ifdef ENABLE_CURL
    mnemon_log(MNEMON_LOG_INFO,
               "downloading embedding model: %s (%dMB)",
               rec->filename,
               (int)(rec->size_bytes / (1024 * 1024)));
    mnemon_log(MNEMON_LOG_INFO, "  from: %s", rec->url);
    mnemon_log(MNEMON_LOG_INFO, "  to:   %s", out_path);

    mnemon_err_t err = download_model(rec->url, out_path);
    if (err != MNEMON_OK) {
        mnemon_log(MNEMON_LOG_ERROR, "model download failed: %s",
                   mnemon_err_msg());
        return err;
    }

    mnemon_log(MNEMON_LOG_INFO, "model downloaded successfully: %s", out_path);
    return MNEMON_OK;
#else
    mnemon_log(MNEMON_LOG_ERROR,
               "no embedding model found and auto-download requires libcurl. "
               "Either:\n"
               "  1. Download manually: curl -L -o %s %s\n"
               "  2. Rebuild with: cmake .. -DENABLE_CURL=ON\n"
               "  3. Set model_path in config to an existing GGUF file",
               out_path, rec->url);
    mnemon_err_set(MNEMON_ERR_EMBED, 0,
                   "no model and libcurl not available for auto-download");
    return MNEMON_ERR_EMBED;
#endif
}
