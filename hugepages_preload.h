/*
 * Copyright (c) 2020 Mark Vainomaa
 *
 * This source code is proprietary software and must not be distributed and/or copied without the express permission of Mark Vainomaa
 */
#pragma once

#include <inttypes.h>

#define PREFIX "[hugepage_fix_preload]"

static int parse_humanreadable(char *, uintmax_t *);
static inline void *get_sym(const char *);
static int rewrite_meminfo(uintmax_t, char **);
