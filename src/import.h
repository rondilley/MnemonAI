/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * import.h -- Bulk import interface
 */

#ifndef MNEMON_IMPORT_H
#define MNEMON_IMPORT_H

#include "mnemon.h"
#include "storage.h"

typedef struct {
    int     imported;
    int     skipped;
    int     errors;
    int     chunks_created;
    int64_t duration_ms;
} mnemon_import_result_t;

typedef struct {
    const char  *source_type;
    const char **tags;
    int          tag_count;
    const char  *chunking;       /* "paragraph", "line", "page", "none" */
    int          max_chunk_size;
    bool         extract_entities;
    bool         preserve_timestamps; /* Use source timestamps for created_at */
} mnemon_import_opts_t;

mnemon_err_t mnemon_import_file(mnemon_storage_t *s, const char *path,
                                const char *format,
                                const mnemon_import_opts_t *opts,
                                mnemon_import_result_t *result);

typedef struct {
    char **chunks;
    int    count;
} mnemon_chunks_t;

mnemon_err_t mnemon_chunk_text(const char *text, size_t len,
                               const char *strategy, int max_chunk_size,
                               mnemon_chunks_t *out);
void mnemon_chunks_free(mnemon_chunks_t *c);

bool mnemon_import_path_allowed(const char *path, const char *allowed_paths);

#endif /* MNEMON_IMPORT_H */
