/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Derived from NVDA-IBMTTS-Driver/addon/synthDrivers/ibmeci.py
 *   french_ibm_fixes (lines 155-158) and french_fixes (lines 137-154), IBM rules first.
 * Copyright (C) 2009-2026 David CM and contributors, GPL-2.0
 */
#include "filters.h"

#ifdef HAVE_PCRE2
#include <pcre2.h>

filter_rule lang_fr_rules[] = {
    /* --- IBM rules first (french_ibm_fixes, lines 155-158) --- */

    /* Currency symbol before number: remove space before thousands. */
    { "([$\x80\xa3])\\s*(\\d+)\\s(000)", "$1$2$3", 0, NULL },

    /* Number followed by thousands and currency symbol: remove space. */
    { "(\\d+)\\s(000)\\s*([$\x80\xa3])", "$1$2$3", 0, NULL },

    /* --- Non-IBM rules (french_fixes, lines 137-154) --- */

    /* Convert n° to numéro (\xb0 = degree/ordinal sign, \xe9 = é). */
    { "\\bn\xb0", "num\xe9ro", PCRE2_CASELESS, NULL },

    /* Anticrash for "quil" in "anquill" context: replace uil with i. */
    { "(?<=anq)uil(?=l)", "i", PCRE2_CASELESS, NULL },

    /* "quil" before non-word char -> "kil". */
    /* re.I | re.L -> PCRE2_CASELESS (PCRE2 is Unicode by default). */
    { "quil(?=\\W)", "kil", PCRE2_CASELESS, NULL },

    /* Function key names f1-f12 pronounced as "1 franc": add space after f. */
    { "f(?=\\s?\\d)", "f ", PCRE2_CASELESS, NULL },

    /* Fix capitalised Roman numerals followed by 'e'/'eme': collapse split. */
    { "\\b([CDILMVX]+)(\\s?)([CDILMVX]e)(me)?\\b", "$1$3", 0, NULL },

    /* Right parenthesis inside a word: add trailing space. */
    { "(\\w\\))(?=\\w)", "$1 ", PCRE2_CASELESS, NULL },

    /* "nvda" -> spoken phonetically. */
    /* \xe8 = è (U+00E8), \xe9 = é (U+00E9) in Latin-1 / ECI byte encoding. */
    { "(n\\s?vda)", " \xe8" "nv\xe9" "d\xe9" "a ", PCRE2_CASELESS, NULL },

    /* Letter 'y' ignored in some situations: ou/oy/uy -> oi/ui + b/n/p. */
    { "([ou])y([bnp])", "$1i$2", PCRE2_CASELESS, NULL },

    { NULL, NULL, 0, NULL }
};

#else
filter_rule lang_fr_rules[] = { { NULL, NULL, 0, NULL } };
#endif
