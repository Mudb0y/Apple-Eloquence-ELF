/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * synth/job.c -- synth_job allocation and arena management.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "job.h"

#include <stdlib.h>
#include <string.h>

synth_job *synth_job_new(size_t frames_cap, size_t arena_cap) {
    synth_job *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->frames = calloc(frames_cap, sizeof(synth_frame));
    if (!j->frames) { free(j); return NULL; }
    j->n_frames = 0;
    j->frames_cap = frames_cap;
    j->arena = malloc(arena_cap);
    if (!j->arena) { free(j->frames); free(j); return NULL; }
    j->arena_used = 0;
    j->arena_cap  = arena_cap;
    return j;
}

synth_frame *synth_job_push_frame(synth_job *j) {
    if (!j) return NULL;
    if (j->n_frames >= j->frames_cap) return NULL;
    synth_frame *f = &j->frames[j->n_frames++];
    memset(f, 0, sizeof(*f));
    return f;
}

void *synth_job_arena_alloc(synth_job *j, size_t n) {
    if (!j) return NULL;
    /* 8-byte align */
    size_t aligned = (j->arena_used + 7) & ~(size_t)7;
    if (aligned + n > j->arena_cap) return NULL;
    void *p = j->arena + aligned;
    j->arena_used = aligned + n;
    return p;
}

char *synth_job_arena_strdup(synth_job *j, const char *s) {
    if (!s) return NULL;
    return synth_job_arena_strndup(j, s, strlen(s));
}

char *synth_job_arena_strndup(synth_job *j, const char *s, size_t len) {
    if (!s) return NULL;
    char *out = synth_job_arena_alloc(j, len + 1);
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = 0;
    return out;
}

void synth_job_free(synth_job *j) {
    if (!j) return;
    free(j->frames);
    free(j->arena);
    free(j);
}
