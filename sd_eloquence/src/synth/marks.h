/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * synth/marks.h -- numeric-id ↔ name table for SSML <mark> support.
 *
 * The encoding is (job_seq << 16) | per_job_idx, so stale marks from a
 * canceled job can be distinguished from current-job marks at callback time.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_SYNTH_MARKS_H
#define SD_ELOQUENCE_SYNTH_MARKS_H

#include <stdint.h>
#include <stdbool.h>

#define MARKS_MAX        256
#define END_STRING_ID    0xFFFFu     /* reserved low-16 sentinel for end-of-string */

/* Initialize / reset the global marks table. Call once at module init. */
void marks_init(void);

/* Register a mark name and return its 32-bit id. Returns 0 if the table is
 * full (caller logs and synthesizes without the mark). NULL name = unregister
 * everything for this job_seq. The 16-bit per-job idx range is 0..0xFFFE;
 * we reserve 0xFFFF for END_STRING. */
uint32_t marks_register(const char *name, uint32_t job_seq);

/* Look up a mark by id. Returns NULL if not in the table or already consumed.
 * Marks the entry consumed so the next lookup returns NULL. */
const char *marks_resolve(uint32_t id);

/* Free every entry whose job_seq matches. Call when a job is freed. */
void marks_release_job(uint32_t job_seq);

/* Helpers. */
static inline uint32_t marks_make_end(uint32_t job_seq) {
    return (job_seq << 16) | END_STRING_ID;
}
static inline uint32_t marks_job_of(uint32_t id) { return id >> 16; }
static inline uint16_t marks_idx_of(uint32_t id) { return (uint16_t)(id & 0xFFFF); }

#endif
