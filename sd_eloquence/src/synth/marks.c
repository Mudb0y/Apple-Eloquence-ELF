/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * synth/marks.c -- flat-table mark registry.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "marks.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    uint32_t id;
    char    *name;
    uint32_t job_seq;
    bool     in_use;
    bool     consumed;
} MarkEntry;

/* Grows on demand from MARKS_MAX; a single long utterance carrying word-level
 * <mark>s registers all its marks before any is consumed, so the live count
 * can far exceed the initial size.  A hard cap here dropped marks past the
 * limit (breaking caret/word tracking on long reads).  All access is under
 * g_lock, and mark names are separately heap-allocated, so realloc'ing this
 * array never invalidates a name pointer already returned by marks_resolve. */
static MarkEntry *g_table = NULL;
static size_t     g_cap   = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t g_next_idx_per_job[65536];   /* tracks per-job counter (small) */
/* In practice only a handful of jobs are in flight; using a 64K array is
 * cheap (128 KB) and keeps the lookup O(1). The wrap to 0 every 0xFFFE marks
 * is fine because each job_seq has its own counter and entries are released
 * when the job is freed. */

void marks_init(void) {
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_cap; i++) {
        free(g_table[i].name);
        g_table[i].name = NULL;
    }
    if (!g_table) {
        MarkEntry *t = calloc(MARKS_MAX, sizeof(*t));
        if (t) { g_table = t; g_cap = MARKS_MAX; }
    } else {
        memset(g_table, 0, g_cap * sizeof(*g_table));
    }
    memset(g_next_idx_per_job, 0, sizeof(g_next_idx_per_job));
    pthread_mutex_unlock(&g_lock);
}

uint32_t marks_register(const char *name, uint32_t job_seq) {
    if (!name) return 0;
    pthread_mutex_lock(&g_lock);
    uint16_t idx = g_next_idx_per_job[job_seq & 0xFFFF];
    if (idx >= END_STRING_ID - 1) {  /* exhausted, drop */
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    /* Find a free (or already-consumed, reusable) slot. */
    size_t slot = g_cap;
    for (size_t i = 0; i < g_cap; i++) {
        if (!g_table[i].in_use || g_table[i].consumed) { slot = i; break; }
    }
    if (slot == g_cap) {
        /* All slots live: grow the table.  Safe under the lock; names live on
         * the heap, so moving the entry array leaves resolved names valid. */
        size_t ncap = g_cap ? g_cap * 2 : MARKS_MAX;
        MarkEntry *nt = realloc(g_table, ncap * sizeof(*nt));
        if (!nt) {
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        memset(nt + g_cap, 0, (ncap - g_cap) * sizeof(*nt));
        slot = g_cap;
        g_table = nt;
        g_cap   = ncap;
    }
    g_next_idx_per_job[job_seq & 0xFFFF] = idx + 1;
    free(g_table[slot].name);
    g_table[slot].id       = (job_seq << 16) | idx;
    g_table[slot].name     = strdup(name);
    g_table[slot].job_seq  = job_seq;
    g_table[slot].in_use   = true;
    g_table[slot].consumed = false;
    uint32_t id = g_table[slot].id;
    pthread_mutex_unlock(&g_lock);
    return id;
}

const char *marks_resolve(uint32_t id) {
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_cap; i++) {
        if (g_table[i].in_use && !g_table[i].consumed && g_table[i].id == id) {
            const char *name = g_table[i].name;
            g_table[i].consumed = true;
            pthread_mutex_unlock(&g_lock);
            return name;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

void marks_release_job(uint32_t job_seq) {
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_cap; i++) {
        if (g_table[i].in_use && g_table[i].job_seq == job_seq) {
            free(g_table[i].name);
            memset(&g_table[i], 0, sizeof(g_table[i]));
        }
    }
    g_next_idx_per_job[job_seq & 0xFFFF] = 0;
    pthread_mutex_unlock(&g_lock);
}
