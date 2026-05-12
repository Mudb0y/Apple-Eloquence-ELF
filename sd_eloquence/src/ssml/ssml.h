/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ssml/ssml.h -- translate SSML 1.0 (as wrapped by speech-dispatcher)
 * into a synth_job. Falls back to plain-text path if parsing fails.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_SSML_H
#define SD_ELOQUENCE_SSML_H

#include "../synth/job.h"

#include <stddef.h>
#include <stdint.h>

/* Parse `data[0..len)` and produce a synth_job. msgtype controls behavior:
 *   SPD_MSGTYPE_TEXT: full SSML or plain-text fallback
 *   SPD_MSGTYPE_CHAR/KEY/SPELL: no SSML; one FRAME_TEXTMODE(2) + FRAME_TEXT + FRAME_TEXTMODE(restore)
 *   SPD_MSGTYPE_SOUND_ICON: not delivered to speak_sync; not handled here
 *
 * `default_dialect` is the current engine dialect, used as the initial
 * FRAME_LANG context for nested <lang> tags.
 *
 * `job_seq` is the caller-supplied job sequence number, used for mark id encoding.
 *
 * Returns a freshly-allocated job (caller frees via synth_job_free) or NULL
 * on out-of-memory. */
synth_job *ssml_parse(const char *data, size_t len,
                      SPDMessageType msgtype,
                      int default_dialect,
                      uint32_t job_seq);

#endif
