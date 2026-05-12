/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * synth/worker.h -- dedicated synth thread + job queue.
 *
 * The worker owns the EciEngine handle. speechd loop pushes jobs; worker
 * walks frames, calls AddText/InsertIndex/Synthesize/Synchronize, and the
 * ECI callback (registered by engine.c) runs on the worker thread.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_SYNTH_WORKER_H
#define SD_ELOQUENCE_SYNTH_WORKER_H

#include "job.h"
#include "../eci/engine.h"
#include "../audio/sink.h"
#include "../config.h"

typedef struct SynthWorker SynthWorker;

/* Spin up the worker thread. Returns NULL on alloc failure. The worker
 * takes ownership of `engine` and `sink` (it does NOT free them on
 * worker_destroy; they are caller-owned). */
SynthWorker *worker_create(EciEngine *engine, AudioSink *sink, const EloqConfig *cfg);

/* Submit a job. The worker takes ownership; it frees via synth_job_free
 * after execution. */
void worker_submit(SynthWorker *w, synth_job *job);

/* Signal stop. Sets the atomic flag; the worker's ECI callback returns
 * eciDataAbort on the next chunk. Returns immediately (does not block on
 * the worker). */
void worker_request_stop(SynthWorker *w);

/* Pause / resume via eciPause. */
void worker_pause(SynthWorker *w);
void worker_resume(SynthWorker *w);

/* Shut down: signal stop, drain queue, join thread. */
void worker_destroy(SynthWorker *w);

#endif
