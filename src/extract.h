/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * extract.h -- Entity extraction via external LLM interface
 *
 * Phase 4 implementation. Phase 1 provides the interface definition.
 */

#ifndef MNEMON_EXTRACT_H
#define MNEMON_EXTRACT_H

#include "mnemon.h"

typedef struct mnemon_extract mnemon_extract_t;

typedef struct {
    mnemon_entity_t *entities;
    int              entity_count;
    mnemon_edge_t   *edges;
    int              edge_count;
} mnemon_extraction_result_t;

/* Initialize extraction client. Returns MNEMON_ERR_EXTRACTION if
   endpoint is not configured or not reachable. */
mnemon_err_t mnemon_extract_init(mnemon_extract_t **out,
                                 const char *endpoint,
                                 const char *model,
                                 int timeout_ms);
void         mnemon_extract_free(mnemon_extract_t *e);

/* Extract entities and relations from text */
mnemon_err_t mnemon_extract_from_text(mnemon_extract_t *e,
                                      const char *text,
                                      mnemon_extraction_result_t *result);
void         mnemon_extraction_result_free(mnemon_extraction_result_t *r);

/* Check if extraction is available */
bool mnemon_extract_available(const mnemon_extract_t *e);

#endif /* MNEMON_EXTRACT_H */
