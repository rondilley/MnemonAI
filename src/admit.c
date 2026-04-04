/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * admit.c -- Admission control: filter low-value content
 *
 * Rejects content that is too short, entirely boilerplate greetings,
 * or meta-conversation that adds no informational value to memory.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>
#include <ctype.h>

#include "admit.h"

/* Minimum content length to admit (very short content is usually noise) */
#define MIN_CONTENT_LEN 10

/* Boilerplate patterns that should not be stored as memories */
static const char *boilerplate[] = {
    "hello",
    "hi there",
    "hey",
    "thanks",
    "thank you",
    "ok",
    "okay",
    "got it",
    "sounds good",
    "yes",
    "no",
    "sure",
    "bye",
    "goodbye",
    "see you",
    "good morning",
    "good afternoon",
    "good evening",
    "how are you",
    "i'm good",
    "i am good",
    "what's up",
    "not much",
    "nm",
    "lol",
    "haha",
    "lmao",
    NULL
};

/* Lowercase and trim a string for comparison */
static void normalize(const char *src, size_t len, char *dst, size_t dst_len)
{
    size_t j = 0;

    /* Skip leading whitespace */
    size_t i = 0;
    while (i < len && isspace((unsigned char)src[i])) i++;

    /* Copy lowercase, stop at dst_len-1 */
    while (i < len && j < dst_len - 1) {
        dst[j++] = (char)tolower((unsigned char)src[i++]);
    }

    /* Trim trailing whitespace */
    while (j > 0 && isspace((unsigned char)dst[j-1])) j--;

    /* Strip trailing punctuation */
    while (j > 0 && (dst[j-1] == '.' || dst[j-1] == '!' || dst[j-1] == '?'))
        j--;

    dst[j] = '\0';
}

bool mnemon_admit_check(const char *content, size_t len)
{
    if (!content || len < MIN_CONTENT_LEN)
        return false;

    /* Normalize for comparison */
    char norm[256];
    normalize(content, len, norm, sizeof(norm));

    size_t norm_len = strlen(norm);
    if (norm_len < MIN_CONTENT_LEN)
        return false;

    /* Check against boilerplate patterns */
    for (int i = 0; boilerplate[i] != NULL; i++) {
        if (strcmp(norm, boilerplate[i]) == 0)
            return false;
    }

    return true;
}
