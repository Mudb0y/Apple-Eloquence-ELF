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

static MarkEntry g_table[MARKS_MAX];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t g_next_idx_per_job[65536];   /* tracks per-job counter (small) */
/* In practice only a handful of jobs are in flight; using a 64K array is
 * cheap (128 KB) and keeps the lookup O(1). The wrap to 0 every 0xFFFE marks
 * is fine because each job_seq has its own counter and entries are released
 * when the job is freed. */

void marks_init(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MARKS_MAX; i++) {
        free(g_table[i].name);
        memset(&g_table[i], 0, sizeof(g_table[i]));
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
    /* Find a free slot. */
    int slot = -1;
    for (int i = 0; i < MARKS_MAX; i++) {
        if (!g_table[i].in_use) { slot = i; break; }
        if (g_table[i].consumed) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_lock);
        return 0;
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
    for (int i = 0; i < MARKS_MAX; i++) {
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
    for (int i = 0; i < MARKS_MAX; i++) {
        if (g_table[i].in_use && g_table[i].job_seq == job_seq) {
            free(g_table[i].name);
            memset(&g_table[i], 0, sizeof(g_table[i]));
        }
    }
    g_next_idx_per_job[job_seq & 0xFFFF] = 0;
    pthread_mutex_unlock(&g_lock);
}
