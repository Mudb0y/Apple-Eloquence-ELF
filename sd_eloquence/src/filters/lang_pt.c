/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Derived from NVDA-IBMTTS-Driver/addon/synthDrivers/ibmeci.py
 *   portuguese_ibm_fixes (lines 134-136).
 * Copyright (C) 2009-2026 David CM and contributors, GPL-2.0
 */
#include "filters.h"

#ifdef HAVE_PCRE2
#include <pcre2.h>

filter_rule lang_pt_rules[] = {
    /* Convert HH:00:SS time format to HH:00 SS (ViaVoice time parser fix). */
    { "(\\d{1,2}):(00):(\\d{1,2})", "$1:$2 $3", 0, NULL },

    { NULL, NULL, 0, NULL }
};

#else
filter_rule lang_pt_rules[] = { { NULL, NULL, 0, NULL } };
#endif
