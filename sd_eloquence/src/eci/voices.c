/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/voices.c -- Apple voice preset table + activation helpers.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "voices.h"

#include <limits.h>
#include <strings.h>
#include <string.h>

const VoicePreset g_voice_presets[N_VOICE_PRESETS] = {
    /* slot 0 */ { "Reed",    "Jacques", 0, 50, 65, 30,  0,  0, 92, "MALE1" },
    /* slot 1 */ { "Shelley", NULL,      1, 50, 81, 30,  0, 50, 95, "FEMALE1" },
    /* slot 2 */ { "Sandy",   NULL,      1, 22, 93, 30,  0, 50, 95, "CHILD_FEMALE" },
    /* slot 3 */ { "Rocko",   NULL,      0, 86, 56, 47,  0,  0, 93, "MALE2" },
    /* slot 4 */ { "Flo",     NULL,      1, 56, 89, 35,  0, 40, 95, "FEMALE2" },
    /* slot 5 */ { "Grandma", NULL,      1, 45, 68, 30,  3, 40, 90, "FEMALE3" },
    /* slot 6 */ { "Grandpa", NULL,      0, 30, 61, 44, 18, 20, 90, "MALE3" },
    /* slot 7 */ { "Eddy",    NULL,      0, 50, 69, 34,  0,  0, 92, NULL },
};

const char *voice_display_name(int slot, const char *iso_lang) {
    if (slot < 0 || slot >= N_VOICE_PRESETS) return NULL;
    const VoicePreset *v = &g_voice_presets[slot];
    if (v->name_fr && iso_lang && strcmp(iso_lang, "fr") == 0)
        return v->name_fr;
    return v->name;
}

static int spd_to_eci_pct(int v) {
    int o = (v + 100) / 2;
    if (o < 0)   o = 0;
    if (o > 100) o = 100;
    return o;
}

static int spd_to_eci_speed(int v) {
    int o = v + 100;
    if (o < 0)   o = 0;
    if (o > 200) o = 200;
    return o;
}

void voice_activate(const EciApi *eci, ECIHand h, int slot,
                    int spd_rate, int spd_pitch, int spd_volume) {
    if (slot < 0 || slot >= N_VOICE_PRESETS) return;
    const VoicePreset *v = &g_voice_presets[slot];

    int speed_val = (spd_rate   != INT_MIN) ? spd_to_eci_speed(spd_rate) : 50;
    int pitch_val = (spd_pitch  != INT_MIN) ? spd_to_eci_pct(spd_pitch)
                                            : v->pitch_baseline;
    int vol_val   = (spd_volume != INT_MIN) ? spd_to_eci_pct(spd_volume)
                                            : v->volume;

    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciGender,           v->gender);
    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciHeadSize,         v->head_size);
    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciPitchFluctuation, v->pitch_fluctuation);
    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciPitchBaseline,    pitch_val);
    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciRoughness,        v->roughness);
    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciBreathiness,      v->breathiness);
    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciSpeed,            speed_val);
    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciVolume,           vol_val);
}

int voice_find_by_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < N_VOICE_PRESETS; i++) {
        const VoicePreset *p = &g_voice_presets[i];
        if (!strcasecmp(name, p->name)) return i;
        if (p->name_fr && !strcasecmp(name, p->name_fr)) return i;
    }
    return -1;
}

int voice_find_by_voice_type(const char *t) {
    if (!t) return -1;
    if (!strcasecmp(t, "CHILD_MALE") || !strcasecmp(t, "CHILD_FEMALE"))
        return 2;  /* Sandy */
    for (int i = 0; i < N_VOICE_PRESETS; i++) {
        const char *vt = g_voice_presets[i].spd_voice_type;
        if (vt && !strcasecmp(t, vt)) return i;
    }
    return -1;
}
