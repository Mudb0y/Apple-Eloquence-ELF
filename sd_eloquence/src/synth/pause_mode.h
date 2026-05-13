/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * synth/pause_mode.h -- NVDA-IBMTTS-style punctuation pause rewriting.
 *
 * EloquencePauseMode 2 rewrites a substring of the form
 *   <wordchar-or-space><punct>(<repeated-punct>)?<space-or-eol-or-slash>
 * by inserting `​`p1` after the leading word/space, replacing the long
 * pause the engine emits at punctuation with a short one-unit pause.
 * The exact regex is NVDA's `pause_re` from
 * NVDA-IBMTTS-Driver/addon/synthDrivers/ibmeci.py.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_SYNTH_PAUSE_MODE_H
#define SD_ELOQUENCE_SYNTH_PAUSE_MODE_H

/* Returns a newly-allocated rewritten copy of `src`.  On pcre2 unavailability
 * or any internal failure, returns strdup(src) so the caller can always free()
 * the result without checking. */
char *pause_mode_rewrite(const char *src);

#endif
