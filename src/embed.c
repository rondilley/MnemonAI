/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * embed.c -- llama.cpp embedding generation
 *
 * Loads a GGUF embedding model (nomic-embed-text-v1.5 Q8_0) via the
 * llama.cpp C API and generates normalized float32 embeddings.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <llama.h>

#include "embed.h"
#include "hardware.h"
#include "log.h"

struct mnemon_embed {
    struct llama_model   *model;
    struct llama_context *ctx;
    int                   n_embd;
    bool                  initialized;
};

/* Route llama.cpp log messages through our logger.
 * WARN/ERROR -> our WARN/ERROR.  INFO/DEBUG -> our DEBUG (suppresses
 * the very verbose per-tensor and metadata dumps during model load). */
static void llama_log_callback(enum ggml_log_level level, const char *text,
                                void *user_data)
{
    (void)user_data;
    if (!text || text[0] == '\0' || text[0] == '\n') return;

    /* Strip trailing newline */
    size_t len = strlen(text);
    char buf[512];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, text, len);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) len--;
    buf[len] = '\0';
    if (len == 0) return;

    switch (level) {
    case GGML_LOG_LEVEL_ERROR:
        mnemon_log(MNEMON_LOG_ERROR, "llama: %s", buf);
        break;
    case GGML_LOG_LEVEL_WARN:
        mnemon_log(MNEMON_LOG_WARNING, "llama: %s", buf);
        break;
    default:
        mnemon_log(MNEMON_LOG_DEBUG, "llama: %s", buf);
        break;
    }
}

/* L2 normalize using SIMD dot product for the norm computation.
 * g_simd_ops.dot_product uses AVX-512 (16 floats/cycle) or AVX2 (8 floats/cycle)
 * instead of scalar (1 float/cycle). At 768 dimensions this is a ~16x speedup
 * for the norm computation. */
static void l2_normalize(float *v, int n)
{
    float norm;
    if (g_simd_ops.dot_product)
        norm = g_simd_ops.dot_product(v, v, (size_t)n); /* sum of squares */
    else {
        norm = 0.0f;
        for (int i = 0; i < n; i++) norm += v[i] * v[i];
    }
    norm = sqrtf(norm);
    if (norm > 1e-8f) {
        /* The division loop is memory-bound, not compute-bound,
         * so SIMD doesn't help much here. */
        for (int i = 0; i < n; i++) v[i] /= norm;
    }
}

mnemon_err_t mnemon_embed_init(mnemon_embed_t **out, const char *model_path,
                               int gpu_layers, int n_threads)
{
    mnemon_embed_t *e;

    if (!out) return MNEMON_ERR_INVALID_INPUT;
    *out = NULL;

    e = calloc(1, sizeof(*e));
    if (!e) return MNEMON_ERR_OOM;

    /* Check if model file exists */
    if (!model_path || model_path[0] == '\0') {
        mnemon_err_set(MNEMON_ERR_EMBED, 0, "no model path configured");
        free(e);
        return MNEMON_ERR_EMBED;
    }

    FILE *fp = fopen(model_path, "rb");
    if (!fp) {
        mnemon_err_set(MNEMON_ERR_EMBED, 0,
                       "model file not found: %s", model_path);
        free(e);
        return MNEMON_ERR_EMBED;
    }
    fclose(fp);

    /* Suppress verbose llama.cpp output (per-tensor, metadata dumps).
     * WARN/ERROR still logged; INFO/DEBUG go to our debug level. */
    llama_log_set(llama_log_callback, NULL);

    /* When GPU is not requested, hide GPU devices from the HIP/CUDA runtime
     * entirely. Without this, the ROCm runtime initializes during
     * llama_backend_init() and can hang on certain GPU targets (e.g.,
     * gfx1151) even when gpu_layers=0. */
    if (gpu_layers <= 0) {
        setenv("HIP_VISIBLE_DEVICES", "-1", 0);   /* 0 = don't overwrite */
        setenv("CUDA_VISIBLE_DEVICES", "-1", 0);
    }

    llama_backend_init();

    /* Load model */
    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = gpu_layers;

    if (gpu_layers > 0) {
        mnemon_log(MNEMON_LOG_INFO,
            "loading embedding model with GPU offload (gpu_layers=%d): %s",
            gpu_layers, model_path);
        mnemon_log(MNEMON_LOG_INFO,
            "first run compiles GPU kernels -- this may take several minutes, "
            "please wait...");
    } else {
        mnemon_log(MNEMON_LOG_INFO, "loading embedding model: %s", model_path);
    }
    e->model = llama_model_load_from_file(model_path, mparams);
    if (!e->model) {
        mnemon_err_set(MNEMON_ERR_EMBED, 0,
                       "failed to load model: %s", model_path);
        llama_backend_free();
        free(e);
        return MNEMON_ERR_EMBED;
    }

    e->n_embd = llama_model_n_embd(e->model);
    mnemon_log(MNEMON_LOG_INFO, "embedding model loaded: %d dimensions",
               e->n_embd);

    /* Create context -- this triggers GPU JIT kernel compilation on first
     * run with ROCm/CUDA. Can take minutes for new GPU targets. */
    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 2048;
    cparams.n_batch = 512;
    cparams.n_threads = n_threads > 0 ? (uint32_t)n_threads : 4;
    cparams.n_threads_batch = cparams.n_threads;
    cparams.embeddings = true;

    if (gpu_layers > 0) {
        mnemon_log(MNEMON_LOG_INFO,
            "creating GPU context (first run compiles GPU kernels -- "
            "this may take several minutes, please wait)...");
    }

    e->ctx = llama_init_from_model(e->model, cparams);
    if (!e->ctx) {
        mnemon_err_set(MNEMON_ERR_EMBED, 0, "failed to create context");
        llama_model_free(e->model);
        llama_backend_free();
        free(e);
        return MNEMON_ERR_EMBED;
    }

    mnemon_log(MNEMON_LOG_INFO, "embedding context ready (gpu_layers=%d)",
               gpu_layers);

    e->initialized = true;
    *out = e;
    return MNEMON_OK;
}

void mnemon_embed_free(mnemon_embed_t *e)
{
    if (!e) return;
    if (e->ctx) llama_free(e->ctx);
    if (e->model) llama_model_free(e->model);
    if (e->initialized) llama_backend_free();
    free(e);
}

mnemon_err_t mnemon_embed_text(mnemon_embed_t *e, const char *text,
                               size_t text_len, float *out, int dimensions)
{
    int n_tokens;
    int32_t max_tokens;

    if (!e || !e->initialized || !text || !out)
        return MNEMON_ERR_INVALID_INPUT;

    if (dimensions != e->n_embd) {
        mnemon_err_set(MNEMON_ERR_EMBED, 0,
                       "dimension mismatch: expected %d, got %d",
                       e->n_embd, dimensions);
        return MNEMON_ERR_EMBED;
    }

    /* Tokenize */
    max_tokens = llama_n_ctx(e->ctx);
    llama_token *tokens = malloc((size_t)max_tokens * sizeof(llama_token));
    if (!tokens) return MNEMON_ERR_OOM;

    n_tokens = llama_tokenize(llama_model_get_vocab(e->model), text,
                              (int32_t)text_len, tokens, max_tokens,
                              true, false);
    if (n_tokens < 0) {
        mnemon_err_set(MNEMON_ERR_EMBED, 0, "tokenization failed");
        free(tokens);
        return MNEMON_ERR_EMBED;
    }

    /* Build batch -- llama_batch_init allocates seq_id[i] arrays internally,
     * so write into them rather than replacing the pointers (which would
     * cause llama_batch_free to free invalid pointers). */
    struct llama_batch batch = llama_batch_init(n_tokens, 0, 1);
    batch.n_tokens = n_tokens;

    for (int i = 0; i < n_tokens; i++) {
        batch.token[i] = tokens[i];
        batch.pos[i] = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = (i == n_tokens - 1) ? 1 : 0;
    }

    /* Decode */
    int rc = llama_decode(e->ctx, batch);
    if (rc != 0) {
        mnemon_err_set(MNEMON_ERR_EMBED, rc, "llama_decode failed: %d", rc);
        llama_batch_free(batch);
        free(tokens);
        return MNEMON_ERR_EMBED;
    }

    /* Extract embeddings */
    float *emb = llama_get_embeddings_seq(e->ctx, 0);
    if (!emb) {
        mnemon_err_set(MNEMON_ERR_EMBED, 0, "no embeddings returned");
        llama_batch_free(batch);
        free(tokens);
        return MNEMON_ERR_EMBED;
    }

    memcpy(out, emb, (size_t)dimensions * sizeof(float));
    l2_normalize(out, dimensions);

    llama_batch_free(batch);
    free(tokens);
    return MNEMON_OK;
}

mnemon_err_t mnemon_embed_batch(mnemon_embed_t *e, const char **texts,
                                const size_t *text_lens, size_t count,
                                float *out, int dimensions)
{
    if (!e || !e->initialized || !texts || !text_lens || !out)
        return MNEMON_ERR_INVALID_INPUT;

    if (dimensions != e->n_embd)
        return MNEMON_ERR_EMBED;

    /* For small batches or when context is tight, fall back to sequential */
    int32_t ctx_size = llama_n_ctx(e->ctx);
    if (count <= 1 || count > 128) {
        /* Sequential fallback for trivial or very large batches */
        for (size_t i = 0; i < count; i++) {
            mnemon_err_t err = mnemon_embed_text(e, texts[i], text_lens[i],
                                    out + (i * (size_t)dimensions), dimensions);
            if (err != MNEMON_OK) return err;
        }
        return MNEMON_OK;
    }

    /*
     * True batch embedding: tokenize all texts, pack into one batch with
     * separate sequence IDs, decode once, extract per-sequence embeddings.
     *
     * This is significantly faster on GPU (one kernel launch for N texts)
     * and marginally faster on CPU (better cache utilization).
     */

    /* Phase 1: tokenize all texts and compute total token count */
    int32_t max_tokens_per_text = ctx_size / (int32_t)count;
    if (max_tokens_per_text < 16) max_tokens_per_text = 16;

    llama_token *all_tokens = malloc((size_t)ctx_size * sizeof(llama_token));
    int *token_counts = calloc(count, sizeof(int));
    int *token_offsets = calloc(count, sizeof(int));
    if (!all_tokens || !token_counts || !token_offsets) {
        free(all_tokens); free(token_counts); free(token_offsets);
        return MNEMON_ERR_OOM;
    }

    int total_tokens = 0;
    for (size_t i = 0; i < count; i++) {
        token_offsets[i] = total_tokens;
        int n = llama_tokenize(llama_model_get_vocab(e->model), texts[i], (int32_t)text_lens[i],
                               all_tokens + total_tokens,
                               max_tokens_per_text, true, false);
        if (n < 0) n = 0;
        token_counts[i] = n;
        total_tokens += n;

        /* If we've exceeded the context, fall back to sequential */
        if (total_tokens >= ctx_size) {
            free(all_tokens); free(token_counts); free(token_offsets);
            for (size_t j = 0; j < count; j++) {
                mnemon_err_t err = mnemon_embed_text(e, texts[j], text_lens[j],
                                        out + (j * (size_t)dimensions), dimensions);
                if (err != MNEMON_OK) return err;
            }
            return MNEMON_OK;
        }
    }

    /* Phase 2: build multi-sequence batch -- write into the seq_id arrays
     * that llama_batch_init allocated rather than replacing the pointers. */
    struct llama_batch batch = llama_batch_init(total_tokens, 0, (int32_t)count);
    batch.n_tokens = total_tokens;

    for (size_t i = 0; i < count; i++) {
        int offset = token_offsets[i];
        int n = token_counts[i];
        for (int j = 0; j < n; j++) {
            int idx = offset + j;
            batch.token[idx] = all_tokens[idx];
            batch.pos[idx] = j; /* position within this sequence */
            batch.n_seq_id[idx] = 1;
            batch.seq_id[idx][0] = (llama_seq_id)i;
            batch.logits[idx] = (j == n - 1) ? 1 : 0; /* logits on last token */
        }
    }

    /* Phase 3: single decode call for all sequences */
    int rc = llama_decode(e->ctx, batch);
    if (rc != 0) {
        llama_batch_free(batch);
        free(all_tokens); free(token_counts); free(token_offsets);
        /* Fall back to sequential on decode failure */
        for (size_t i = 0; i < count; i++) {
            mnemon_err_t err = mnemon_embed_text(e, texts[i], text_lens[i],
                                    out + (i * (size_t)dimensions), dimensions);
            if (err != MNEMON_OK) return err;
        }
        return MNEMON_OK;
    }

    /* Phase 4: extract per-sequence embeddings */
    for (size_t i = 0; i < count; i++) {
        float *emb = llama_get_embeddings_seq(e->ctx, (llama_seq_id)i);
        if (emb) {
            memcpy(out + (i * (size_t)dimensions), emb,
                   (size_t)dimensions * sizeof(float));
            l2_normalize(out + (i * (size_t)dimensions), dimensions);
        } else {
            /* Fallback: zero embedding if extraction fails */
            memset(out + (i * (size_t)dimensions), 0,
                   (size_t)dimensions * sizeof(float));
        }
    }

    llama_batch_free(batch);
    free(all_tokens);
    free(token_counts);
    free(token_offsets);

    return MNEMON_OK;
}

bool mnemon_embed_available(const mnemon_embed_t *e)
{
    return e && e->initialized;
}

int mnemon_embed_dimensions(const mnemon_embed_t *e)
{
    return e ? e->n_embd : 0;
}
