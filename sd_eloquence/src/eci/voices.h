/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/voices.h -- Apple's 8 ECI voice presets (transcribed from
 * KonaVoicePresets.plist in TextToSpeechKonaSupport.framework).
 *
 * Apple's eci.dylib synthesizes from voice slot 0 only; slots 1..7 hold
 * inert preset data unless an explicit eciCopyVoice promotes them. We
 * always write the chosen preset's params into slot 0.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_ECI_VOICES_H
#define SD_ELOQUENCE_ECI_VOICES_H

#include "runtime.h"
#include "../config.h"

#define ECI_ACTIVE_SLOT 0

typedef struct {
    const char *name;
    const char *name_fr;          /* French override; NULL = use `name` */
    int gender;                   /* eciGender: 0=male, 1=female */
    int head_size;                /* 0..100 */
    int pitch_baseline;           /* 0..100 */
    int pitch_fluctuation;
    int roughness;
    int breathiness;
    int volume;                   /* 0..100 */
    const char *spd_voice_type;   /* SSIP voice_type tag, or NULL */
} VoicePreset;

#define N_VOICE_PRESETS 8
extern const VoicePreset g_voice_presets[N_VOICE_PRESETS];

/* Return preset name appropriate for the active language; uses name_fr for
 * French dialects, name otherwise. */
const char *voice_display_name(int slot, const char *iso_lang);

/* Activate a preset into slot 0 of the engine.
 *
 * spd_rate / spd_pitch / spd_volume are speech-dispatcher overrides in the
 * range -100..+100, or INT_MIN to use the preset's defaults. Speed defaults
 * to Apple's plist value (50).
 *
 * If cfg is non-NULL and any of its voice_* override fields are not
 * ELOQ_VOICE_PARAM_UNSET, the corresponding ECI voice param is rewritten
 * after the preset is loaded. cfg may be NULL when the caller doesn't have
 * access to the global config (e.g. from a frame replay path). */
void voice_activate(const EciApi *eci, ECIHand h, int slot,
                    int spd_rate, int spd_pitch, int spd_volume,
                    const EloqConfig *cfg);

/* Find a preset by case-insensitive name (English or French). Returns the
 * slot index, or -1 if not found. */
int voice_find_by_name(const char *name);

/* Find a preset slot by speechd voice_type ("MALE1", "FEMALE3", etc.).
 * CHILD_MALE and CHILD_FEMALE both map to Sandy (slot 2). Returns the slot,
 * or -1 if not matched. */
int voice_find_by_voice_type(const char *voice_type);

#endif
