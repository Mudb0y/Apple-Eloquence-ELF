/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * spd_audio.h -- provides AudioTrack / AudioFormat types matching the
 * ABI that libspeechd_module.so's module_tts_output_server() expects.
 * Originally written from observed ABI because the system
 * speech-dispatcher package (e.g. Arch) doesn't ship spd_audio.h.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */

#ifndef SD_ELOQUENCE_SPD_AUDIO_H
#define SD_ELOQUENCE_SPD_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPD_AUDIO_LE = 0,
    SPD_AUDIO_BE = 1,
} AudioFormat;

typedef struct {
    int           bits;          /* 8 or 16 */
    int           num_channels;  /* 1 = mono, 2 = stereo */
    int           sample_rate;   /* Hz */
    int           num_samples;   /* per channel */
    signed short *samples;
} AudioTrack;

#ifdef __cplusplus
}
#endif

#endif /* SD_ELOQUENCE_SPD_AUDIO_H */
