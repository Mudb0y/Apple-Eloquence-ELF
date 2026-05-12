/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/engine.h -- quirks-aware ECI engine wrapper.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_ECI_ENGINE_H
#define SD_ELOQUENCE_ECI_ENGINE_H

#include "runtime.h"
#include "languages.h"
#include "../config.h"

typedef struct {
    EciApi   api;
    ECIHand  h;
    int      sample_rate_param;   /* 0=8k 1=11025 2=22050 */
    int      sample_rate_hz;
    int      current_dialect;
    int      current_voice_slot;
    int          use_dictionaries;
    char         dict_dir[ELOQ_PATH_MAX];
    ECIDictHand  dicts[N_LANGS];   /* one per language, lazily loaded */
    /* Saved at engine_open so engine_switch_language can re-register them
     * on the fresh handle after Delete+NewEx (used for CJK dialects whose
     * SetParam-based switch leaves internal state uninitialized). */
    ECICallback  audio_cb;
    void        *cb_data;
    short       *pcm_chunk;
    int          pcm_chunk_samples;
} EciEngine;

/* Open eci.so at `eci_so_path`, create the engine handle for `initial_dialect`,
 * apply sample_rate_param (falling back to 1 if rejected), register the audio
 * callback. Returns 0 on success; on failure returns -1 and writes a
 * heap-allocated error string to *errmsg. */
int  engine_open(EciEngine *e,
                 const char *eci_so_path,
                 int initial_dialect,
                 int sample_rate_param,
                 ECICallback audio_cb,
                 void *cb_data,
                 short *pcm_chunk,
                 int   pcm_chunk_samples,
                 char **errmsg);

/* Close the engine handle. The shared library stays mapped (deliberate; see
 * runtime.c). Safe to call with !e->h. */
void engine_close(EciEngine *e);

/* Switch to a new language dialect. Non-CJK dialects switch via
 * eciSetParam(eciLanguageDialect, ...); CJK dialects (zh-CN, zh-TW, ja-JP,
 * ko-KR) switch via eciDelete + eciNewEx because their language modules'
 * internal state (notably reset_sent_vars in chsrom/cht) is not properly
 * initialized by SetParam alone — first AddText after a SetParam-based
 * switch to Mandarin SIGSEGVs deep in the engine. NewEx initializes
 * everything cleanly.
 *
 * IMPORTANT: when this returns success for a CJK switch, the engine
 * handle has been REPLACED -- slot-0 voice params have been reset to
 * the language module's defaults. Callers that track per-voice state
 * MUST re-apply it (typically via voice_activate) after the switch. */
int  engine_switch_language(EciEngine *e, int dialect);

/* Pause / resume via eciPause. */
void engine_pause(EciEngine *e, int on);

/* Stop synthesis (eciStop). Safe to call from any thread. */
void engine_stop(EciEngine *e);

/* Returns a heap-allocated version string ("6.1.0.0") via eciVersion. */
char *engine_version(EciEngine *e);

/* Lookup or lazily load the dictionary for `dialect` and apply it via SetDict.
 * No-op if e->use_dictionaries is 0 or no dictionary files exist. Returns 0
 * on success or no-op, -1 on Load/SetDict error. */
int engine_load_dictionary(EciEngine *e, int dialect);

#endif
