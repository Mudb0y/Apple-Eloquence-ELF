/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/engine.c -- quirks-aware engine wrapper.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "engine.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sample_rate_param_to_hz(int p) {
    switch (p) { case 0: return 8000; case 1: return 11025; case 2: return 22050; }
    return 11025;
}

int engine_open(EciEngine *e,
                const char *eci_so_path,
                int initial_dialect,
                int sample_rate_param,
                ECICallback audio_cb,
                void *cb_data,
                short *pcm_chunk,
                int   pcm_chunk_samples,
                char **errmsg) {
    memset(e, 0, sizeof(*e));

    char *err = NULL;
    if (eci_runtime_open(eci_so_path, &e->api, &err) != 0) {
        if (errmsg) *errmsg = err;
        return -1;
    }

    e->h = e->api.NewEx((enum ECILanguageDialect)initial_dialect);
    if (!e->h) {
        const LangEntry *L = lang_by_dialect(initial_dialect);
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "eciNewEx(%#x %s) returned NULL", initial_dialect,
                 L ? L->human : "?");
        if (errmsg) *errmsg = strdup(buf);
        return -1;
    }
    e->current_dialect = initial_dialect;
    e->current_voice_slot = 0;

    /* Apple rejects sample_rate=2 (22050); fall back to 11025. */
    if (e->api.SetParam(e->h, eciSampleRate, sample_rate_param) < 0) {
        sample_rate_param = 1;
        e->api.SetParam(e->h, eciSampleRate, sample_rate_param);
    }
    e->sample_rate_param = sample_rate_param;
    e->sample_rate_hz    = sample_rate_param_to_hz(sample_rate_param);

    if (audio_cb)        e->api.RegisterCallback(e->h, audio_cb, cb_data);
    if (pcm_chunk_samples > 0 && pcm_chunk)
        e->api.SetOutputBuffer(e->h, pcm_chunk_samples, pcm_chunk);
    return 0;
}

void engine_close(EciEngine *e) {
    if (e->h) {
        e->api.Stop(e->h);
        e->api.Delete(e->h);
        e->h = NULL;
    }
    eci_runtime_close();
}

int engine_switch_language(EciEngine *e, int dialect) {
    if (!e->h || dialect == e->current_dialect) return 0;
    e->api.Stop(e->h);
    e->api.Synchronize(e->h);
    /* SetParam sometimes returns -1 but the engine synthesizes in the new
     * dialect anyway; we trust that path because Delete+NewEx crashes
     * Apple's build on the second reload of a language .so. */
    e->api.SetParam(e->h, eciLanguageDialect, dialect);
    e->current_dialect = dialect;
    return 0;
}

void engine_pause(EciEngine *e, int on) {
    if (e->h) e->api.Pause(e->h, on ? ECITrue : ECIFalse);
}

void engine_stop(EciEngine *e) {
    if (e->h) e->api.Stop(e->h);
}

char *engine_version(EciEngine *e) {
    char buf[64] = { 0 };
    e->api.Version(buf);
    return strdup(buf);
}
