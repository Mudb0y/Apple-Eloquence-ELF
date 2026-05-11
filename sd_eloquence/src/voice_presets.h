/*
 * voice_presets.h -- Apple's 8 ECI voice presets, transcribed from
 * KonaVoicePresets.plist inside TextToSpeechKonaSupport.framework.
 *
 * Apple ships the same eight named voices for each of their 14 supported
 * languages. The per-voice parameter values are identical regardless of
 * language (only the human-facing voice name changes -- French uses
 * "Jacques" instead of "Reed" for slot 0). We apply the same parameter
 * sets to each fresh engine handle so the voices sound consistent on
 * Linux versus how Apple ships them on iOS/tvOS/macOS.
 *
 * Apple's plist numbers voices 1..9 with slot 5 unused. We compress the
 * lineup into ECI's 0..7 preset slot range -- the slot number is internal
 * to this module and never exposed to speech-dispatcher.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_ELOQUENCE_VOICE_PRESETS_H
#define SD_ELOQUENCE_VOICE_PRESETS_H

#include <stddef.h>  /* NULL */

/* Parameter values mirror Apple's KonaVoicePresets.plist verbatim:
 *   gender / vocalTract: 0 = male, 1 = female (eciGender)
 *   headSize, pitchBase, pitchFluctuation, roughness, breathiness, volume
 *     all use ECI's 0..100 scale.
 *   speed is intentionally NOT preset here -- it follows the SPD rate
 *     setting and gets applied separately by module_set / spd_to_eci_speed. */
typedef struct {
    const char *name;            /* Default name (en-US lineup) */
    const char *name_fr;         /* French override -- "Jacques" replaces "Reed" */
    int gender;                  /* eciGender:        0=male, 1=female */
    int head_size;               /* eciHeadSize:      0..100 */
    int pitch_baseline;          /* eciPitchBaseline: 0..100 */
    int pitch_fluctuation;       /* eciPitchFluctuation */
    int roughness;               /* eciRoughness */
    int breathiness;             /* eciBreathiness */
    int volume;                  /* eciVolume */
    const char *spd_voice_type;  /* SSIP voice_type tag this voice answers to */
} VoicePreset;

/* Slot 0..7. Apple's "eciVoiceNumber" 1, 2, 3, 4, 6, 7, 8, 9 (skipping 5)
 * is normalised to a contiguous 0..7 here. */
static const VoicePreset g_voice_presets[8] = {
    /* slot 0 */ { "Reed",    "Jacques", 0, 50, 65, 30,  0,  0, 92, "MALE1" },
    /* slot 1 */ { "Shelley", NULL,      1, 50, 81, 30,  0, 50, 95, "FEMALE1" },
    /* slot 2 */ { "Sandy",   NULL,      1, 22, 93, 30,  0, 50, 95, "CHILD_FEMALE" },
    /* slot 3 */ { "Rocko",   NULL,      0, 86, 56, 47,  0,  0, 93, "MALE2" },
    /* slot 4 */ { "Flo",     NULL,      1, 56, 89, 35,  0, 40, 95, "FEMALE2" },
    /* slot 5 */ { "Grandma", NULL,      1, 45, 68, 30,  3, 40, 90, "FEMALE3" },
    /* slot 6 */ { "Grandpa", NULL,      0, 30, 61, 44, 18, 20, 90, "MALE3" },
    /* slot 7 */ { "Eddy",    NULL,      0, 50, 69, 34,  0,  0, 92, NULL /* no SSIP type */ },
};

#define N_VOICE_PRESETS (sizeof(g_voice_presets)/sizeof(g_voice_presets[0]))

#endif /* SD_ELOQUENCE_VOICE_PRESETS_H */
