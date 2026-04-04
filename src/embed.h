/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * embed.h -- llama.cpp embedding generation interface
 */

#ifndef MNEMON_EMBED_H
#define MNEMON_EMBED_H

#include "mnemon.h"

typedef struct mnemon_embed mnemon_embed_t;

mnemon_err_t mnemon_embed_init(mnemon_embed_t **out, const char *model_path,
                               int gpu_layers, int n_threads);
void         mnemon_embed_free(mnemon_embed_t *e);

mnemon_err_t mnemon_embed_text(mnemon_embed_t *e, const char *text,
                               size_t text_len, float *out, int dimensions);
mnemon_err_t mnemon_embed_batch(mnemon_embed_t *e, const char **texts,
                                const size_t *text_lens, size_t count,
                                float *out, int dimensions);
bool mnemon_embed_available(const mnemon_embed_t *e);
int  mnemon_embed_dimensions(const mnemon_embed_t *e);

#endif /* MNEMON_EMBED_H */
