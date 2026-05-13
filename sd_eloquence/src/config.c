/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * config.c -- eloquence.conf parser.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "config.h"

#include "eci/eci.h"
#include "eci/voices.h"
#include "eci/languages.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void config_defaults(EloqConfig *c) {
    memset(c, 0, sizeof(*c));
    c->debug = 0;
    strcpy(c->data_dir, "/usr/lib/eloquence");
    c->dict_dir[0] = 0;
    c->default_sample_rate = 1;
    c->default_voice_slot  = 0;
    c->default_language    = eciGeneralAmericanEnglish;
    c->resample_rate    = 0;
    c->resample_quality = 4;
    c->resample_phase   = 0;
    c->resample_steep   = 0;
    c->use_dictionaries  = 1;
    c->load_abbr_dict    = 0;
    c->phrase_prediction = 0;
    c->backquote_tags    = 0;
    c->rate_boost        = 0;
    c->pause_mode        = 2;
    c->utterance_tail_ms = 25;

    c->voice_head_size         = ELOQ_VOICE_PARAM_UNSET;
    c->voice_roughness         = ELOQ_VOICE_PARAM_UNSET;
    c->voice_breathiness       = ELOQ_VOICE_PARAM_UNSET;
    c->voice_pitch_baseline    = ELOQ_VOICE_PARAM_UNSET;
    c->voice_pitch_fluctuation = ELOQ_VOICE_PARAM_UNSET;
}

static int parse_voice_param(const char *v, int *out) {
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (!end || *end != 0) return -1;
    if (n < 0 || n > 100)  return -1;
    *out = (int)n;
    return 0;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    if (*s == '"' && e > s + 1 && e[-1] == '"') { *--e = 0; s++; }
    return s;
}

static int parse_quality(const char *v) {
    if (!strcasecmp(v, "quick"))     return 0;
    if (!strcasecmp(v, "low"))       return 1;
    if (!strcasecmp(v, "medium"))    return 2;
    if (!strcasecmp(v, "high"))      return 3;
    if (!strcasecmp(v, "very-high") ||
        !strcasecmp(v, "vhq"))       return 4;
    return -1;
}

static int parse_phase(const char *v) {
    if (!strcasecmp(v, "intermediate")) return 0;
    if (!strcasecmp(v, "linear"))       return 1;
    if (!strcasecmp(v, "minimum"))      return 2;
    return -1;
}

int config_apply_kv(EloqConfig *c, const char *key, const char *val) {
    if (!key || !val) return -1;

#define COPY_PATH(field) do { \
        strncpy(c->field, val, sizeof(c->field) - 1); \
        c->field[sizeof(c->field) - 1] = 0; \
    } while (0)

    if      (!strcasecmp(key, "Debug"))                  c->debug = atoi(val);
    else if (!strcasecmp(key, "EloquenceDataDir"))       COPY_PATH(data_dir);
    else if (!strcasecmp(key, "EloquenceDictionaryDir")) COPY_PATH(dict_dir);
    else if (!strcasecmp(key, "EloquenceSampleRate")) {
        int n = atoi(val);
        if (n < 0 || n > 2) return -1;
        c->default_sample_rate = n;
    }
    else if (!strcasecmp(key, "EloquenceDefaultVoice")) {
        char *end = NULL;
        long n = strtol(val, &end, 10);
        if (end && *end == 0 && n >= 0 && n < N_VOICE_PRESETS)
            c->default_voice_slot = (int)n;
        else {
            int slot = voice_find_by_name(val);
            if (slot < 0) return -1;
            c->default_voice_slot = slot;
        }
    }
    else if (!strcasecmp(key, "EloquenceDefaultLanguage")) {
        char *end = NULL;
        long n = strtol(val, &end, 0);
        if (end && *end == 0) {
            if (!lang_by_dialect((int)n)) return -1;
            c->default_language = (int)n;
        } else {
            const LangEntry *L = lang_by_iso(val);
            if (!L) return -1;
            c->default_language = L->eci_dialect;
        }
    }
    else if (!strcasecmp(key, "EloquenceResampleRate"))    c->resample_rate    = atoi(val);
    else if (!strcasecmp(key, "EloquenceResampleQuality")) {
        int q = parse_quality(val); if (q < 0) return -1; c->resample_quality = q;
    }
    else if (!strcasecmp(key, "EloquenceResamplePhase")) {
        int p = parse_phase(val); if (p < 0) return -1; c->resample_phase = p;
    }
    else if (!strcasecmp(key, "EloquenceResampleSteep"))   c->resample_steep   = atoi(val) ? 1 : 0;
    else if (!strcasecmp(key, "EloquenceUseDictionaries"))  c->use_dictionaries  = atoi(val) ? 1 : 0;
    else if (!strcasecmp(key, "EloquenceLoadAbbrDict"))     c->load_abbr_dict    = atoi(val) ? 1 : 0;
    else if (!strcasecmp(key, "EloquencePhrasePrediction")) c->phrase_prediction = atoi(val) ? 1 : 0;
    else if (!strcasecmp(key, "EloquenceBackquoteTags"))    c->backquote_tags    = atoi(val) ? 1 : 0;
    else if (!strcasecmp(key, "EloquenceRateBoost"))        c->rate_boost        = atoi(val) ? 1 : 0;
    else if (!strcasecmp(key, "EloquencePauseMode")) {
        int n = atoi(val); if (n < 0 || n > 2) return -1; c->pause_mode = n;
    }
    else if (!strcasecmp(key, "EloquenceUtteranceTailMs")) {
        int n = atoi(val); if (n < 0 || n > 200) return -1; c->utterance_tail_ms = n;
    }
    else if (!strcasecmp(key, "EloquenceHeadSize"))         return parse_voice_param(val, &c->voice_head_size);
    else if (!strcasecmp(key, "EloquenceRoughness"))        return parse_voice_param(val, &c->voice_roughness);
    else if (!strcasecmp(key, "EloquenceBreathiness"))      return parse_voice_param(val, &c->voice_breathiness);
    else if (!strcasecmp(key, "EloquencePitchBaseline"))    return parse_voice_param(val, &c->voice_pitch_baseline);
    else if (!strcasecmp(key, "EloquencePitchFluctuation")) return parse_voice_param(val, &c->voice_pitch_fluctuation);
    else return -1;  /* unknown key */

#undef COPY_PATH
    return 0;
}

int config_parse_file(EloqConfig *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return 0;
        return -1;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (*p == 0 || *p == '#') continue;
        char *sp = p;
        while (*sp && !isspace((unsigned char)*sp)) sp++;
        if (*sp == 0) continue;
        *sp++ = 0;
        char *val = trim(sp);
        int r = config_apply_kv(c, p, val);
        if (r != 0 && c->debug)
            fprintf(stderr, "sd_eloquence: ignored config '%s = %s'\n", p, val);
    }
    fclose(f);
    return 0;
}

const char *config_effective_dict_dir(const EloqConfig *c) {
    static char buf[ELOQ_PATH_MAX + 8];
    if (c->dict_dir[0]) return c->dict_dir;
    snprintf(buf, sizeof(buf), "%s/dicts", c->data_dir);
    return buf;
}
