/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Derived from NVDA-IBMTTS-Driver/addon/synthDrivers/ibmeci.py:63-73
 *                                ibm_global_fixes
 * Copyright (C) 2009-2026 David CM and contributors, GPL-2.0
 */
#include "filters.h"

#ifdef HAVE_PCRE2
#include <pcre2.h>

filter_rule lang_global_rules[] = {
    /* Prevent spelling-out when punctuation follows a word. */
    { "([a-z]+)([~#$%^*({|\\\\[<%\\x95])", "$1 $2", PCRE2_CASELESS, NULL },
    /* Don't break phrases like books(s). */
    { "([a-z]+)\\s+(\\(s\\))",             "$1$2",  PCRE2_CASELESS, NULL },
    /* Remove space before punctuation when not followed by letter/digit. */
    { "([a-z]+|\\d+|\\W+)\\s+([:.!;,?](?![a-z]|\\d))",
                                            "$1$2",  PCRE2_CASELESS, NULL },
    /* Reduce two-space separator inside brackets/parens. */
    { "([\\(\\[]+)  (.)",                  "$1$2",  0, NULL },
    { "(.)  ([\\)\\]]+)",                  "$1$2",  0, NULL },
    { NULL, NULL, 0, NULL }
};

#else
filter_rule lang_global_rules[] = { { NULL, NULL, 0, NULL } };
#endif
