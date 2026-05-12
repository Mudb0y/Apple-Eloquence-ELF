/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Derived from NVDA-IBMTTS-Driver/addon/synthDrivers/ibmeci.py
 *   spanish_ibm_fixes (lines 113-118), spanish_ibm_anticrash (lines 119-122),
 *   and spanish_fixes (lines 104-112), IBM rules first.
 * Copyright (C) 2009-2026 David CM and contributors, GPL-2.0
 */
#include "filters.h"

#ifdef HAVE_PCRE2
#include <pcre2.h>

filter_rule lang_es_rules[] = {
    /* --- IBM rules first (spanish_ibm_fixes, lines 113-118) --- */

    /* ViaVoice's time parser is broken for minutes 20-59; convert periods to colons. */
    { "([0-2]?[0-4])\\.([2-5][0-9])\\.([0-5][0-9])", "$1:$2:$3", 0, NULL },

    /* Fix when numbers are separated by space (second number has 3 digits). */
    { "(\\d+) (\\d{3})", "$1  $2", 0, NULL },

    /* --- IBM anticrash rules (spanish_ibm_anticrash, lines 119-122) --- */

    /* Ordinal femenino 0{1,12} followed by \xaa */
    { "\\b(0{1,12})(\xaa)", "$1 $2", 0, NULL },

    /* Ordinal femenino for 12+ digit numbers ending in 1,2,3,6,7,9 */
    { "(\\d{12,}[123679])(\xaa)", "$1 $2", 0, NULL },

    /* --- Non-IBM rules (spanish_fixes, lines 104-112) --- */

    /* Euros: currency symbol + up to 3 digits followed by space-grouped thousands.cents */
    { "([\x80$]\\d{1,3})((\\s\\d{3})+\\.\\d{2})", "$1 $2", 0, NULL },

    /* Fix numbers separated by space where second has 3 digits (non-IBM variant, same pattern). */
    /* Already covered by the IBM rule above; skipping duplicate. */

    /* Ordinal femenino for 12+ digit numbers ending in 1,2,3,6,7,9 (non-IBM variant). */
    /* Already covered by the IBM anticrash rule above; skipping duplicate. */

    { NULL, NULL, 0, NULL }
};

#else
filter_rule lang_es_rules[] = { { NULL, NULL, 0, NULL } };
#endif
