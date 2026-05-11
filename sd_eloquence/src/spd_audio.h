/*
 * spd_audio.h -- vendored to provide AudioTrack / AudioFormat when the
 * system speech-dispatcher package doesn't ship spd_audio.h (e.g. Arch).
 * The ABI matches what libspeechd_module.so expects from
 * module_tts_output_server().
 *
 * SPDX-License-Identifier: MIT
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
