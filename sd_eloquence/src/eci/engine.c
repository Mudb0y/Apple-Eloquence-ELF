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
#include <unistd.h>

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

    /* Enable annotation parsing so backquote control tags ("`vv82", "`vs55",
     * "`pp1", "`p<N>", "`l<dialect>", "`ts1", etc.) are interpreted as
     * directives rather than spoken literally. NVDA's _ibmeci.py does the
     * same; without this, the prefix injected by phrase_prediction
     * gets pronounced as "backquote p p one ...".
     *
     * eciSynthMode  1 = TTS general text  (vs. 0 = screen-reader mode)
     * eciInputType  1 = phonetic/annotated  (despite the name, this is what
     *                  unlocks backquote-tag parsing on Apple's eci.dylib;
     *                  NVDA's comment notes it's "required to process audio
     *                  properly"). */
    e->api.SetParam(e->h, eciSynthMode, 1);
    e->api.SetParam(e->h, eciInputType, 1);

    if (audio_cb)        e->api.RegisterCallback(e->h, audio_cb, cb_data);
    if (pcm_chunk_samples > 0 && pcm_chunk)
        e->api.SetOutputBuffer(e->h, pcm_chunk_samples, pcm_chunk);
    return 0;
}

void engine_close(EciEngine *e) {
    if (e->h) {
        for (int i = 0; i < N_LANGS; i++) {
            if (e->dicts[i]) {
                e->api.DeleteDict(e->h, e->dicts[i]);
                e->dicts[i] = NULL;
            }
        }
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
    engine_load_dictionary(e, dialect);
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

int engine_load_dictionary(EciEngine *e, int dialect) {
    if (!e->use_dictionaries) return 0;
    const LangEntry *L = lang_by_dialect(dialect);
    if (!L) return 0;
    int idx = lang_index(L);
    if (idx < 0) return 0;
    if (e->dicts[idx]) {
        e->api.SetDict(e->h, e->dicts[idx]);
        return 0;
    }
    char path[ELOQ_PATH_MAX + 64];
    int any = 0;
    ECIDictHand d = e->api.NewDict(e->h);
    if (!d) return -1;
    static const struct {
        const char *suffix;
        enum ECIDictVolume vol;
    } files[] = {
        { "main.dic", eciMainDict },
        { "root.dic", eciRootDict },
        { "abbr.dic", eciAbbvDict },
    };
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]); i++) {
        /* Skip the abbreviation dictionary unless explicitly opted in --
         * Eloquence's abbr expansion is opinionated and surprising for
         * screen-reader text, so we keep it off by default. */
        if (files[i].vol == eciAbbvDict && !e->load_abbr_dict)
            continue;
        snprintf(path, sizeof(path), "%s/%s%s", e->dict_dir, L->langid, files[i].suffix);
        if (access(path, R_OK) == 0) {
            if (e->api.LoadDict(e->h, d, files[i].vol, path) == DictNoError)
                any = 1;
        }
    }
    if (!any) {
        e->api.DeleteDict(e->h, d);
        return 0;
    }
    e->dicts[idx] = d;
    e->api.SetDict(e->h, d);
    return 0;
}
