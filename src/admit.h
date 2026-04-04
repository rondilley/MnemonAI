/*
 * Copyright (c) 2026, Ron Dilley
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * admit.h -- Admission control: filter low-value content before storage
 */

#ifndef MNEMON_ADMIT_H
#define MNEMON_ADMIT_H

#include "mnemon.h"

/* Check if content should be admitted for storage.
 * Returns true if content passes all admission filters.
 * Returns false if content is boilerplate/greeting/meta-conversation. */
bool mnemon_admit_check(const char *content, size_t len);

#endif /* MNEMON_ADMIT_H */
