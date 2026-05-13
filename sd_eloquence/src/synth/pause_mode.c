/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * synth/pause_mode.c -- punctuation pause rewriting via pcre2.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "pause_mode.h"

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_PCRE2
#include <pcre2.h>

/* NVDA's pause_re from NVDA-IBMTTS-Driver/synthDrivers/ibmeci.py:
 *
 *     ([a-zA-Z0-9]|\s)([-,.:;)(?!])(\2*?)(\s|[\\/]|$)
 *
 * Group 1 captures a wordchar or whitespace (the char before the punct).
 * Group 2 captures one punctuation char.
 * Group 3 captures any repeats of group 2 (lazy).
 * Group 4 captures the trailing space, backslash, slash, or end-of-input.
 *
 * The replacement "$1 `p1$2$3$4" inserts " `p1" between the leading
 * wordchar and the punctuation, so the engine sees a short one-unit
 * pause annotation instead of its longer natural pause.
 *
 * \\s and \\2 are PCRE escapes; \\\\ is a literal backslash inside the
 * character class. */
static const char PAUSE_RE_PATTERN[] =
    "([a-zA-Z0-9]|\\s)([-,.:;)(?!])(\\2*?)(\\s|[\\\\/]|$)";
static const char PAUSE_RE_REPLACEMENT[] = "$1 `p1$2$3$4";

static pcre2_code *g_pause_re = NULL;
static int         g_pause_re_failed = 0;

static pcre2_code *compile_pause_re(void) {
    if (g_pause_re || g_pause_re_failed) return g_pause_re;
    int errnum;
    PCRE2_SIZE erroff;
    g_pause_re = pcre2_compile((PCRE2_SPTR)PAUSE_RE_PATTERN,
                               PCRE2_ZERO_TERMINATED, 0,
                               &errnum, &erroff, NULL);
    if (!g_pause_re) g_pause_re_failed = 1;
    return g_pause_re;
}

char *pause_mode_rewrite(const char *src) {
    if (!src) return NULL;
    pcre2_code *c = compile_pause_re();
    if (!c) return strdup(src);

    size_t in_len = strlen(src);
    /* Each match inserts " `p1" (4 chars) before the punctuation; bound
     * generously so we don't usually need a second pass. */
    size_t cap = in_len * 2 + 16;
    char *out = malloc(cap);
    if (!out) return strdup(src);

    pcre2_match_data *md = pcre2_match_data_create_from_pattern(c, NULL);
    PCRE2_SIZE out_len = cap;
    int rc = pcre2_substitute(c, (PCRE2_SPTR)src, in_len, 0,
                              PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH,
                              md, NULL,
                              (PCRE2_SPTR)PAUSE_RE_REPLACEMENT,
                              PCRE2_ZERO_TERMINATED,
                              (PCRE2_UCHAR *)out, &out_len);
    if (rc == PCRE2_ERROR_NOMEMORY) {
        free(out);
        cap = out_len + 1;
        out = malloc(cap);
        if (!out) {
            pcre2_match_data_free(md);
            return strdup(src);
        }
        out_len = cap;
        rc = pcre2_substitute(c, (PCRE2_SPTR)src, in_len, 0,
                              PCRE2_SUBSTITUTE_GLOBAL,
                              md, NULL,
                              (PCRE2_SPTR)PAUSE_RE_REPLACEMENT,
                              PCRE2_ZERO_TERMINATED,
                              (PCRE2_UCHAR *)out, &out_len);
    }
    pcre2_match_data_free(md);
    if (rc < 0) {
        free(out);
        return strdup(src);
    }
    return out;
}

#else  /* !HAVE_PCRE2 */

char *pause_mode_rewrite(const char *src) {
    return src ? strdup(src) : NULL;
}

#endif
