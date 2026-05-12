/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/languages.c -- dialect/iso table.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "languages.h"

#include <strings.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

const LangEntry g_langs[N_LANGS] = {
    { eciGeneralAmericanEnglish, 1,  0, "enu.so", "enu", "en", "us", "American English" },
    { eciBritishEnglish,         1,  1, "eng.so", "eng", "en", "gb", "British English" },
    { eciCastilianSpanish,       2,  0, "esp.so", "esp", "es", "es", "Castilian Spanish" },
    { eciMexicanSpanish,         2,  1, "esm.so", "esm", "es", "mx", "Latin American Spanish" },
    { eciStandardFrench,         3,  0, "fra.so", "fra", "fr", "fr", "French" },
    { eciCanadianFrench,         3,  1, "frc.so", "frc", "fr", "ca", "Canadian French" },
    { eciStandardGerman,         4,  0, "deu.so", "deu", "de", "de", "German" },
    { eciStandardItalian,        5,  0, "ita.so", "ita", "it", "it", "Italian" },
    { eciMandarinChinese,        6,  0, "chs.so", "chs", "zh", "cn", "Mandarin Chinese (Simplified)" },
    { eciTaiwaneseMandarin,      6,  1, "cht.so", "cht", "zh", "tw", "Mandarin Chinese (Traditional)" },
    { eciBrazilianPortuguese,    7,  0, "ptb.so", "ptb", "pt", "br", "Brazilian Portuguese" },
    { eciStandardJapanese,       8,  0, "jpn.so", "jpn", "ja", "jp", "Japanese" },
    { eciStandardFinnish,        9,  0, "fin.so", "fin", "fi", "fi", "Finnish" },
    { eciStandardKorean,        10,  0, "kor.so", "kor", "ko", "kr", "Korean" },
};

LangState g_lang_state[N_LANGS] = { 0 };  /* set by engine.c at init */

const LangEntry *lang_by_dialect(int dialect) {
    for (int i = 0; i < N_LANGS; i++)
        if (g_langs[i].eci_dialect == dialect) return &g_langs[i];
    return NULL;
}

const LangEntry *lang_by_iso(const char *tag) {
    if (!tag) return NULL;
    /* Try exact "lang-region" match first. */
    for (int i = 0; i < N_LANGS; i++) {
        char ietf[16];
        snprintf(ietf, sizeof(ietf), "%s-%s",
                 g_langs[i].iso_lang, g_langs[i].iso_variant);
        if (strcasecmp(tag, ietf) == 0) return &g_langs[i];
    }
    /* Then fall back to "lang" prefix match (any region). */
    for (int i = 0; i < N_LANGS; i++) {
        size_t L = strlen(g_langs[i].iso_lang);
        if (strncasecmp(tag, g_langs[i].iso_lang, L) == 0 &&
            (tag[L] == 0 || tag[L] == '-' || tag[L] == '_'))
            return &g_langs[i];
    }
    return NULL;
}

int lang_index(const LangEntry *L) {
    if (!L || L < g_langs || L >= g_langs + N_LANGS) return -1;
    return (int)(L - g_langs);
}

const char *lang_encoding_for(int dialect) {
    switch (dialect) {
        case eciMandarinChinese:   return "gb18030";
        case eciStandardJapanese:  return "cp932";
        case eciStandardKorean:    return "cp949";
        case eciHongKongCantonese: return "big5";
        default:                   return "cp1252";
    }
}
