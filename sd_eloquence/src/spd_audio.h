/*
 * spd_audio.h -- minimal definitions matching speech-dispatcher's internal
 * spd_audio.h ABI.
 *
 * speech-dispatcher includes its own spd_audio.h via <spd_audio.h> from
 * <speech-dispatcher/spd_module_main.h>, but the Arch package doesn't ship
 * that header in /usr/include. Other speech-dispatcher module projects
 * (viavoice-spd, evvrelink, macintalk) all vendor a minimal compatible
 * declaration; we do the same here.
 *
 * The ABI definitions match what libspeechd_module.so expects when modules
 * pass AudioTrack to module_tts_output_server().
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
