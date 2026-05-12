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

typedef struct {
    EciApi   api;
    ECIHand  h;
    int      sample_rate_param;   /* 0=8k 1=11025 2=22050 */
    int      sample_rate_hz;
    int      current_dialect;
    int      current_voice_slot;
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

/* Switch to a new language dialect. Uses SetParam(eciLanguageDialect)
 * rather than Delete+NewEx because the latter crashes Apple's build on
 * second reload of a language .so. */
int  engine_switch_language(EciEngine *e, int dialect);

/* Pause / resume via eciPause. */
void engine_pause(EciEngine *e, int on);

/* Stop synthesis (eciStop). Safe to call from any thread. */
void engine_stop(EciEngine *e);

/* Returns a heap-allocated version string ("6.1.0.0") via eciVersion. */
char *engine_version(EciEngine *e);

#endif
