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
     * same; without these, the prefix injected by send_params/phrase_prediction
     * gets pronounced as "backquote vv eight two ...".
     *
     * eciSynthMode  1 = TTS general text  (vs. 0 = screen-reader mode)
     * eciInputType  1 = phonetic/annotated  (despite the name, this is what
     *                  unlocks backquote-tag parsing on Apple's eci.dylib;
     *                  NVDA's comment notes it's "required to process audio
     *                  properly"). */
    e->api.SetParam(e->h, eciSynthMode, 1);
    e->api.SetParam(e->h, eciInputType, 1);

    /* Save these so engine_switch_language can re-register them on the fresh
     * handle after a Delete+NewEx (CJK path). */
    e->audio_cb           = audio_cb;
    e->cb_data            = cb_data;
    e->pcm_chunk          = pcm_chunk;
    e->pcm_chunk_samples  = pcm_chunk_samples;

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

static int dialect_is_cjk(int dialect) {
    return dialect == eciMandarinChinese    || dialect == eciTaiwaneseMandarin
        || dialect == eciStandardJapanese   || dialect == eciStandardKorean;
}

int engine_switch_language(EciEngine *e, int dialect) {
    if (!e->h || dialect == e->current_dialect) return 0;
    e->api.Stop(e->h);
    e->api.Synchronize(e->h);

    if (dialect_is_cjk(dialect)) {
        /* CJK path: Delete + NewEx. SetParam alone leaves the language
         * module's internal state (reset_sent_vars in chsrom/cht etc.)
         * uninitialized, and the FIRST AddText in the new dialect
         * SIGSEGVs deep inside the engine. NewEx initializes everything
         * cleanly. Phase 0 confirmed fresh-process NewEx works for all
         * four CJK dialects. */
        e->api.Delete(e->h);
        e->h = e->api.NewEx((enum ECILanguageDialect)dialect);
        if (!e->h) return -1;
        /* Re-apply the params engine_open set, on the fresh handle. */
        if (e->api.SetParam(e->h, eciSampleRate, e->sample_rate_param) < 0) {
            e->sample_rate_param = 1;
            e->api.SetParam(e->h, eciSampleRate, 1);
            e->sample_rate_hz = sample_rate_param_to_hz(1);
        }
        e->api.SetParam(e->h, eciSynthMode, 1);
        e->api.SetParam(e->h, eciInputType, 1);
        if (e->audio_cb)
            e->api.RegisterCallback(e->h, e->audio_cb, e->cb_data);
        if (e->pcm_chunk_samples > 0 && e->pcm_chunk)
            e->api.SetOutputBuffer(e->h, e->pcm_chunk_samples, e->pcm_chunk);
        /* The dict cache was tied to the old handle; invalidate so the
         * load below re-builds against the new one. */
        memset(e->dicts, 0, sizeof(e->dicts));
    } else {
        /* Latin path: SetParam works fine here. SetParam sometimes returns
         * -1 but the engine still synthesizes in the new dialect; trust it. */
        e->api.SetParam(e->h, eciLanguageDialect, dialect);
    }

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
