/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Derived from NVDA-IBMTTS-Driver/addon/synthDrivers/ibmeci.py
 *   german_ibm_fixes (lines 129-133) and german_fixes (lines 123-128), IBM rules first.
 * Copyright (C) 2009-2026 David CM and contributors, GPL-2.0
 */
#include "filters.h"

#ifdef HAVE_PCRE2
#include <pcre2.h>

filter_rule lang_de_rules[] = {
    /* --- IBM rules first (german_ibm_fixes, lines 129-133) --- */

    /* Crash word: dane-ben */
    { "dane-ben", "dane `0 ben", PCRE2_CASELESS, NULL },

    /* Crash word: dage-gen */
    { "dage-gen", "dage `0 gen", PCRE2_CASELESS, NULL },

    /* --- Non-IBM rules (german_fixes, lines 123-128) --- */
    /* Note: dane-ben and dage-gen already covered above. */

    /* audio/macro/video + hyphen + en... words */
    { "(audio|macro|video)(-)(en[a-z]+)", "$1 `0 $3", PCRE2_CASELESS, NULL },

    { NULL, NULL, 0, NULL }
};

#else
filter_rule lang_de_rules[] = { { NULL, NULL, 0, NULL } };
#endif
