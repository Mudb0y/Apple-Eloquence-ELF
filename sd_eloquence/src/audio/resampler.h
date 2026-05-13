/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * audio/resampler.h -- libsoxr wrapper. Pass-through when disabled.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_AUDIO_RESAMPLER_H
#define SD_ELOQUENCE_AUDIO_RESAMPLER_H

#include <stddef.h>
#include <stdint.h>

typedef struct Resampler Resampler;

/* Allocate a resampler going input_hz -> output_hz. output_hz=0 or
 * input_hz==output_hz returns a pass-through resampler (resampler_process
 * just memcpy's). quality is 0..4 (quick..very-high); phase is 0..2
 * (intermediate/linear/minimum); steep is 0 or 1. */
Resampler *resampler_new(int input_hz, int output_hz,
                         int quality, int phase, int steep,
                         char **errmsg);
void resampler_free(Resampler *r);

/* Resample `in_samples` int16 mono samples from `in` into `out` (capacity
 * `out_cap`). Returns count actually written, or -1 on error. */
int  resampler_process(Resampler *r,
                       const int16_t *in, int in_samples,
                       int16_t *out, int out_cap);

/* End-of-utterance drain — pulls one chunk of the polyphase tail from
 * libsoxr.  Returns count written to `out` (0 when fully drained, -1 on
 * error).  Call in a loop until it returns 0. */
int  resampler_flush(Resampler *r, int16_t *out, int out_cap);

/* Reset libsoxr's internal state so the resampler is ready for a new
 * stream.  Call once after resampler_flush() has returned 0, before any
 * further resampler_process() calls. */
void resampler_clear(Resampler *r);

/* True if the resampler is active (not pass-through). */
int  resampler_is_active(const Resampler *r);
int  resampler_output_rate(const Resampler *r);

#endif
