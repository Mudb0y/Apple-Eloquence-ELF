/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * filters/filters.h -- per-language anti-crash regex driver.
 *
 * Rules are derived from NVDA-IBMTTS-Driver/addon/synthDrivers/ibmeci.py
 * (Copyright (C) 2009-2026 David CM, GPL-2.0). Attribution in
 * filters/README.GPL.md.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_FILTERS_H
#define SD_ELOQUENCE_FILTERS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *pattern;
    const char *replacement;
    uint32_t    flags;          /* PCRE2_CASELESS, etc.; 0 = none */
    void       *compiled;       /* lazily filled (pcre2_code *) */
} filter_rule;

/* NULL-terminated arrays exported by lang_*.c. */
extern filter_rule lang_global_rules[];
extern filter_rule lang_en_rules[];
extern filter_rule lang_es_rules[];
extern filter_rule lang_fr_rules[];
extern filter_rule lang_de_rules[];
extern filter_rule lang_pt_rules[];

/* Apply the global + per-dialect rule chain to `text` (NUL-terminated).
 * Allocates a new buffer (caller frees) holding the filtered result.
 * Returns NULL only on allocation failure; if PCRE2 isn't compiled in or
 * a rule fails, returns a fresh strdup of `text` unchanged. */
char *filters_apply(const char *text, int dialect);

/* True if the filter engine actually runs regex (HAVE_PCRE2). False = no-op. */
int   filters_active(void);

#endif
