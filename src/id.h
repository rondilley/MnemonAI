/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * id.h -- UUIDv7 interface
 */

#ifndef MNEMON_ID_H
#define MNEMON_ID_H

#include "mnemon.h"

typedef struct {
    uint8_t bytes[16];
} mnemon_uuid_t;

mnemon_err_t mnemon_uuid_generate(mnemon_uuid_t *out);
mnemon_err_t mnemon_uuid_to_string(const mnemon_uuid_t *id,
                                   char *buf, size_t len);
mnemon_err_t mnemon_uuid_from_string(const char *str, mnemon_uuid_t *out);
int          mnemon_uuid_compare(const mnemon_uuid_t *a,
                                 const mnemon_uuid_t *b);
bool         mnemon_uuid_is_zero(const mnemon_uuid_t *id);

#endif /* MNEMON_ID_H */
