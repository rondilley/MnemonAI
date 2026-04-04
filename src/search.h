/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * search.h -- Hybrid retrieval and RRF ranking interface
 */

#ifndef MNEMON_SEARCH_H
#define MNEMON_SEARCH_H

#include "mnemon.h"
#include "storage.h"

/* Hybrid search: dispatches to graph, vector, and keyword rankers,
   fuses results via Reciprocal Rank Fusion (RRF, k=60). */
mnemon_err_t mnemon_search_hybrid(mnemon_storage_t *s,
                                  const mnemon_query_t *q,
                                  mnemon_result_set_t *out);

/* Vector-only search */
mnemon_err_t mnemon_search_semantic(mnemon_storage_t *s,
                                    const mnemon_query_t *q,
                                    mnemon_result_set_t *out);

/* FTS5 BM25 keyword-only search */
mnemon_err_t mnemon_search_keyword(mnemon_storage_t *s,
                                   const mnemon_query_t *q,
                                   mnemon_result_set_t *out);

#endif /* MNEMON_SEARCH_H */
