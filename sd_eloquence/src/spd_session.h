/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * spd_session.h -- accessors for the speech-dispatcher session's
 * current rate / pitch / volume.
 *
 * SSIP SET commands (RATE / PITCH / VOLUME) carry session-wide values
 * that must persist across voice and language changes; speech-
 * dispatcher sends them alongside SET VOICE_TYPE / SET LANGUAGE in an
 * arbitrary order per utterance, so any voice_activate after a SET
 * RATE has to use the same rate or the just-applied value gets wiped.
 *
 * The state lives in module.c; these getters are how worker.c reaches
 * it when handling SSML <voice> push/pop frames.  INT_MIN means the
 * client has not set the parameter yet; voice_activate falls back to
 * the preset default in that case.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_SPD_SESSION_H
#define SD_ELOQUENCE_SPD_SESSION_H

int spd_session_rate(void);
int spd_session_pitch(void);
int spd_session_volume(void);

#endif
