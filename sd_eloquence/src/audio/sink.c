/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * audio/sink.c -- forward PCM to module_tts_output_server.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "sink.h"

#include "spd_audio.h"
#include <speech-dispatcher/spd_module_main.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int audio_sink_init(AudioSink *s, Resampler *r,
                    int engine_rate_hz, int pcm_chunk_samples,
                    char **errmsg) {
    memset(s, 0, sizeof(*s));
    s->resampler = r;
    s->engine_rate_hz = engine_rate_hz;
    if (r && resampler_is_active(r)) {
        /* 8× covers 11025 -> 48000 with headroom for soxr's polyphase
         * flush boundaries; less than that and soxr can write past the
         * end of the buffer. */
        s->scratch_cap = pcm_chunk_samples * 8;
        s->scratch = malloc(s->scratch_cap * sizeof(int16_t));
        if (!s->scratch) {
            if (errmsg) *errmsg = strdup("alloc failure");
            return -1;
        }
    }
    return 0;
}

void audio_sink_dispose(AudioSink *s) {
    free(s->scratch);
    s->scratch = NULL;
}

static void forward(int16_t *samples, int n, int rate_hz) {
    AudioTrack t = {
        .bits         = 16,
        .num_channels = 1,
        .num_samples  = n,
        .sample_rate  = rate_hz,
        .samples      = samples,
    };
    module_tts_output_server(&t, SPD_AUDIO_LE);
}

int audio_sink_push(AudioSink *s, const int16_t *in, int n) {
    if (n <= 0) return 0;

    if (s->resampler && resampler_is_active(s->resampler)) {
        int got = resampler_process(s->resampler, in, n, s->scratch, s->scratch_cap);
        if (got < 0) {
            fprintf(stderr, "sd_eloquence: resampler error\n");
            return -1;
        }
        if (got > 0) {
            int rate = resampler_output_rate(s->resampler);
            forward(s->scratch, got, rate);
        }
        return 0;
    }

    /* Pass-through: forward. The const-correct cast is OK because
     * module_tts_output_server doesn't mutate the buffer despite the
     * non-const pointer. */
    forward((int16_t *)in, n, s->engine_rate_hz);
    return 0;
}

void audio_sink_flush(AudioSink *s) {
    if (!s->resampler || !resampler_is_active(s->resampler) || !s->scratch) return;
    int got = resampler_flush(s->resampler, s->scratch, s->scratch_cap);
    if (got > 0)
        forward(s->scratch, got, resampler_output_rate(s->resampler));
}
