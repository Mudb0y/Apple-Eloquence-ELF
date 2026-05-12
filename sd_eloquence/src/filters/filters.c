/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * filters/filters.c -- PCRE2 driver. Compiles patterns lazily.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "filters.h"

#include "../eci/eci.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_PCRE2
#include <pcre2.h>

static pcre2_code *compile_rule(filter_rule *r) {
    if (r->compiled) return (pcre2_code *)r->compiled;
    int errnum = 0;
    PCRE2_SIZE erroff = 0;
    pcre2_code *c = pcre2_compile((PCRE2_SPTR)r->pattern,
                                  PCRE2_ZERO_TERMINATED, r->flags,
                                  &errnum, &erroff, NULL);
    if (!c) {
        char err[128];
        pcre2_get_error_message(errnum, (PCRE2_UCHAR *)err, sizeof(err));
        fprintf(stderr, "sd_eloquence: filter compile failed at offset %zu: %s\n  pattern: %s\n",
                (size_t)erroff, err, r->pattern);
        return NULL;
    }
    r->compiled = c;
    return c;
}

static char *apply_array(char *text, filter_rule rules[]) {
    if (!text) return NULL;
    pcre2_match_data *md = NULL;
    for (filter_rule *r = rules; r->pattern; r++) {
        pcre2_code *c = compile_rule(r);
        if (!c) continue;
        if (!md) md = pcre2_match_data_create_from_pattern(c, NULL);
        size_t in_len = strlen(text);
        size_t cap = in_len * 2 + 16;
        char *out = malloc(cap);
        if (!out) break;
        PCRE2_SIZE out_len = cap;
        int rc = pcre2_substitute(c, (PCRE2_SPTR)text, in_len, 0,
                                  PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH,
                                  md, NULL,
                                  (PCRE2_SPTR)r->replacement, PCRE2_ZERO_TERMINATED,
                                  (PCRE2_UCHAR *)out, &out_len);
        if (rc == PCRE2_ERROR_NOMEMORY) {
            free(out);
            out = malloc(out_len + 1);
            if (!out) break;
            cap = out_len + 1;
            out_len = cap;
            rc = pcre2_substitute(c, (PCRE2_SPTR)text, in_len, 0,
                                  PCRE2_SUBSTITUTE_GLOBAL, md, NULL,
                                  (PCRE2_SPTR)r->replacement, PCRE2_ZERO_TERMINATED,
                                  (PCRE2_UCHAR *)out, &out_len);
        }
        if (rc < 0) {
            free(out);
            continue;
        }
        free(text);
        text = out;
        text[out_len] = 0;
    }
    if (md) pcre2_match_data_free(md);
    return text;
}

int filters_active(void) { return 1; }
#else
static char *apply_array(char *text, filter_rule rules[]) {
    (void)rules;
    return text;
}
int filters_active(void) { return 0; }
#endif

static filter_rule *rules_for_dialect(int dialect) {
    switch (dialect) {
        case eciGeneralAmericanEnglish:
        case eciBritishEnglish:        return lang_en_rules;
        case eciCastilianSpanish:
        case eciMexicanSpanish:        return lang_es_rules;
        case eciStandardFrench:
        case eciCanadianFrench:        return lang_fr_rules;
        case eciStandardGerman:        return lang_de_rules;
        case eciBrazilianPortuguese:   return lang_pt_rules;
        default:                        return NULL;
    }
}

char *filters_apply(const char *text, int dialect) {
    if (!text) return NULL;
    char *t = strdup(text);
    if (!t) return NULL;
    t = apply_array(t, lang_global_rules);
    filter_rule *per = rules_for_dialect(dialect);
    if (per) t = apply_array(t, per);
    return t;
}

/* STUB rule arrays so filters.c compiles before lang_*.c land.
 * Each will be replaced by a real array in Tasks E2-E7. */
filter_rule lang_global_rules[] = { {NULL,NULL,0,NULL} };
filter_rule lang_en_rules[]     = { {NULL,NULL,0,NULL} };
filter_rule lang_es_rules[]     = { {NULL,NULL,0,NULL} };
filter_rule lang_fr_rules[]     = { {NULL,NULL,0,NULL} };
filter_rule lang_de_rules[]     = { {NULL,NULL,0,NULL} };
filter_rule lang_pt_rules[]     = { {NULL,NULL,0,NULL} };
