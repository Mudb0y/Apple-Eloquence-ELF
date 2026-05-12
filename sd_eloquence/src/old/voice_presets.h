/*
 * voice_presets.h -- Apple's 8 ECI voice presets transcribed from
 * KonaVoicePresets.plist inside TextToSpeechKonaSupport.framework.
 * Parameter values are language-independent; only the display name
 * differs (French uses "Jacques" for slot 0 instead of "Reed").
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_ELOQUENCE_VOICE_PRESETS_H
#define SD_ELOQUENCE_VOICE_PRESETS_H

#include <stddef.h>

typedef struct {
    const char *name;
    const char *name_fr;         /* French override; NULL = use `name` */
    int gender;                  /* eciGender: 0=male, 1=female */
    int head_size;               /* 0..100 */
    int pitch_baseline;          /* 0..100 */
    int pitch_fluctuation;
    int roughness;
    int breathiness;
    int volume;
    const char *spd_voice_type;  /* SSIP voice_type tag, or NULL */
} VoicePreset;

/* Apple numbers these voices 1, 2, 3, 4, 6, 7, 8, 9 (skipping slot 5);
 * we re-pack into ECI's 0..7. The slot number is internal. */
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
