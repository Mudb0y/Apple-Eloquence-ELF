/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * synth/worker.c -- dedicated synth thread + job queue.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "worker.h"

#include "marks.h"
#include "pause_mode.h"
#include "../filters/filters.h"
#include "../eci/voices.h"
#include "../eci/languages.h"

#include <speech-dispatcher/spd_module_main.h>

#include <iconv.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PCM_CHUNK_SAMPLES 8192

struct SynthWorker {
    EciEngine        *engine;
    AudioSink        *sink;
    const EloqConfig *cfg;

    pthread_t         thread;
    pthread_mutex_t   lock;
    pthread_cond_t    cv;
    synth_job        *head;
    synth_job        *tail;

    _Atomic int       running;
    _Atomic int       stop_requested;
    _Atomic int       paused;
    _Atomic uint32_t  current_job_seq;
};

/* The ECI callback needs access to the SynthWorker; we stash it in a single
 * global pointer (one worker per process). */
static SynthWorker *g_worker = NULL;
static int16_t g_pcm_chunk[PCM_CHUNK_SAMPLES];

static enum ECICallbackReturn eci_callback(ECIHand h, enum ECIMessage msg,
                                           long lParam, void *pData) {
    (void)h; (void)pData;
    SynthWorker *w = g_worker;
    if (!w) return eciDataProcessed;

    if (atomic_load(&w->stop_requested))
        return eciDataAbort;

    if (msg == eciWaveformBuffer) {
        if (lParam <= 0) return eciDataProcessed;
        if (lParam > PCM_CHUNK_SAMPLES) lParam = PCM_CHUNK_SAMPLES;
        audio_sink_push(w->sink, g_pcm_chunk, (int)lParam);
        return eciDataProcessed;
    }

    if (msg == eciIndexReply) {
        uint32_t id = (uint32_t)lParam;
        uint32_t job_seq = atomic_load(&w->current_job_seq);
        if ((id >> 16) != job_seq) return eciDataProcessed;  /* stale */
        if ((id & 0xFFFF) == END_STRING_ID) return eciDataProcessed;
        const char *name = marks_resolve(id);
        if (name) module_report_index_mark(name);
        return eciDataProcessed;
    }

    return eciDataProcessed;
}

/* iconv UTF-8 → target encoding. Returns malloc'd buffer (NUL-terminated). */
static char *transcode(const char *in, const char *enc) {
    if (!enc || strcasecmp(enc, "utf-8") == 0) return strdup(in);
    iconv_t cd = iconv_open(enc, "UTF-8");
    if (cd == (iconv_t)-1) return strdup(in);
    size_t in_left = strlen(in);
    size_t out_cap = in_left * 4 + 16;
    char *out = malloc(out_cap);
    if (!out) { iconv_close(cd); return NULL; }
    char *in_p  = (char *)in;
    char *out_p = out;
    size_t out_left = out_cap - 1;
    while (in_left > 0) {
        size_t r = iconv(cd, &in_p, &in_left, &out_p, &out_left);
        if (r == (size_t)-1) {
            /* Skip one byte and substitute '?'. */
            if (out_left == 0) break;
            *out_p++ = '?';
            out_left--;
            in_p++;
            in_left--;
        }
    }
    *out_p = 0;
    iconv_close(cd);
    return out;
}

static void exec_text_frame(SynthWorker *w, synth_frame *f, int dialect) {
    if (!f->u.text.text) return;
    char *filtered = filters_apply(f->u.text.text, dialect);
    if (!filtered) return;
    /* Backquote sanitization on user-supplied text -- has to happen before
     * the pause rewrite below, so our injected `​`p1` annotations survive. */
    if (!w->cfg->backquote_tags) {
        for (char *p = filtered; *p; p++) if (*p == '`') *p = ' ';
    }
    /* PauseMode 2: rewrite punctuation pauses to `​`p1` (NVDA-style). */
    if (w->cfg->pause_mode == 2) {
        char *rewritten = pause_mode_rewrite(filtered);
        free(filtered);
        filtered = rewritten;
        if (!filtered) return;
    }
    char *enc = transcode(filtered, lang_encoding_for(dialect));
    free(filtered);
    if (!enc) return;
    w->engine->api.AddText(w->engine->h, enc);
    free(enc);
}

static int break_factor_for_rate(int eci_speed) {
    /* NVDA's empirical scale: {10:1, 43:2, 60:3, 75:4, 85:5}. */
    static const struct { int r; int f; } tbl[] = {
        {10,1}, {43,2}, {60,3}, {75,4}, {85,5}, {0,0}
    };
    int prev_r = tbl[0].r, prev_f = tbl[0].f;
    if (eci_speed <= prev_r) return prev_f;
    for (int i = 1; tbl[i].r; i++) {
        if (eci_speed <= tbl[i].r) {
            return prev_f + (tbl[i].f - prev_f) * (eci_speed - prev_r) / (tbl[i].r - prev_r);
        }
        prev_r = tbl[i].r; prev_f = tbl[i].f;
    }
    return prev_f;
}

static void exec_break_frame(SynthWorker *w, synth_frame *f) {
    int eci_speed = w->engine->api.GetVoiceParam(w->engine->h, ECI_ACTIVE_SLOT, eciSpeed);
    int factor = break_factor_for_rate(eci_speed);
    int p = (f->u.brk.millis * factor) / 100;  /* rough scaling */
    if (p < 1) p = 1;
    char buf[32];
    snprintf(buf, sizeof(buf), " `p%d ", p);
    w->engine->api.AddText(w->engine->h, buf);
}

static void exec_job(SynthWorker *w, synth_job *j) {
    atomic_store(&w->current_job_seq, j->seq);
    /* Clear any stop signal left over from a previous cancel that targeted
     * an already-finished job. Without this, a fast-cancel-then-speak from
     * Orca (e.g. holding Tab) can land between exec_job returning and the
     * next dequeue, leaving stop_requested=1 — which would otherwise abort
     * this fresh utterance before any audio is generated. The signal that
     * matters for THIS job is set after this point by worker_request_stop. */
    atomic_store(&w->stop_requested, 0);

    int saved_dialect_stack[16] = { 0 };
    int saved_voice_stack[16] = { 0 };
    int saved_mode_stack[16] = { 0 };
    int dialect_top = -1, voice_top = -1, mode_top = -1;
    int current_dialect = w->engine->current_dialect;

    /* Optional pre-utterance phrase-prediction annotation. */
    if (w->cfg->phrase_prediction)
        w->engine->api.AddText(w->engine->h, "`pp1 ");

    for (size_t i = 0; i < j->n_frames; i++) {
        if (atomic_load(&w->stop_requested)) break;
        synth_frame *f = &j->frames[i];
        switch (f->kind) {
            case FRAME_TEXT:
                exec_text_frame(w, f, current_dialect);
                break;
            case FRAME_MARK:
                w->engine->api.InsertIndex(w->engine->h, (int)f->u.mark.id);
                break;
            case FRAME_BREAK:
                exec_break_frame(w, f);
                break;
            case FRAME_PROSODY_PUSH: {
                int saved = w->engine->api.GetVoiceParam(w->engine->h, ECI_ACTIVE_SLOT,
                                                        f->u.prosody.param);
                f->u.prosody.saved_value = saved;
                int new_val = f->u.prosody.new_value;
                /* RateBoost: NVDA's 1.6x multiplier on the SSML-driven rate,
                 * letting fast-reading users break past the natural top end.
                 * Only applies to eciSpeed; clamped to the engine's 0..200. */
                if (f->u.prosody.param == eciSpeed && w->cfg->rate_boost) {
                    new_val = (int)(new_val * 1.6 + 0.5);
                    if (new_val > 200) new_val = 200;
                }
                w->engine->api.SetVoiceParam(w->engine->h, ECI_ACTIVE_SLOT,
                                             f->u.prosody.param, new_val);
                break;
            }
            case FRAME_PROSODY_POP:
                /* Find the most recent matching PUSH on the same param. */
                for (ssize_t k = (ssize_t)i - 1; k >= 0; k--) {
                    if (j->frames[k].kind == FRAME_PROSODY_PUSH) {
                        w->engine->api.SetVoiceParam(w->engine->h, ECI_ACTIVE_SLOT,
                                                     j->frames[k].u.prosody.param,
                                                     j->frames[k].u.prosody.saved_value);
                        break;
                    }
                }
                break;
            case FRAME_VOICE_PUSH:
                if (voice_top + 1 < (int)(sizeof(saved_voice_stack)/sizeof(int))) {
                    saved_voice_stack[++voice_top] = w->engine->current_voice_slot;
                    voice_activate(&w->engine->api, w->engine->h, f->u.voice.slot,
                                   INT_MIN, INT_MIN, INT_MIN, w->cfg);
                    w->engine->current_voice_slot = f->u.voice.slot;
                }
                break;
            case FRAME_VOICE_POP:
                if (voice_top >= 0) {
                    int s = saved_voice_stack[voice_top--];
                    voice_activate(&w->engine->api, w->engine->h, s,
                                   INT_MIN, INT_MIN, INT_MIN, w->cfg);
                    w->engine->current_voice_slot = s;
                }
                break;
            case FRAME_LANG_PUSH:
                if (dialect_top + 1 < (int)(sizeof(saved_dialect_stack)/sizeof(int))) {
                    saved_dialect_stack[++dialect_top] = current_dialect;
                    engine_switch_language(w->engine, f->u.lang.dialect);
                    current_dialect = f->u.lang.dialect;
                }
                break;
            case FRAME_LANG_POP:
                if (dialect_top >= 0) {
                    int d = saved_dialect_stack[dialect_top--];
                    engine_switch_language(w->engine, d);
                    current_dialect = d;
                }
                break;
            case FRAME_TEXTMODE:
                if (mode_top + 1 < (int)(sizeof(saved_mode_stack)/sizeof(int))) {
                    saved_mode_stack[++mode_top] = f->u.textmode.saved_mode;
                    w->engine->api.SetParam(w->engine->h, eciTextMode, f->u.textmode.mode);
                }
                break;
        }
    }

    /* PauseMode 1: a single short pause at the very end of the utterance,
     * shortening the long inter-utterance pause the engine would otherwise
     * emit at a final period.  NVDA-IBMTTS does the same. */
    if (w->cfg->pause_mode == 1)
        w->engine->api.AddText(w->engine->h, "`p1 ");

    /* End-of-string mark to detect completion. */
    w->engine->api.InsertIndex(w->engine->h, (int)marks_make_end(j->seq));

    w->engine->api.Synthesize(w->engine->h);
    w->engine->api.Synchronize(w->engine->h);
    audio_sink_flush(w->sink);

    /* Pad with ~100ms of trailing silence at the engine's native rate.
     * speech-dispatcher's audio backend (pulse/alsa) can clip the last
     * few ms of an utterance when the stream drains at end-of-job; the
     * trailing silence absorbs that clip so the real speech reaches
     * the user intact.  Skip on stop_requested -- a cancel should end
     * promptly without the extra tail. */
    if (!atomic_load(&w->stop_requested)) {
        static const int16_t silence[2205] = { 0 }; /* 100ms at 22050 Hz */
        int n = w->engine->sample_rate_hz / 10;
        if (n > (int)(sizeof(silence) / sizeof(silence[0])))
            n = (int)(sizeof(silence) / sizeof(silence[0]));
        if (n > 0) {
            audio_sink_push(w->sink, silence, n);
            audio_sink_flush(w->sink);
        }
    }
    fflush(stdout);

    if (atomic_load(&w->stop_requested))
        module_report_event_stop();
    else
        module_report_event_end();

    marks_release_job(j->seq);
    synth_job_free(j);

    atomic_store(&w->stop_requested, 0);
}

static void *worker_main(void *ud) {
    SynthWorker *w = ud;
    while (atomic_load(&w->running)) {
        pthread_mutex_lock(&w->lock);
        while (atomic_load(&w->running) && !w->head)
            pthread_cond_wait(&w->cv, &w->lock);
        synth_job *j = w->head;
        if (j) {
            w->head = j->next;
            if (!w->head) w->tail = NULL;
            j->next = NULL;
        }
        pthread_mutex_unlock(&w->lock);
        if (!j) continue;

        /* No pre-exec stop-flag check here: worker_request_stop drains
         * the queue itself (reporting event_stop for each job already
         * queued at cancel time), so anything we successfully dequeue is
         * a post-cancel submission. exec_job clears stop_requested at
         * entry to absorb any signal left over from a cancel that landed
         * after the previous job already finished naturally. */
        exec_job(w, j);
    }
    return NULL;
}

SynthWorker *worker_create(EciEngine *engine, AudioSink *sink, const EloqConfig *cfg) {
    SynthWorker *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->engine = engine;
    w->sink = sink;
    w->cfg  = cfg;
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cv, NULL);
    atomic_store(&w->running, 1);
    g_worker = w;
    /* Re-register the engine callback to point at OUR callback.
     * engine_open may have wired a different one (or NULL); we always own it
     * once a worker is created. */
    engine->api.RegisterCallback(engine->h, eci_callback, NULL);
    engine->api.SetOutputBuffer(engine->h, PCM_CHUNK_SAMPLES, g_pcm_chunk);
    if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
        free(w);
        g_worker = NULL;
        return NULL;
    }
    return w;
}

void worker_submit(SynthWorker *w, synth_job *job) {
    pthread_mutex_lock(&w->lock);
    job->next = NULL;
    if (w->tail) w->tail->next = job;
    else         w->head = job;
    w->tail = job;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->lock);
}

void worker_request_stop(SynthWorker *w) {
    atomic_store(&w->stop_requested, 1);
    engine_stop(w->engine);
    /* Drain queued (not yet started) jobs -- report stop for each. */
    pthread_mutex_lock(&w->lock);
    synth_job *j = w->head;
    while (j) {
        synth_job *next = j->next;
        module_report_event_stop();
        marks_release_job(j->seq);
        synth_job_free(j);
        j = next;
    }
    w->head = w->tail = NULL;
    pthread_mutex_unlock(&w->lock);
}

void worker_pause(SynthWorker *w) {
    atomic_store(&w->paused, 1);
    engine_pause(w->engine, 1);
}

void worker_resume(SynthWorker *w) {
    atomic_store(&w->paused, 0);
    engine_pause(w->engine, 0);
}

void worker_destroy(SynthWorker *w) {
    if (!w) return;
    atomic_store(&w->running, 0);
    pthread_mutex_lock(&w->lock);
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread, NULL);
    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->cv);
    g_worker = NULL;
    free(w);
}
