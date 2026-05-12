/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/languages.h -- dialect/iso/availability table.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_ECI_LANGUAGES_H
#define SD_ELOQUENCE_ECI_LANGUAGES_H

#include "eci.h"

typedef enum {
    LANG_AVAILABLE = 0,
    LANG_DISABLED  = 1,   /* present on disk but gated out (e.g. CJK known-unstable) */
    LANG_MISSING   = 2,   /* the .so isn't installed */
    LANG_WEDGED    = 3,   /* tripped a SIGSEGV guard or NewEx returned NULL */
} LangState;

typedef struct {
    int   eci_dialect;
    int   ini_major;
    int   ini_minor;
    const char *so_name;       /* e.g. "enu.so" */
    const char *langid;        /* e.g. "enu" -- prefix for main.dic/root.dic/abbr.dic */
    const char *iso_lang;      /* e.g. "en" */
    const char *iso_variant;   /* e.g. "us" */
    const char *human;
} LangEntry;

#define N_LANGS 14
extern const LangEntry g_langs[N_LANGS];

extern LangState g_lang_state[N_LANGS];

/* Lookups. Return NULL / -1 on miss. */
const LangEntry *lang_by_dialect(int dialect);
const LangEntry *lang_by_iso(const char *tag);     /* "en", "en-US", "en_us" */
int              lang_index(const LangEntry *L);   /* offset in g_langs */

/* Encoding for `eci.AddText` payloads on a given dialect (see spec §7.3).
 * Returns one of "cp1252" "gb18030" "cp932" "cp949" "big5". */
const char *lang_encoding_for(int dialect);

#endif
