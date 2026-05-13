/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * audio/resampler.c -- libsoxr wrapper.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "resampler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SOXR
#include <soxr.h>
#endif

struct Resampler {
    int input_hz;
    int output_hz;
    int active;
#ifdef HAVE_SOXR
    soxr_t  soxr;
#endif
};

Resampler *resampler_new(int input_hz, int output_hz,
                         int quality, int phase, int steep,
                         char **errmsg) {
    Resampler *r = calloc(1, sizeof(*r));
    if (!r) { if (errmsg) *errmsg = strdup("alloc failure"); return NULL; }
    r->input_hz = input_hz;
    r->output_hz = output_hz;

    if (output_hz <= 0 || output_hz == input_hz) {
        r->active = 0;
        r->output_hz = input_hz;
        return r;
    }

#ifdef HAVE_SOXR
    static const unsigned long qtbl[] = { SOXR_QQ, SOXR_LQ, SOXR_MQ, SOXR_HQ, SOXR_VHQ };
    int qi = quality;
    if (qi < 0 || qi > 4) qi = 4;
    unsigned long recipe = qtbl[qi];
    switch (phase) {
        case 0: recipe |= SOXR_INTERMEDIATE_PHASE; break;
        case 1: recipe |= SOXR_LINEAR_PHASE;       break;
        case 2: recipe |= SOXR_MINIMUM_PHASE;      break;
    }
    if (steep) recipe |= SOXR_STEEP_FILTER;

    soxr_quality_spec_t qs = soxr_quality_spec(recipe, 0);
    soxr_io_spec_t      is = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);
    soxr_error_t err = NULL;
    r->soxr = soxr_create(input_hz, output_hz, 1, &err, &is, &qs, NULL);
    if (err) {
        if (errmsg) {
            char buf[256];
            snprintf(buf, sizeof(buf), "soxr_create: %s", soxr_strerror(err));
            *errmsg = strdup(buf);
        }
        free(r);
        return NULL;
    }
    r->active = 1;
    return r;
#else
    (void)quality; (void)phase; (void)steep;
    if (errmsg) *errmsg = strdup("resampling requested but built without HAVE_SOXR");
    free(r);
    return NULL;
#endif
}

void resampler_free(Resampler *r) {
    if (!r) return;
#ifdef HAVE_SOXR
    if (r->soxr) soxr_delete(r->soxr);
#endif
    free(r);
}

int resampler_process(Resampler *r,
                      const int16_t *in, int in_samples,
                      int16_t *out, int out_cap) {
    if (!r->active) {
        int n = in_samples < out_cap ? in_samples : out_cap;
        memcpy(out, in, n * sizeof(int16_t));
        return n;
    }
#ifdef HAVE_SOXR
    size_t in_done = 0, out_done = 0;
    soxr_error_t err = soxr_process(r->soxr, in, in_samples, &in_done,
                                    out, out_cap, &out_done);
    if (err) return -1;
    return (int)out_done;
#else
    (void)in; (void)in_samples; (void)out; (void)out_cap;
    return -1;
#endif
}

int resampler_flush(Resampler *r, int16_t *out, int out_cap) {
    if (!r->active) return 0;
#ifdef HAVE_SOXR
    size_t out_done = 0;
    soxr_error_t err = soxr_process(r->soxr, NULL, 0, NULL, out, out_cap, &out_done);
    if (err) return -1;
    return (int)out_done;
#else
    (void)out; (void)out_cap;
    return 0;
#endif
}

void resampler_clear(Resampler *r) {
    if (!r || !r->active) return;
#ifdef HAVE_SOXR
    if (r->soxr) soxr_clear(r->soxr);
#endif
}

int resampler_is_active(const Resampler *r) { return r ? r->active : 0; }
int resampler_output_rate(const Resampler *r) { return r ? r->output_hz : 0; }
