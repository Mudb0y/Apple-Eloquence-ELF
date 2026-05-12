/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * synth/job.h -- synth_job + synth_frame types.
 *
 * A speak request from speechd is parsed into a job. The job's frames are
 * executed by the synth worker in order: text frames go through filters →
 * AddText; mark frames become InsertIndex; prosody/voice/lang push/pop
 * pairs save and restore engine state. Worker code lives in worker.c.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_SYNTH_JOB_H
#define SD_ELOQUENCE_SYNTH_JOB_H

#include <speech-dispatcher/spd_module_main.h>

#include <stddef.h>
#include <stdint.h>

typedef enum {
    FRAME_TEXT,
    FRAME_MARK,
    FRAME_BREAK,
    FRAME_PROSODY_PUSH,
    FRAME_PROSODY_POP,
    FRAME_VOICE_PUSH,
    FRAME_VOICE_POP,
    FRAME_LANG_PUSH,
    FRAME_LANG_POP,
    FRAME_TEXTMODE,
} synth_frame_kind;

typedef struct {
    synth_frame_kind kind;
    union {
        struct { char *text; }                                  text;
        struct { uint32_t id; const char *name; }                mark;
        struct { int millis; }                                   brk;
        struct { int param; int new_value; int saved_value; }    prosody;
        struct { int slot; int saved_slot; }                     voice;
        struct { int dialect; int saved_dialect; }               lang;
        struct { int mode; int saved_mode; }                     textmode;
    } u;
} synth_frame;

typedef struct synth_job {
    struct synth_job *next;
    uint32_t           seq;
    SPDMessageType     msgtype;
    synth_frame       *frames;
    size_t             n_frames;
    size_t             frames_cap;
    char              *arena;       /* bump-allocator for text + mark names */
    size_t             arena_used;
    size_t             arena_cap;
} synth_job;

/* Allocate a job with an initial frame and arena capacity. */
synth_job *synth_job_new(size_t frames_cap, size_t arena_cap);

/* Append a frame (returns pointer to the frame slot to fill, or NULL if
 * the frame array is full). */
synth_frame *synth_job_push_frame(synth_job *j);

/* Allocate `n` bytes from the job arena (returns NULL if exhausted). */
void *synth_job_arena_alloc(synth_job *j, size_t n);

/* Duplicate a string into the job arena. Returns NULL if arena is full or
 * `s` is NULL. */
char *synth_job_arena_strdup(synth_job *j, const char *s);

/* Duplicate a (text, len) chunk into the arena -- not necessarily NUL-terminated
 * in the source; the returned string IS NUL-terminated. */
char *synth_job_arena_strndup(synth_job *j, const char *s, size_t len);

/* Free a job and everything in its arena. */
void synth_job_free(synth_job *j);

#endif
