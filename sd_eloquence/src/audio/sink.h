/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * audio/sink.h -- forward PCM to speech-dispatcher's module_tts_output_server.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_AUDIO_SINK_H
#define SD_ELOQUENCE_AUDIO_SINK_H

#include "resampler.h"

#include <stdint.h>

typedef struct {
    Resampler *resampler;
    int        engine_rate_hz;
    int16_t   *scratch;          /* used when resampler is active */
    int        scratch_cap;
} AudioSink;

/* Initialize a sink. If `resampler` is non-NULL the sink will resample;
 * pass NULL for direct pass-through at `engine_rate_hz`. The scratch buffer
 * is allocated internally (resampler ratio × pcm_chunk_samples). */
int  audio_sink_init(AudioSink *s, Resampler *resampler,
                     int engine_rate_hz, int pcm_chunk_samples,
                     char **errmsg);
void audio_sink_dispose(AudioSink *s);

/* Push `n` int16 mono samples through the sink; resamples (if active) and
 * forwards via module_tts_output_server. Returns 0 on success, -1 on
 * resampler or speechd error. */
int  audio_sink_push(AudioSink *s, const int16_t *samples, int n);

/* Drain resampler tail at end of utterance. */
void audio_sink_flush(AudioSink *s);

#endif
