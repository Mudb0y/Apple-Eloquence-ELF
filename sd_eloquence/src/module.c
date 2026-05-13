/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * module.c -- speech-dispatcher entry points.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include <speech-dispatcher/spd_module_main.h>

#include "config.h"
#include "eci/engine.h"
#include "eci/languages.h"
#include "eci/voices.h"
#include "audio/resampler.h"
#include "audio/sink.h"
#include "synth/worker.h"
#include "synth/marks.h"
#include "ssml/ssml.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static EloqConfig   g_cfg;
static EciEngine    g_engine;
static Resampler   *g_resampler = NULL;
static AudioSink    g_sink;
static SynthWorker *g_worker = NULL;
static _Atomic uint32_t g_job_seq = 0;

/* Per-voice SPD rate/pitch/volume overrides; INT_MIN = use preset default. */
static int g_spd_rate[N_VOICE_PRESETS];
static int g_spd_pitch[N_VOICE_PRESETS];
static int g_spd_volume[N_VOICE_PRESETS];

static int enter_data_dir(char **errmsg) {
    char p[1024];
    snprintf(p, sizeof(p), "%s/eci.so", g_cfg.data_dir);
    if (access(p, R_OK) != 0) {
        char buf[2048];
        snprintf(buf, sizeof(buf), "%s not found (set EloquenceDataDir)", p);
        if (errmsg) *errmsg = strdup(buf);
        return -1;
    }
    snprintf(p, sizeof(p), "%s/eci.ini", g_cfg.data_dir);
    if (access(p, R_OK) != 0) {
        char buf[2048];
        snprintf(buf, sizeof(buf), "%s not found (cmake --install should generate it)", p);
        if (errmsg) *errmsg = strdup(buf);
        return -1;
    }
    if (chdir(g_cfg.data_dir) != 0) {
        char buf[2048];
        snprintf(buf, sizeof(buf), "chdir(%s): %s", g_cfg.data_dir, strerror(errno));
        if (errmsg) *errmsg = strdup(buf);
        return -1;
    }
    return 0;
}

int module_config(const char *configfile) {
    config_defaults(&g_cfg);
    if (configfile) config_parse_file(&g_cfg, configfile);
    return 0;
}

int module_init(char **msg) {
    for (int i = 0; i < N_VOICE_PRESETS; i++) {
        g_spd_rate[i]   = INT_MIN;
        g_spd_pitch[i]  = INT_MIN;
        g_spd_volume[i] = INT_MIN;
    }
    marks_init();

    /* CJK dialects gated out: chs/cht/jpn crash mid-utterance via an
     * unrebased function pointer in the converted .so files. korrom is
     * gated for parity until validated. See docs/cjk-investigation/. */
    for (int i = 0; i < N_LANGS; i++) {
        const char *so = g_langs[i].so_name;
        int is_cjk = (strcmp(so, "jpn.so") == 0 || strcmp(so, "kor.so") == 0 ||
                      strcmp(so, "chs.so") == 0 || strcmp(so, "cht.so") == 0);
        g_lang_state[i] = is_cjk ? LANG_DISABLED : LANG_AVAILABLE;
    }

    if (enter_data_dir(msg) != 0) return -1;

    char eci_so[1024];
    snprintf(eci_so, sizeof(eci_so), "%s/eci.so", g_cfg.data_dir);

    /* engine_open passes NULL audio_cb because worker_create will reregister
     * its own callback. */
    if (engine_open(&g_engine, eci_so, g_cfg.default_language,
                    g_cfg.default_sample_rate,
                    NULL, NULL, NULL, 0, msg) != 0)
        return -1;

    /* Wire user-dictionary loading. */
    g_engine.use_dictionaries = g_cfg.use_dictionaries;
    g_engine.load_abbr_dict   = g_cfg.load_abbr_dict;
    {
        const char *dd = config_effective_dict_dir(&g_cfg);
        strncpy(g_engine.dict_dir, dd, sizeof(g_engine.dict_dir) - 1);
        g_engine.dict_dir[sizeof(g_engine.dict_dir) - 1] = 0;
    }
    engine_load_dictionary(&g_engine, g_engine.current_dialect);

    /* Activate the default voice preset into slot 0. */
    voice_activate(&g_engine.api, g_engine.h, g_cfg.default_voice_slot,
                   INT_MIN, INT_MIN, INT_MIN, &g_cfg);
    g_engine.current_voice_slot = g_cfg.default_voice_slot;

    char *re_err = NULL;
    g_resampler = resampler_new(g_engine.sample_rate_hz, g_cfg.resample_rate,
                                g_cfg.resample_quality, g_cfg.resample_phase,
                                g_cfg.resample_steep, &re_err);
    if (!g_resampler) {
        if (re_err && msg) *msg = re_err;
        return -1;
    }
    if (audio_sink_init(&g_sink, g_resampler, g_engine.sample_rate_hz,
                        8192, msg) != 0)
        return -1;

    module_audio_set_server();

    g_worker = worker_create(&g_engine, &g_sink, &g_cfg);
    if (!g_worker) {
        if (msg) *msg = strdup("worker_create failed");
        return -1;
    }

    char ver[64] = { 0 };
    g_engine.api.Version(ver);
    if (msg) {
        char *out = malloc(160);
        if (out) {
            const LangEntry *L = lang_by_dialect(g_engine.current_dialect);
            snprintf(out, 160,
                     "ETI Eloquence %s -- ready (rate=%d Hz, lang=%s, voice=%s)",
                     ver, g_engine.sample_rate_hz,
                     L ? L->human : "?",
                     g_voice_presets[g_engine.current_voice_slot].name);
        }
        *msg = out;
    }
    return 0;
}

int module_close(void) {
    worker_destroy(g_worker);
    g_worker = NULL;
    audio_sink_dispose(&g_sink);
    resampler_free(g_resampler);
    g_resampler = NULL;
    engine_close(&g_engine);
    return 0;
}

int module_audio_set(const char *var, const char *val)  { (void)var; (void)val; return 0; }
int module_audio_init(char **s)                          { if (s) *s = strdup("ok"); return 0; }
int module_loglevel_set(const char *v, const char *l)    {
    if (!strcasecmp(v, "log_level")) g_cfg.debug = atoi(l) > 0;
    return 0;
}
int module_debug(int e, const char *f)                   { g_cfg.debug = e; (void)f; return 0; }

int  module_speak(char *d, size_t b, SPDMessageType t) {
    (void)d; (void)b; (void)t;
    return -1;
}

void module_speak_sync(const char *d, size_t bytes, SPDMessageType t) {
    if (!g_worker) {
        module_speak_error();
        return;
    }
    uint32_t seq = atomic_fetch_add(&g_job_seq, 1) + 1;
    synth_job *job = ssml_parse(d, bytes, t, g_engine.current_dialect, seq);
    if (!job) {
        module_speak_error();
        return;
    }
    module_speak_ok();
    module_report_event_begin();
    worker_submit(g_worker, job);
}

void module_speak_begin(void) {}
void module_speak_end(void)   {}
void module_speak_pause(void) {}
void module_speak_stop(void)  {}

int module_stop(void) {
    if (g_worker) worker_request_stop(g_worker);
    return 0;
}

size_t module_pause(void) {
    if (g_worker) worker_pause(g_worker);
    return 0;
}

int module_set(const char *var, const char *val) {
    if (!g_worker || !var || !val) return 0;

    if (!strcasecmp(var, "rate")) {
        int spd = atoi(val);
        g_spd_rate[g_engine.current_voice_slot] = spd;
        voice_activate(&g_engine.api, g_engine.h, g_engine.current_voice_slot,
                       g_spd_rate[g_engine.current_voice_slot],
                       g_spd_pitch[g_engine.current_voice_slot],
                       g_spd_volume[g_engine.current_voice_slot],
                       &g_cfg);
        return 0;
    }
    if (!strcasecmp(var, "pitch")) {
        g_spd_pitch[g_engine.current_voice_slot] = atoi(val);
        voice_activate(&g_engine.api, g_engine.h, g_engine.current_voice_slot,
                       g_spd_rate[g_engine.current_voice_slot],
                       g_spd_pitch[g_engine.current_voice_slot],
                       g_spd_volume[g_engine.current_voice_slot],
                       &g_cfg);
        return 0;
    }
    if (!strcasecmp(var, "volume")) {
        g_spd_volume[g_engine.current_voice_slot] = atoi(val);
        voice_activate(&g_engine.api, g_engine.h, g_engine.current_voice_slot,
                       g_spd_rate[g_engine.current_voice_slot],
                       g_spd_pitch[g_engine.current_voice_slot],
                       g_spd_volume[g_engine.current_voice_slot],
                       &g_cfg);
        return 0;
    }
    if (!strcasecmp(var, "voice_type")) {
        int slot = voice_find_by_voice_type(val);
        if (slot >= 0) {
            g_engine.current_voice_slot = slot;
            voice_activate(&g_engine.api, g_engine.h, slot,
                           INT_MIN, INT_MIN, INT_MIN, &g_cfg);
        }
        return 0;
    }
    if (!strcasecmp(var, "language")) {
        const LangEntry *L = lang_by_iso(val);
        if (L && L->eci_dialect != g_engine.current_dialect)
            engine_switch_language(&g_engine, L->eci_dialect);
        return 0;
    }
    if (!strcasecmp(var, "synthesis_voice")) {
        /* "Reed-en-US" / "Jacques-fr-fr" -- parse preset name then dialect. */
        int matched = -1;
        size_t name_len = 0;
        for (int i = 0; i < N_VOICE_PRESETS; i++) {
            const VoicePreset *p = &g_voice_presets[i];
            size_t nl = strlen(p->name);
            if (!strncasecmp(val, p->name, nl)) { matched = i; name_len = nl; break; }
            if (p->name_fr) {
                size_t fl = strlen(p->name_fr);
                if (!strncasecmp(val, p->name_fr, fl)) { matched = i; name_len = fl; break; }
            }
        }
        if (matched < 0) return 0;
        g_engine.current_voice_slot = matched;
        voice_activate(&g_engine.api, g_engine.h, matched,
                       INT_MIN, INT_MIN, INT_MIN, &g_cfg);
        if (val[name_len] == '-' && val[name_len + 1]) {
            const LangEntry *L = lang_by_iso(val + name_len + 1);
            if (L && L->eci_dialect != g_engine.current_dialect)
                engine_switch_language(&g_engine, L->eci_dialect);
        }
        return 0;
    }
    if (!strcasecmp(var, "punctuation_mode")) {
        int mode = !strcasecmp(val, "all") ? 2 : 0;
        g_engine.api.SetParam(g_engine.h, eciTextMode, mode);
        return 0;
    }
    return 0;
}

static void uppercase_region(char *p) {
    for (; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
}

SPDVoice **module_list_voices(void) {
    int cap = N_LANGS * N_VOICE_PRESETS;
    SPDVoice **list = calloc(cap + 1, sizeof(SPDVoice *));
    if (!list) return NULL;
    int idx = 0;
    for (int li = 0; li < N_LANGS; li++) {
        if (g_lang_state[li] != LANG_AVAILABLE) continue;
        for (int vi = 0; vi < N_VOICE_PRESETS; vi++) {
            SPDVoice *v = calloc(1, sizeof(SPDVoice));
            if (!v) continue;
            char ietf[16];
            snprintf(ietf, sizeof(ietf), "%s-%s",
                     g_langs[li].iso_lang, g_langs[li].iso_variant);
            uppercase_region(ietf + strlen(g_langs[li].iso_lang) + 1);
            char unique[64];
            const char *vname = voice_display_name(vi, g_langs[li].iso_lang);
            snprintf(unique, sizeof(unique), "%s-%s", vname, ietf);
            v->name     = strdup(unique);
            v->language = strdup(ietf);
            v->variant  = strdup(vname);
            list[idx++] = v;
        }
    }
    list[idx] = NULL;
    return list;
}

int module_loop(void) { return module_process(STDIN_FILENO, 1); }
