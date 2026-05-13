/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * config.h -- eloquence.conf parser. All keys have hardcoded defaults;
 * unknown keys log a debug warning but never fail init.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_CONFIG_H
#define SD_ELOQUENCE_CONFIG_H

#define ELOQ_PATH_MAX 512

typedef struct {
    int  debug;
    char data_dir[ELOQ_PATH_MAX];
    char dict_dir[ELOQ_PATH_MAX];   /* empty -> derive from data_dir + "/dicts" */
    int  default_sample_rate;        /* 0/1/2 */
    int  default_voice_slot;         /* 0..7 */
    int  default_language;           /* ECI dialect code */

    /* libsoxr resampling */
    int  resample_rate;              /* 0 = pass-through */
    int  resample_quality;           /* 0=quick .. 4=very-high */
    int  resample_phase;             /* 0=intermediate 1=linear 2=minimum */
    int  resample_steep;

    /* NVDA-style toggles */
    int  use_dictionaries;           /* default 1 */
    int  phrase_prediction;          /* default 0 */
    int  send_params;                /* default 0 */
    int  backquote_tags;             /* default 0 (security) */
} EloqConfig;

/* Initialize *c with hardcoded defaults. */
void config_defaults(EloqConfig *c);

/* Parse `path` into *c, layering over the existing values. Missing file = OK,
 * use defaults; returns 0. Returns -1 only on fopen errors other than ENOENT.
 * Unknown keys log via fprintf(stderr,...) when c->debug is non-zero. */
int  config_parse_file(EloqConfig *c, const char *path);

/* Parse a single key=value pair (used by tests; bypasses file I/O). Returns
 * 0 on success, -1 on validation error. Whitespace already stripped. */
int  config_apply_kv(EloqConfig *c, const char *key, const char *val);

/* Returns the effective dictionary dir: c->dict_dir if non-empty, otherwise
 * "${c->data_dir}/dicts" in a static buffer (caller must not retain across
 * subsequent calls). */
const char *config_effective_dict_dir(const EloqConfig *c);

#endif
