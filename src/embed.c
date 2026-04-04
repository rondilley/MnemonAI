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
#include "log.h"

struct mnemon_embed {
    struct llama_model   *model;
    struct llama_context *ctx;
    int                   n_embd;
    bool                  initialized;
};

static void l2_normalize(float *v, int n)
{
    float norm = 0.0f;
    int i;
    for (i = 0; i < n; i++)
        norm += v[i] * v[i];
    norm = sqrtf(norm);
    if (norm > 1e-8f) {
        for (i = 0; i < n; i++)
            v[i] /= norm;
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

    llama_backend_init();

    /* Load model */
    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = gpu_layers;

    mnemon_log(MNEMON_LOG_INFO, "loading embedding model: %s", model_path);
    e->model = llama_load_model_from_file(model_path, mparams);
    if (!e->model) {
        mnemon_err_set(MNEMON_ERR_EMBED, 0,
                       "failed to load model: %s", model_path);
        llama_backend_free();
        free(e);
        return MNEMON_ERR_EMBED;
    }

    e->n_embd = llama_n_embd(e->model);
    mnemon_log(MNEMON_LOG_INFO, "embedding model loaded: %d dimensions",
               e->n_embd);

    /* Create context */
    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 2048;
    cparams.n_batch = 512;
    cparams.n_threads = n_threads > 0 ? (uint32_t)n_threads : 4;
    cparams.n_threads_batch = cparams.n_threads;
    cparams.embeddings = true;

    e->ctx = llama_new_context_with_model(e->model, cparams);
    if (!e->ctx) {
        mnemon_err_set(MNEMON_ERR_EMBED, 0, "failed to create context");
        llama_free_model(e->model);
        llama_backend_free();
        free(e);
        return MNEMON_ERR_EMBED;
    }

    e->initialized = true;
    *out = e;
    return MNEMON_OK;
}

void mnemon_embed_free(mnemon_embed_t *e)
{
    if (!e) return;
    if (e->ctx) llama_free(e->ctx);
    if (e->model) llama_free_model(e->model);
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

    n_tokens = llama_tokenize(e->model, text, (int32_t)text_len,
                              tokens, max_tokens, true, false);
    if (n_tokens < 0) {
        mnemon_err_set(MNEMON_ERR_EMBED, 0, "tokenization failed");
        free(tokens);
        return MNEMON_ERR_EMBED;
    }

    /* Build batch */
    struct llama_batch batch = llama_batch_init(n_tokens, 0, 1);
    batch.n_tokens = n_tokens;

    for (int i = 0; i < n_tokens; i++) {
        batch.token[i] = tokens[i];
        batch.pos[i] = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i] = (llama_seq_id *[]){&(llama_seq_id){0}}[0];
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
    mnemon_err_t err;
    size_t i;

    if (!e || !texts || !text_lens || !out)
        return MNEMON_ERR_INVALID_INPUT;

    /* Sequential embedding for Phase 1. Batch optimization in Phase 2. */
    for (i = 0; i < count; i++) {
        err = mnemon_embed_text(e, texts[i], text_lens[i],
                                out + (i * (size_t)dimensions), dimensions);
        if (err != MNEMON_OK)
            return err;
    }

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
