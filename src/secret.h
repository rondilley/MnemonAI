/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * secret.h -- FSM-based secret pattern detection interface
 */

#ifndef MNEMON_SECRET_H
#define MNEMON_SECRET_H

#include "mnemon.h"

typedef enum {
    MNEMON_SECRET_NONE = 0,
    MNEMON_SECRET_GITHUB_TOKEN,
    MNEMON_SECRET_OPENAI_KEY,
    MNEMON_SECRET_AWS_KEY,
    MNEMON_SECRET_PRIVATE_KEY,
    MNEMON_SECRET_JWT,
    MNEMON_SECRET_API_KEY_ASSIGNMENT,
    MNEMON_SECRET_PASSWORD_ASSIGNMENT,
    MNEMON_SECRET_HIGH_ENTROPY,
} mnemon_secret_type_t;

typedef struct {
    mnemon_secret_type_t type;
    size_t               offset;
    size_t               length;
} mnemon_secret_match_t;

typedef struct {
    mnemon_secret_match_t *matches;
    size_t                 count;
    size_t                 capacity;
} mnemon_secret_result_t;

const char  *mnemon_secret_type_name(mnemon_secret_type_t type);
mnemon_err_t mnemon_secret_scan(const char *data, size_t len,
                                mnemon_secret_result_t *result);
void         mnemon_secret_result_free(mnemon_secret_result_t *result);
bool         mnemon_secret_detected(const char *data, size_t len);
float        mnemon_entropy(const char *data, size_t len);

#endif /* MNEMON_SECRET_H */
