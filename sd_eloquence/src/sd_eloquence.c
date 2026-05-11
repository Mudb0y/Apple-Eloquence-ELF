/*
 * sd_eloquence.c -- native Speech Dispatcher module for ETI Eloquence.
 *
 * Wraps the ECI 6.x C API and streams PCM to speech-dispatcher's audio
 * server. Compatible with any libc-only ECI build, including:
 *  - The Apple TextToSpeechKonaSupport.framework dylibs (via macho2elf)
 *  - Speechworks / IBMTTS Linux distributions
 *  - LevelStar Icon's bundled distribution
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 * SPDX-License-Identifier: MIT
 *
 * Speech-dispatcher's libspeechd_module.so provides main(), the protocol
 * loop, and audio plumbing. We only implement the TTS-specific callbacks
 * declared in <speech-dispatcher/spd_module_main.h>.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* The system <speech-dispatcher/spd_module_main.h> internally does
 * `#include <spd_audio.h>` without the speech-dispatcher/ prefix and the
 * Arch package doesn't ship that header, so we provide our own minimal
 * compatible copy in src/spd_audio.h. The build system puts src/ on the
 * include search path so the bare include resolves correctly. */
#include "spd_audio.h"
#include <speech-dispatcher/spd_module_main.h>

#ifdef HAVE_SOXR
#include <soxr.h>
#endif

#include "eci.h"
#include "voice_presets.h"

#define DBG(fmt, ...) \
    do { if (g_debug) fprintf(stderr, "sd_eloquence: " fmt "\n", ##__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ */
/* Configuration loaded from eloquence.conf                           */
/* ------------------------------------------------------------------ */
static int   g_debug = 0;
/* Directory holding eci.so, the language modules, and the multi-section
 * eci.ini that ties them together. The module chdirs here at init so the
 * engine's static constructor finds eci.ini in cwd, then dlopens eci.so
 * from the same place. */
static char  g_data_dir[512]      = "/usr/lib/eloquence";
static int   g_default_sample_rate = 1;  /* 0=8k, 1=11k, 2=22k */
static int   g_default_voice_slot  = 0;  /* 0..7, index into g_voice_presets */
static int   g_default_language    = eciGeneralAmericanEnglish;

/* Resampling: if g_resample_rate > 0, feed engine PCM through libsoxr to
 * upsample to that rate before sending to the audio server. */
static int   g_resample_rate    = 0;   /* 0 = pass-through */
static int   g_resample_quality = 4;   /* 0=quick 1=low 2=med 3=high 4=very-high */
static int   g_resample_phase   = 0;   /* 0=intermediate 1=linear 2=minimum */
static int   g_resample_steep   = 0;   /* 1 = steep cutoff filter */

/* ------------------------------------------------------------------ */
/* Runtime state                                                      */
/* ------------------------------------------------------------------ */
static EciApi   eci;
static ECIHand  h_engine = NULL;
static int      h_engine_sample_rate_hz = 11025;  /* derived from current eciSampleRate */
static int      g_current_language      = eciGeneralAmericanEnglish;
static int      g_current_voice_slot    = 0;     /* 0..7 */
static volatile int g_stop_requested    = 0;

/* SPD-side cached parameter values per voice slot. Apply on engine create
 * AND on slot change so each voice picks up the user's rate/pitch/volume
 * choices when selected. -1 means "use the slot's preset default". */
static int g_spd_rate[N_VOICE_PRESETS];
static int g_spd_pitch[N_VOICE_PRESETS];
static int g_spd_volume[N_VOICE_PRESETS];

/* Per-language availability flag. We deliberately do NOT probe at init by
 * looping eciNewEx + eciDelete: that pattern triggers Apple's C++ static-
 * destructor ordering bug (also documented in examples/speak.c and
 * eci_runtime.c -- dlclose during running destructors crashes inside
 * libc atexit). Instead we set this flag from a static list: jpn / kor /
 * chs / cht fail eciNewEx with retval=-21 on Apple's converted modules
 * (they need data files our extraction doesn't carry); everything else
 * works. Marking them unavailable here keeps the voice list honest and
 * keeps the engine off the broken language modules entirely. */
static int g_lang_available[14];  /* parallel to g_langs[] */

/* PCM buffer the engine fills via eciSetOutputBuffer + our callback. */
#define PCM_CHUNK_SAMPLES   8192        /* per-chunk request, in samples */
static int16_t  g_pcm_chunk[PCM_CHUNK_SAMPLES];

#ifdef HAVE_SOXR
static soxr_t  g_soxr = NULL;
/* Output buffer for resampled PCM. Real-world max ratio is 11025 -> 48000
 * = ~4.35x; an 8x buffer covers that plus the headroom soxr's polyphase
 * filter sometimes wants on internal flush boundaries. Without the
 * headroom soxr would write past the end of g_resampled and segfault
 * mid-utterance the first time it hit a chunk that ran the filter long. */
#define RESAMPLED_MAX_SAMPLES (PCM_CHUNK_SAMPLES * 8)
static int16_t  g_resampled[RESAMPLED_MAX_SAMPLES];
#endif

/* Full 14-language table. Each entry binds together the ECI dialect code
 * passed to eciNewEx / SetParam, the eci.ini section name [major.minor]
 * used to locate the .so on disk, the .so filename (under g_data_dir),
 * and the ISO 639-1 / region tags surfaced to speech-dispatcher via
 * SPDVoice.language / SPDVoice.variant. */
typedef struct {
    int   eci_dialect;
    int   ini_major;        /* eci.ini section name -- upper half of dialect code */
    int   ini_minor;        /* lower half */
    const char *so_name;    /* "enu.so" */
    const char *iso_lang;   /* "en" */
    const char *iso_variant;/* "us" */
    const char *human;      /* "American English" */
} LangEntry;

static const LangEntry g_langs[] = {
    { eciGeneralAmericanEnglish, 1,  0, "enu.so", "en", "us", "American English" },
    { eciBritishEnglish,         1,  1, "eng.so", "en", "gb", "British English" },
    { eciCastilianSpanish,       2,  0, "esp.so", "es", "es", "Castilian Spanish" },
    { eciMexicanSpanish,         2,  1, "esm.so", "es", "mx", "Latin American Spanish" },
    { eciStandardFrench,         3,  0, "fra.so", "fr", "fr", "French" },
    { eciCanadianFrench,         3,  1, "frc.so", "fr", "ca", "Canadian French" },
    { eciStandardGerman,         4,  0, "deu.so", "de", "de", "German" },
    { eciStandardItalian,        5,  0, "ita.so", "it", "it", "Italian" },
    { eciMandarinChinese,        6,  0, "chs.so", "zh", "cn", "Mandarin Chinese (Simplified)" },
    { eciTaiwaneseMandarin,      6,  1, "cht.so", "zh", "tw", "Mandarin Chinese (Traditional)" },
    { eciBrazilianPortuguese,    7,  0, "ptb.so", "pt", "br", "Brazilian Portuguese" },
    { eciStandardJapanese,       8,  0, "jpn.so", "ja", "jp", "Japanese" },
    { eciStandardFinnish,        9,  0, "fin.so", "fi", "fi", "Finnish" },
    { eciStandardKorean,        10,  0, "kor.so", "ko", "kr", "Korean" },
};
#define N_LANGS (sizeof(g_langs)/sizeof(g_langs[0]))

static const LangEntry *find_lang_by_dialect(int dialect) {
    for (size_t i = 0; i < N_LANGS; i++)
        if (g_langs[i].eci_dialect == dialect) return &g_langs[i];
    return NULL;
}

static const LangEntry *find_lang_by_iso(const char *tag) {
    /* Accept either an ISO 639-1 prefix ("en", "de") or a full IETF tag
     * ("en-US", "de-DE"). The full tag is preferred -- it picks the
     * exact dialect -- so try that match first. Fall back to the
     * language-only match when no region is given. */
    for (size_t i = 0; i < N_LANGS; i++) {
        char ietf[16];
        snprintf(ietf, sizeof(ietf), "%s-%s",
                 g_langs[i].iso_lang, g_langs[i].iso_variant);
        if (strcasecmp(tag, ietf) == 0) return &g_langs[i];
        /* Some callers may pass lowercase region: "en-us". snprintf
         * above produced lowercase region already, so the strcasecmp
         * caught that too. */
    }
    /* Fall back to first match on the language portion only. */
    for (size_t i = 0; i < N_LANGS; i++) {
        size_t lang_len = strlen(g_langs[i].iso_lang);
        if (strncasecmp(tag, g_langs[i].iso_lang, lang_len) == 0 &&
            (tag[lang_len] == 0 || tag[lang_len] == '-' ||
             tag[lang_len] == '_'))
            return &g_langs[i];
    }
    return NULL;
}

/* Return the display name for slot in language `lang`. The French entry
 * uses "Jacques" instead of "Reed" for slot 0 to mirror Apple's plist. */
static const char *voice_display_name(int slot, const LangEntry *lang) {
    const VoicePreset *v = &g_voice_presets[slot];
    if (v->name_fr && lang && strcmp(lang->iso_lang, "fr") == 0)
        return v->name_fr;
    return v->name;
}

/* ------------------------------------------------------------------ */
/* Resampler init / teardown                                          */
/* ------------------------------------------------------------------ */
#ifdef HAVE_SOXR
static int resampler_setup(int input_hz) {
    /* Reset any previous resampler */
    if (g_soxr) {
        soxr_delete(g_soxr);
        g_soxr = NULL;
    }
    if (g_resample_rate <= 0 || g_resample_rate == input_hz) {
        DBG("resampler: pass-through (input=%d Hz)", input_hz);
        return 0;
    }

    /* Map quality (0..4) -> soxr quality recipe */
    static const unsigned long quality_table[] = {
        SOXR_QQ, SOXR_LQ, SOXR_MQ, SOXR_HQ, SOXR_VHQ
    };
    int qi = g_resample_quality;
    if (qi < 0 || qi >= (int)(sizeof(quality_table)/sizeof(quality_table[0])))
        qi = 4;  /* default to VHQ */
    unsigned long recipe = quality_table[qi];

    /* Phase flags. soxr's default phase (no flag) is symmetric linear-phase;
     * SOXR_INTERMEDIATE_PHASE is what sox's `rate -v` (no -L/-M/-I) emits. */
    switch (g_resample_phase) {
        case 0: recipe |= SOXR_INTERMEDIATE_PHASE; break;
        case 1: recipe |= SOXR_LINEAR_PHASE;       break;
        case 2: recipe |= SOXR_MINIMUM_PHASE;      break;
    }
    if (g_resample_steep) recipe |= SOXR_STEEP_FILTER;

    soxr_quality_spec_t qspec = soxr_quality_spec(recipe, 0);
    soxr_io_spec_t      ispec = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);

    soxr_error_t err = NULL;
    g_soxr = soxr_create(input_hz, g_resample_rate, 1,
                          &err, &ispec, &qspec, NULL);
    if (err) {
        fprintf(stderr, "sd_eloquence: soxr_create failed: %s\n", soxr_strerror(err));
        g_soxr = NULL;
        return -1;
    }
    DBG("resampler: %d Hz -> %d Hz (recipe=%#lx)", input_hz, g_resample_rate, recipe);
    return 0;
}

static void resampler_teardown(void) {
    if (g_soxr) {
        soxr_delete(g_soxr);
        g_soxr = NULL;
    }
}
#else
static int  resampler_setup(int input_hz) { (void)input_hz; return 0; }
static void resampler_teardown(void) {}
#endif

/* ------------------------------------------------------------------ */
/* Callback: ECI -> our PCM forwarder (+ optional resampling)         */
/* ------------------------------------------------------------------ */
static enum ECICallbackReturn eci_cb(ECIHand h, enum ECIMessage msg,
                                     long lParam, void *pData) {
    (void)h; (void)pData;

    if (g_stop_requested)
        return eciDataAbort;

    if (msg != eciWaveformBuffer)
        return eciDataProcessed;

    if (lParam <= 0)
        return eciDataProcessed;

    /* Clamp to the buffer size we handed the engine. If the engine ever
     * returns a sample count larger than PCM_CHUNK_SAMPLES (observed on
     * some builds when end-of-utterance carries a residual), we'd read
     * past the end of g_pcm_chunk -- which on the resampler path means
     * libsoxr reads garbage from beyond our buffer and segfaults deep
     * inside its polyphase loop. */
    if (lParam > PCM_CHUNK_SAMPLES) {
        DBG("eci_cb: clamping lParam=%ld to buffer size %d",
            lParam, PCM_CHUNK_SAMPLES);
        lParam = PCM_CHUNK_SAMPLES;
    }

    AudioTrack track;
    track.bits         = 16;
    track.num_channels = 1;

#ifdef HAVE_SOXR
    if (g_soxr) {
        size_t in_done = 0, out_done = 0;
        soxr_error_t err = soxr_process(g_soxr,
            g_pcm_chunk, (size_t)lParam, &in_done,
            g_resampled, RESAMPLED_MAX_SAMPLES, &out_done);
        if (err) {
            fprintf(stderr, "sd_eloquence: soxr_process: %s\n", soxr_strerror(err));
            return eciDataAbort;
        }
        if (out_done > 0) {
            track.num_samples = (int)out_done;
            track.sample_rate = g_resample_rate;
            track.samples     = g_resampled;
            module_tts_output_server(&track, SPD_AUDIO_LE);
        }
        return eciDataProcessed;
    }
#endif

    /* Pass-through path: send engine PCM as-is. */
    track.num_samples = (int)lParam;
    track.sample_rate = h_engine_sample_rate_hz;
    track.samples     = g_pcm_chunk;
    module_tts_output_server(&track, SPD_AUDIO_LE);
    return eciDataProcessed;
}

#ifdef HAVE_SOXR
/* Flush soxr's internal buffer at end of synthesis to capture trailing samples. */
static void resampler_flush(void) {
    if (!g_soxr) return;
    size_t out_done = 0;
    soxr_error_t err = soxr_process(g_soxr,
        NULL, 0, NULL,
        g_resampled, RESAMPLED_MAX_SAMPLES, &out_done);
    if (err || out_done == 0) return;
    AudioTrack track = {
        .bits = 16, .num_channels = 1,
        .num_samples = (int)out_done,
        .sample_rate = g_resample_rate,
        .samples     = g_resampled,
    };
    module_tts_output_server(&track, SPD_AUDIO_LE);
}
#else
static void resampler_flush(void) {}
#endif

/* ------------------------------------------------------------------ */
/* Config file parsing                                                */
/* ------------------------------------------------------------------ */
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    /* Strip surrounding quotes */
    if (*s == '"' && e > s+1 && e[-1] == '"') { *--e = 0; s++; }
    return s;
}

int module_config(const char *configfile) {
    if (!configfile) {
        DBG("no config file path passed; using built-in defaults");
        return 0;
    }
    FILE *f = fopen(configfile, "r");
    if (!f) {
        fprintf(stderr, "sd_eloquence: cannot open %s: %s\n",
                configfile, strerror(errno));
        /* Not fatal -- we have defaults */
        return 0;
    }

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (*p == 0 || *p == '#') continue;

        /* Split on first whitespace */
        char *sp = p;
        while (*sp && !isspace((unsigned char)*sp)) sp++;
        if (*sp == 0) continue;
        *sp++ = 0;
        char *val = trim(sp);

        if (strcasecmp(p, "Debug") == 0) {
            g_debug = atoi(val);
        } else if (strcasecmp(p, "EloquenceDataDir") == 0) {
            strncpy(g_data_dir, val, sizeof(g_data_dir)-1);
            g_data_dir[sizeof(g_data_dir)-1] = 0;
        } else if (strcasecmp(p, "EciLibrary") == 0 ||
                   strcasecmp(p, "EciVoicePath") == 0) {
            /* Legacy keys from earlier eloquence.conf revisions. The new
             * model uses EloquenceDataDir and an install-generated eci.ini
             * that knows about every language module, so these no longer
             * make sense. Silently accept and ignore so older configs
             * keep loading without errors. */
            DBG("ignoring legacy config key '%s' (now derived from EloquenceDataDir)", p);
        } else if (strcasecmp(p, "EloquenceSampleRate") == 0) {
            g_default_sample_rate = atoi(val);
            if (g_default_sample_rate < 0 || g_default_sample_rate > 2)
                g_default_sample_rate = 1;
        } else if (strcasecmp(p, "EloquenceDefaultVoice") == 0) {
            /* Accept either a 0..7 slot index or one of the Apple voice
             * names (Reed, Shelley, Sandy, Rocko, Flo, Grandma, Grandpa,
             * Eddy; "Jacques" maps to slot 0 like Reed). */
            char *end = NULL;
            long n = strtol(val, &end, 10);
            if (end && *end == 0 && n >= 0 && n < (long)N_VOICE_PRESETS) {
                g_default_voice_slot = (int)n;
            } else {
                int matched = -1;
                for (int i = 0; i < (int)N_VOICE_PRESETS; i++) {
                    if (!strcasecmp(val, g_voice_presets[i].name) ||
                        (g_voice_presets[i].name_fr &&
                         !strcasecmp(val, g_voice_presets[i].name_fr))) {
                        matched = i; break;
                    }
                }
                if (matched >= 0) g_default_voice_slot = matched;
                else DBG("unknown EloquenceDefaultVoice '%s', keeping slot %d",
                         val, g_default_voice_slot);
            }
        } else if (strcasecmp(p, "EloquenceDefaultLanguage") == 0) {
            /* Accept decimal, 0x... hex, OR an ISO-639-1 / IETF tag like
             * "en-US" / "de" / "ja-JP". */
            const LangEntry *L = NULL;
            char *end = NULL;
            long n = strtol(val, &end, 0);
            if (end && *end == 0) {
                L = find_lang_by_dialect((int)n);
                if (L) g_default_language = (int)n;
                else DBG("unknown EloquenceDefaultLanguage 0x%lx", n);
            } else {
                L = find_lang_by_iso(val);
                if (L) g_default_language = L->eci_dialect;
                else DBG("unknown EloquenceDefaultLanguage '%s'", val);
            }
        } else if (strcasecmp(p, "EloquenceResampleRate") == 0) {
            g_resample_rate = atoi(val);
        } else if (strcasecmp(p, "EloquenceResampleQuality") == 0) {
            if      (!strcasecmp(val, "quick"))      g_resample_quality = 0;
            else if (!strcasecmp(val, "low"))        g_resample_quality = 1;
            else if (!strcasecmp(val, "medium"))     g_resample_quality = 2;
            else if (!strcasecmp(val, "high"))       g_resample_quality = 3;
            else if (!strcasecmp(val, "very-high") ||
                     !strcasecmp(val, "vhq"))        g_resample_quality = 4;
            else { DBG("unknown ResampleQuality '%s', defaulting to very-high", val);
                   g_resample_quality = 4; }
        } else if (strcasecmp(p, "EloquenceResamplePhase") == 0) {
            if      (!strcasecmp(val, "intermediate")) g_resample_phase = 0;
            else if (!strcasecmp(val, "linear"))       g_resample_phase = 1;
            else if (!strcasecmp(val, "minimum"))      g_resample_phase = 2;
            else { DBG("unknown ResamplePhase '%s', defaulting to intermediate", val);
                   g_resample_phase = 0; }
        } else if (strcasecmp(p, "EloquenceResampleSteep") == 0) {
            g_resample_steep = atoi(val) ? 1 : 0;
        } else {
            DBG("unknown config key '%s' (ignored)", p);
        }
    }
    fclose(f);
    DBG("config: debug=%d data_dir=%s rate=%d voice=%d (%s) lang=%#x",
        g_debug, g_data_dir, g_default_sample_rate, g_default_voice_slot,
        g_voice_presets[g_default_voice_slot].name, g_default_language);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Engine init / lifecycle                                            */
/* ------------------------------------------------------------------ */

/* Engine slot 0 is the "active" voice on Apple's eci.dylib -- it's the
 * slot used for synthesis. Setting per-voice parameters on slots 1..7
 * configures stored presets but doesn't affect what the engine speaks
 * with. To change voice we have to push the chosen preset's parameter
 * values into slot 0; eciCopyVoice would do this too but writing the
 * params directly is just as cheap and avoids depending on a feature
 * Apple's build may handle differently. spd_to_eci_pct /
 * spd_to_eci_speed are forward-declared because they're defined below
 * but used here. */
static int spd_to_eci_pct(int spd_val);
static int spd_to_eci_speed(int spd_val);

#define ECI_ACTIVE_SLOT 0

/* Switch the engine's active voice (slot 0) to a different preset.
 *
 * Writes ALL eight per-voice parameters explicitly, every time, even
 * the ones the new preset shares with the old one. Otherwise the
 * leftover values from the previous voice "leak" into the new one --
 * e.g. switching from Reed (pitchBase=65) to Sandy (pitchBase=93)
 * with only the differing fields set would let Reed's pitch survive
 * if the engine fast-paths SetVoiceParam to a no-op when the new
 * value equals the cached one, or if some param re-derives others
 * after a later write.
 *
 * Order matters less than completeness, but we go gender -> head ->
 * pitch_fluct -> pitch_base -> roughness -> breath -> speed -> volume
 * so the dominant "voice shape" params (gender, head, pitch) land
 * before the modulation params (roughness, breath).
 *
 * eciSpeed: the preset itself doesn't ship a speed (Apple's plist
 * pins every preset at speed=50), so we use Apple's 50 unless the
 * user has dialed a per-voice SPD rate override for this slot.
 *
 * SPD per-voice overrides (rate/pitch/volume) get layered on at
 * the end so they win over the preset defaults. */
static void activate_voice_preset(ECIHand h, int slot) {
    if (slot < 0 || slot >= (int)N_VOICE_PRESETS) return;
    const VoicePreset *v = &g_voice_presets[slot];

    int speed_val = (g_spd_rate[slot]   != INT_MIN)
                    ? spd_to_eci_speed(g_spd_rate[slot])
                    : 50;
    int pitch_val = (g_spd_pitch[slot]  != INT_MIN)
                    ? spd_to_eci_pct(g_spd_pitch[slot])
                    : v->pitch_baseline;
    int vol_val   = (g_spd_volume[slot] != INT_MIN)
                    ? spd_to_eci_pct(g_spd_volume[slot])
                    : v->volume;

    DBG("activate %s: gender=%d head=%d pitch=%d/%d rough=%d breath=%d speed=%d vol=%d",
        v->name, v->gender, v->head_size,
        pitch_val, v->pitch_fluctuation,
        v->roughness, v->breathiness, speed_val, vol_val);

    eci.SetVoiceParam(h, ECI_ACTIVE_SLOT, eciGender,           v->gender);
    eci.SetVoiceParam(h, ECI_ACTIVE_SLOT, eciHeadSize,         v->head_size);
    eci.SetVoiceParam(h, ECI_ACTIVE_SLOT, eciPitchFluctuation, v->pitch_fluctuation);
    eci.SetVoiceParam(h, ECI_ACTIVE_SLOT, eciPitchBaseline,    pitch_val);
    eci.SetVoiceParam(h, ECI_ACTIVE_SLOT, eciRoughness,        v->roughness);
    eci.SetVoiceParam(h, ECI_ACTIVE_SLOT, eciBreathiness,      v->breathiness);
    eci.SetVoiceParam(h, ECI_ACTIVE_SLOT, eciSpeed,            speed_val);
    eci.SetVoiceParam(h, ECI_ACTIVE_SLOT, eciVolume,           vol_val);

    /* Read back what the engine actually accepted so we can spot any
     * silent rejection. The numbers should match what we just set; if
     * they don't, the engine refused that param and we need to debug
     * the interaction. */
    DBG("readback   : gender=%d head=%d pitch=%d/%d rough=%d breath=%d speed=%d vol=%d",
        eci.GetVoiceParam(h, ECI_ACTIVE_SLOT, eciGender),
        eci.GetVoiceParam(h, ECI_ACTIVE_SLOT, eciHeadSize),
        eci.GetVoiceParam(h, ECI_ACTIVE_SLOT, eciPitchBaseline),
        eci.GetVoiceParam(h, ECI_ACTIVE_SLOT, eciPitchFluctuation),
        eci.GetVoiceParam(h, ECI_ACTIVE_SLOT, eciRoughness),
        eci.GetVoiceParam(h, ECI_ACTIVE_SLOT, eciBreathiness),
        eci.GetVoiceParam(h, ECI_ACTIVE_SLOT, eciSpeed),
        eci.GetVoiceParam(h, ECI_ACTIVE_SLOT, eciVolume));
}

/* Apply our full cached state to a freshly-created engine handle:
 * sample rate, the currently selected voice preset, and the standard
 * synthesis callback/output buffer wiring. */
static void apply_engine_state(ECIHand h) {
    if (eci.SetParam(h, eciSampleRate, g_default_sample_rate) < 0) {
        DBG("apply: setParam(eciSampleRate=%d) rejected, falling back to 11025 Hz",
            g_default_sample_rate);
        g_default_sample_rate = 1;
        eci.SetParam(h, eciSampleRate, g_default_sample_rate);
    }
    activate_voice_preset(h, g_current_voice_slot);
    eci.RegisterCallback(h, eci_cb, NULL);
    eci.SetOutputBuffer(h, PCM_CHUNK_SAMPLES, g_pcm_chunk);
}

/* Switch to a different language by destroying the current engine and
 * creating a new one with eciNewEx. The order MUST be Stop -> Synchronize
 * -> Delete -> NewEx (serial, one engine alive at a time):
 *
 *   - Holding two ECI engines alive concurrently corrupts internal state
 *     (observed: SIGSEGV in enu.so during synthesis when a second engine
 *     was created without first deleting the previous one).
 *   - eciDelete returns immediately on Apple's build but the engine's
 *     worker thread can still be mid-synthesis. Calling Synchronize
 *     before Delete forces the engine to drain pending audio first, so
 *     no worker thread is left dereferencing freed engine state.
 *
 * Returns 0 on success, -1 on failure. On failure h_engine is NULL and
 * the caller should fall back (e.g. recreate the previous language).
 */
/* Switch the engine to a new language. The first time we're called we
 * have to use eciNewEx (no engine exists yet). After that we MUST use
 * eciSetParam(eciLanguageDialect, ...) -- not eciDelete + eciNewEx --
 * because Apple's eci.dylib runs a singleton engine whose internal
 * language modules don't reload cleanly after eciDelete. Specifically,
 * the second eciNewEx that re-loads a language .so (e.g. chs.so on
 * user toggling zh-CN twice in Orca) hits a stale-state crash inside
 * the language module's reset path -- coredump shows
 * chs.so reset_sent_vars -> _fence with PC at the function entry,
 * symptomatic of stack/state corruption from the previous engine's
 * teardown.
 *
 * eciSetParam doesn't tear down the engine. Empirically some
 * cross-language transitions return -1 (rejection), but synthesis
 * still produces audio in the requested language afterwards; even
 * when SetParam reports failure the engine seems to internally
 * load the new dialect's tables. We just trust that path. */
static int rebuild_engine_for_language(int dialect) {
    if (!h_engine) {
        /* First-ever engine create. eciNewEx is the only way to set
         * the initial language; eciSetParam would no-op against a
         * non-existent engine. */
        h_engine = eci.NewEx(dialect);
        if (!h_engine) {
            const LangEntry *lang = find_lang_by_dialect(dialect);
            fprintf(stderr, "sd_eloquence: eciNewEx(%#x %s) failed\n",
                    dialect, lang ? lang->human : "?");
            return -1;
        }
        g_current_language = dialect;
        apply_engine_state(h_engine);
        DBG("engine created for language %#x (%s)",
            dialect, find_lang_by_dialect(dialect)
                     ? find_lang_by_dialect(dialect)->human : "?");
        return 0;
    }

    /* Subsequent switches: SetParam-only, no recreate. Voice presets
     * persist across SetParam (they live in the engine's per-slot
     * voice tables, which the language switch doesn't reset on Apple's
     * build), so we don't re-apply them here -- doing so was poking
     * chs.so's internal state during the switch and tripping a
     * SIGSEGV inside reset_sent_vars on the next synthesis. */
    eci.Stop(h_engine);
    eci.Synchronize(h_engine);
    int prev = eci.SetParam(h_engine, eciLanguageDialect, dialect);
    g_current_language = dialect;
    DBG("language switch to %#x (%s) via SetParam, prev=%#x",
        dialect, find_lang_by_dialect(dialect)
                 ? find_lang_by_dialect(dialect)->human : "?", prev);
    return 0;
}

/* Populate g_lang_available[].
 *
 * CJK is currently disabled across the board (jpn / kor / chs / cht).
 * macho2elf's GOT-rebase fix made the language modules LOAD, and the
 * eciSetParam-based language switching kept the engine alive across
 * transitions, but synthesis remains unreliable:
 *
 *   - chs.so reproducibly SIGSEGVs in reset_sent_vars -> _fence on
 *     the first eciAddText after a switch to zh-CN, even though the
 *     structurally identical cht.so survives the same path.
 *   - jpn.so / ja-JP synthesizes audio for some inputs but the
 *     romanizer (jpnrom.so) appears to read uninitialised tables
 *     under speechd-style threading; reproduces as intermittent
 *     SIGSEGV in DictSearch::lookupFuncDict under Orca usage.
 *   - kor.so + cht.so come along for the ride: shipping a partial
 *     CJK set (one out of four) was confusing for users and would
 *     keep eating support attention, so gate the whole family until
 *     we can debug the RomanizerManager / language-module-init
 *     boundary properly.
 *
 * The 10 non-CJK languages (en-US, en-GB, es-ES, es-MX, fr-FR, fr-CA,
 * de-DE, it-IT, pt-BR, fi-FI) are stable.
 *
 * We deliberately do NOT probe at init by looping eciNewEx + eciDelete:
 * that pattern triggers Apple's C++ static-destructor ordering bug
 * already documented in eci_runtime.c and examples/speak.c. */
static void mark_available_languages(void) {
    for (size_t li = 0; li < N_LANGS; li++) {
        const char *so = g_langs[li].so_name;
        int cjk = (strcmp(so, "jpn.so") == 0 ||
                   strcmp(so, "kor.so") == 0 ||
                   strcmp(so, "chs.so") == 0 ||
                   strcmp(so, "cht.so") == 0);
        g_lang_available[li] = !cjk;
        DBG("%-30s %s", g_langs[li].human,
            g_lang_available[li]
                ? "available"
                : "unavailable (CJK temporarily disabled)");
    }
}

/* chdir() into g_data_dir before dlopen so the engine's static
 * constructor finds eci.ini there. Returns -1 if the directory or
 * eci.so / eci.ini are missing. */
static int enter_data_dir(char **errmsg) {
    char eci_so[1024], eci_ini[1024];
    snprintf(eci_so,  sizeof(eci_so),  "%s/eci.so",  g_data_dir);
    snprintf(eci_ini, sizeof(eci_ini), "%s/eci.ini", g_data_dir);

    if (access(eci_so, R_OK) != 0) {
        if (errmsg) {
            char buf[2048];
            snprintf(buf, sizeof(buf),
                "ECI runtime missing: %s not found (set EloquenceDataDir "
                "in eloquence.conf to the directory holding eci.so and "
                "the language modules)", eci_so);
            *errmsg = strdup(buf);
        }
        return -1;
    }
    if (access(eci_ini, R_OK) != 0) {
        if (errmsg) {
            char buf[2048];
            snprintf(buf, sizeof(buf),
                "eci.ini missing: %s not found (the install step should "
                "have generated it; re-run `cmake --install build`)",
                eci_ini);
            *errmsg = strdup(buf);
        }
        return -1;
    }
    if (chdir(g_data_dir) != 0) {
        if (errmsg) {
            char buf[2048];
            snprintf(buf, sizeof(buf),
                "chdir(%s) failed: %s", g_data_dir, strerror(errno));
            *errmsg = strdup(buf);
        }
        return -1;
    }
    DBG("chdir -> %s", g_data_dir);
    return 0;
}

int module_init(char **msg) {
    /* Initialise the SPD-side cached parameters to "use preset default". */
    for (size_t i = 0; i < N_VOICE_PRESETS; i++) {
        g_spd_rate[i]   = INT_MIN;
        g_spd_pitch[i]  = INT_MIN;
        g_spd_volume[i] = INT_MIN;
    }
    g_current_voice_slot = g_default_voice_slot;

    /* chdir into the data dir so the engine's static constructor finds
     * eci.ini there, then dlopen eci.so from the same place. */
    char *err = NULL;
    if (enter_data_dir(&err) != 0) {
        if (msg) *msg = err ? err : strdup("EloquenceDataDir unusable");
        return -1;
    }
    char eci_so_path[1024];
    snprintf(eci_so_path, sizeof(eci_so_path), "%s/eci.so", g_data_dir);
    DBG("module_init: opening eci runtime at '%s'", eci_so_path);
    if (eci_runtime_open(eci_so_path, &eci, &err) != 0) {
        if (msg) *msg = err ? err : strdup("ECI library open failed");
        return -1;
    }

    /* Mark which languages we can advertise. Done from a static knowledge
     * table rather than runtime probing -- see mark_available_languages
     * for why repeatedly calling eciNewEx/eciDelete at init is unsafe. */
    mark_available_languages();

    /* If the configured default language is unavailable, fall back to
     * the first language that works (en-US in practice). */
    const LangEntry *def = find_lang_by_dialect(g_default_language);
    int def_idx = def ? (int)(def - g_langs) : -1;
    if (def_idx < 0 || !g_lang_available[def_idx]) {
        for (size_t li = 0; li < N_LANGS; li++) {
            if (g_lang_available[li]) {
                DBG("default language %#x unavailable, falling back to %s",
                    g_default_language, g_langs[li].human);
                g_default_language = g_langs[li].eci_dialect;
                break;
            }
        }
    }

    if (rebuild_engine_for_language(g_default_language) != 0) {
        if (msg) *msg = strdup(
            "eciNewEx returned NULL for every probed language -- check that "
            "EloquenceDataDir holds a working ECI runtime and language set");
        return -1;
    }

    switch (g_default_sample_rate) {
        case 0: h_engine_sample_rate_hz = 8000;  break;
        case 1: h_engine_sample_rate_hz = 11025; break;
        case 2: h_engine_sample_rate_hz = 22050; break;
    }
    DBG("engine sample rate = %d Hz (param %d)",
        h_engine_sample_rate_hz, g_default_sample_rate);

    if (resampler_setup(h_engine_sample_rate_hz) != 0) {
        if (msg) *msg = strdup("resampler setup failed");
        return -1;
    }

    module_audio_set_server();

    char ver[64] = {0};
    eci.Version(ver);
    if (msg) {
        char *out = malloc(160);
        if (out) {
            const LangEntry *L = find_lang_by_dialect(g_current_language);
            snprintf(out, 160,
                     "ETI Eloquence %s -- ready (rate=%d Hz, lang=%s, voice=%s)",
                     ver, h_engine_sample_rate_hz,
                     L ? L->human : "?",
                     g_voice_presets[g_current_voice_slot].name);
        }
        *msg = out;
    }
    return 0;
}

int module_close(void) {
    if (h_engine) {
        eci.Stop(h_engine);
        eci.Delete(h_engine);
        h_engine = NULL;
    }
    resampler_teardown();
    eci_runtime_close();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Voice listing                                                      */
/* ------------------------------------------------------------------ */
/* Build an upper-case IETF region tag: "us" -> "US". */
static void uppercase_region(char *p) {
    for (; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
}

SPDVoice **module_list_voices(void) {
    /* Capacity = N_LANGS * N_VOICE_PRESETS (the worst case before
     * gating). The actual count after filtering broken languages may
     * be smaller; trailing entries stay NULL. */
    int cap = (int)N_LANGS * (int)N_VOICE_PRESETS;
    SPDVoice **list = calloc(cap + 1, sizeof(SPDVoice *));
    if (!list) return NULL;
    int idx = 0;
    for (size_t li = 0; li < N_LANGS; li++) {
        if (!g_lang_available[li]) continue;
        for (int vi = 0; vi < (int)N_VOICE_PRESETS; vi++) {
            SPDVoice *v = calloc(1, sizeof(SPDVoice));
            if (!v) continue;
            /* Orca's UI groups by `language` (so we need the full IETF
             * tag here -- "en" alone collapses en-US and en-GB into one
             * "English" entry) and shows `variant` as the per-voice
             * "Person" label. `name` is the unique identifier passed
             * back as synthesis_voice; it must be distinct across the
             * whole list. */
            char ietf[16];
            snprintf(ietf, sizeof(ietf), "%s-%s",
                     g_langs[li].iso_lang, g_langs[li].iso_variant);
            uppercase_region(ietf + strlen(g_langs[li].iso_lang) + 1);

            char unique_name[64];
            const char *voice = voice_display_name(vi, &g_langs[li]);
            snprintf(unique_name, sizeof(unique_name), "%s-%s", voice, ietf);

            v->name     = strdup(unique_name);
            v->language = strdup(ietf);
            v->variant  = strdup(voice);
            list[idx++] = v;
        }
    }
    list[idx] = NULL;
    return list;
}

/* ------------------------------------------------------------------ */
/* SPD parameter -> ECI mapping                                       */
/* ------------------------------------------------------------------ */

/* SPD rate/pitch/volume range: -100..+100.
 *
 * Pitch and volume use ECI's 0..100 range, mapped linearly.
 *
 * Speed is different: Apple's eci.dylib accepts eciSpeed values 0..250
 * (the IBM ViaVoice range), with engine default = 50. Capping our output
 * at 100 left "+100 in Orca is still slow" because we only exposed ~40%
 * of the engine's real range. Map SPD -100..+100 linearly to ECI 0..200
 * so SPD=0 lands at ECI=100 (a fluent reading pace, ~2x engine default)
 * and SPD=+100 lands at ECI=200 (very fast, with 50 units of engine
 * headroom remaining if anyone needs even faster). */
static int spd_to_eci_pct(int spd_val) {
    int v = (spd_val + 100) / 2;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

static int spd_to_eci_speed(int spd_val) {
    int v = spd_val + 100;   /* -100..+100 -> 0..200 */
    if (v < 0) v = 0;
    if (v > 200) v = 200;
    return v;
}

int module_set(const char *var, const char *val) {
    if (!h_engine) return -1;
    DBG("module_set: %s = %s", var, val);

    /* rate/pitch/volume always set on ECI_ACTIVE_SLOT (slot 0) since
     * that's the slot the engine actually synthesizes with. We cache
     * the value indexed by the current preset slot so that the user's
     * adjustment travels with that voice -- selecting Shelley, dialing
     * her down to rate=-30, switching to Reed, and switching back to
     * Shelley keeps Shelley at -30. */
    if (strcasecmp(var, "rate") == 0) {
        int spd = atoi(val);
        g_spd_rate[g_current_voice_slot] = spd;
        eci.SetVoiceParam(h_engine, ECI_ACTIVE_SLOT,
                          eciSpeed, spd_to_eci_speed(spd));
        return 0;
    }
    if (strcasecmp(var, "pitch") == 0) {
        int spd = atoi(val);
        g_spd_pitch[g_current_voice_slot] = spd;
        eci.SetVoiceParam(h_engine, ECI_ACTIVE_SLOT,
                          eciPitchBaseline, spd_to_eci_pct(spd));
        return 0;
    }
    if (strcasecmp(var, "volume") == 0) {
        int spd = atoi(val);
        g_spd_volume[g_current_voice_slot] = spd;
        eci.SetVoiceParam(h_engine, ECI_ACTIVE_SLOT,
                          eciVolume, spd_to_eci_pct(spd));
        return 0;
    }
    if (strcasecmp(var, "synthesis_voice") == 0) {
        /* SPD voice names from our module_list_voices have the form
         * "<voice>-<lang>-<region>" e.g. "Reed-en-US", "Jacques-fr-fr".
         * Orca picks a voice from the list and sends only this string --
         * no accompanying language= setting -- so we have to extract
         * both pieces here, otherwise selecting "Reed-en-US" while the
         * engine sits on en-GB results in en-GB synthesizing American
         * English text (the symptom that prompted this fix).
         *
         * Match the voice-name prefix to pick the slot, then peel off
         * the suffix "<lang>-<region>" and feed it through find_lang_by_iso
         * for the language switch. */
        int matched_slot = -1;
        size_t name_len = 0;
        for (int i = 0; i < (int)N_VOICE_PRESETS; i++) {
            const VoicePreset *p = &g_voice_presets[i];
            size_t nlen = strlen(p->name);
            if (!strncasecmp(val, p->name, nlen)) {
                matched_slot = i; name_len = nlen; break;
            }
            if (p->name_fr) {
                size_t flen = strlen(p->name_fr);
                if (!strncasecmp(val, p->name_fr, flen)) {
                    matched_slot = i; name_len = flen; break;
                }
            }
        }
        if (matched_slot < 0) {
            DBG("unknown synthesis_voice '%s', keeping slot %d",
                val, g_current_voice_slot);
            return 0;
        }
        g_current_voice_slot = matched_slot;
        DBG("synthesis_voice -> slot %d (%s)",
            matched_slot, g_voice_presets[matched_slot].name);

        /* If there's a "-<lang>-<region>" tail, switch language too. */
        if (val[name_len] == '-' && val[name_len + 1]) {
            const char *lang_tail = val + name_len + 1;
            const LangEntry *L = find_lang_by_iso(lang_tail);
            if (L) {
                int idx = (int)(L - g_langs);
                if (g_lang_available[idx] &&
                    L->eci_dialect != g_current_language) {
                    DBG("synthesis_voice implies language %s; switching", L->human);
                    rebuild_engine_for_language(L->eci_dialect);
                }
            }
        }
        /* Push the chosen preset's parameter set into the engine's
         * active slot so the voice change actually takes effect on
         * the next synthesis. */
        if (h_engine)
            activate_voice_preset(h_engine, g_current_voice_slot);
        return 0;
    }
    if (strcasecmp(var, "language") == 0) {
        const LangEntry *L = find_lang_by_iso(val);
        if (!L) {
            DBG("unknown language '%s'", val);
            return 0;
        }
        int idx = (int)(L - g_langs);
        if (!g_lang_available[idx]) {
            DBG("language '%s' (%s) not available; engine refused init at probe",
                val, L->human);
            return 0;
        }
        if (L->eci_dialect == g_current_language) {
            DBG("language already %s", L->human);
            return 0;
        }
        /* Apple's eciSetParam(eciLanguageDialect, X) is unreliable mid-
         * session; recreate the engine via eciNewEx instead. The recreate
         * is ~0.6ms so it's transparent to the user. rebuild_engine_for_language
         * does an atomic swap -- on failure the old engine stays alive. */
        DBG("language switch %s -> %s; rebuilding engine",
            find_lang_by_dialect(g_current_language)
                ? find_lang_by_dialect(g_current_language)->human : "?",
            L->human);
        rebuild_engine_for_language(L->eci_dialect);
        return 0;
    }
    if (strcasecmp(var, "voice_type") == 0) {
        /* Match SPD voice_type tag against the preset table. */
        int slot = g_current_voice_slot;
        for (int i = 0; i < (int)N_VOICE_PRESETS; i++) {
            if (g_voice_presets[i].spd_voice_type &&
                !strcasecmp(val, g_voice_presets[i].spd_voice_type)) {
                slot = i; break;
            }
        }
        /* Child voices: SPD distinguishes CHILD_MALE / CHILD_FEMALE but
         * Apple ships only one child voice (Sandy, female). Send both
         * tags to that slot. */
        if (!strcasecmp(val, "CHILD_MALE") || !strcasecmp(val, "CHILD_FEMALE"))
            slot = 2;  /* Sandy */
        g_current_voice_slot = slot;
        DBG("voice_type %s -> slot %d (%s)",
            val, slot, g_voice_presets[slot].name);
        /* Activate the new voice in the engine's active slot. */
        activate_voice_preset(h_engine, g_current_voice_slot);
        return 0;
    }
    if (strcasecmp(var, "punctuation_mode") == 0) {
        /* SPD: none / some / most / all -> ECI eciTextMode.
         *
         * ECI text modes (cross-verified against Apple's eci.dylib):
         *   0 = normal (engine default punctuation handling)
         *   1 = alphanumeric (single chars pronounced one at a time)
         *   2 = verbatim (every char including punctuation read by name)
         *   3 = phonetic alphabet ("Tango" for T) -- NOT what "spell
         *       punctuation" means; we don't use this mode anywhere.
         */
        int mode = 0;
        if      (!strcasecmp(val, "none")) mode = 0;
        else if (!strcasecmp(val, "some")) mode = 0;
        else if (!strcasecmp(val, "most")) mode = 0;
        else if (!strcasecmp(val, "all"))  mode = 2;
        eci.SetParam(h_engine, eciTextMode, mode);
        return 0;
    }

    /* Silently accept unknown */
    return 0;
}

int module_loglevel_set(const char *var, const char *val) {
    if (strcasecmp(var, "log_level") == 0) {
        g_debug = atoi(val) > 0;
    }
    return 0;
}

int module_audio_set(const char *var, const char *val) {
    /* We only ever output via the speech-dispatcher audio server; no other
     * audio backends supported. Accept and ignore. */
    (void)var; (void)val;
    return 0;
}

int module_audio_init(char **status_info) {
    if (status_info) *status_info = strdup("ok");
    return 0;
}

int module_debug(int enable, const char *file) {
    g_debug = enable;
    (void)file;
    return 0;
}

/* ------------------------------------------------------------------ */
/* SSML stripper                                                      */
/* ------------------------------------------------------------------ */
/*
 * speech-dispatcher passes text that may contain SSML markup like
 * <speak>, <mark name="..."/>, <prosody rate="..">, <break time="..ms"/>
 * etc. ECI doesn't parse SSML -- it'd literally speak the tag text. We
 * strip all `<...>` runs and decode the standard XML entities.
 *
 * TODO: turn this into real SSML support eventually:
 *   - <mark name="..."/> -> eciInsertIndex + module_report_index_mark
 *     when eciIndexReply fires.
 *   - <prosody rate/pitch/volume> -> push/pop eciSetVoiceParam stack.
 *   - <break time="Nms"/> -> insert silence.
 *   - <voice name="..."> -> switch slot per-utterance.
 *
 * For now, plain text content is all that's preserved.
 */
static char *strip_ssml(const char *in, size_t len) {
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t j = 0, i = 0;

    while (i < len) {
        char c = in[i];

        if (c == '<') {
            /* Skip everything until the matching '>' (or buffer end). */
            while (i < len && in[i] != '>') i++;
            if (i < len) i++;  /* consume '>' */
            continue;
        }

        if (c == '&') {
            /* Try to decode an XML entity ending in ';'. */
            size_t k = i + 1;
            while (k < len && k - i < 10 && in[k] != ';') k++;
            if (k < len && in[k] == ';') {
                size_t elen = k - i - 1;
                if      (elen == 3 && !memcmp(in+i+1, "amp",  3)) out[j++] = '&';
                else if (elen == 2 && !memcmp(in+i+1, "lt",   2)) out[j++] = '<';
                else if (elen == 2 && !memcmp(in+i+1, "gt",   2)) out[j++] = '>';
                else if (elen == 4 && !memcmp(in+i+1, "quot", 4)) out[j++] = '"';
                else if (elen == 4 && !memcmp(in+i+1, "apos", 4)) out[j++] = '\'';
                else if (elen >= 2 && in[i+1] == '#') {
                    /* Numeric entity: &#NNN; or &#xHH; -- encode as UTF-8. */
                    int code = 0, base = 10;
                    const char *p = in + i + 2;
                    if (p < in + k && (*p == 'x' || *p == 'X')) { base = 16; p++; }
                    while (p < in + k) {
                        int d = (*p <= '9') ? (*p - '0')
                                : ((*p | 32) - 'a' + 10);
                        if (d < 0 || d >= base) { code = -1; break; }
                        code = code * base + d;
                        p++;
                    }
                    if (code < 0) {
                        out[j++] = '&';  /* malformed; preserve char */
                        i++; continue;
                    }
                    if (code < 0x80) {
                        out[j++] = (char)code;
                    } else if (code < 0x800) {
                        out[j++] = (char)(0xC0 | (code >> 6));
                        out[j++] = (char)(0x80 | (code & 0x3F));
                    } else if (code < 0x10000) {
                        out[j++] = (char)(0xE0 | (code >> 12));
                        out[j++] = (char)(0x80 | ((code >> 6) & 0x3F));
                        out[j++] = (char)(0x80 | (code & 0x3F));
                    } else if (code < 0x110000) {
                        out[j++] = (char)(0xF0 | (code >> 18));
                        out[j++] = (char)(0x80 | ((code >> 12) & 0x3F));
                        out[j++] = (char)(0x80 | ((code >> 6) & 0x3F));
                        out[j++] = (char)(0x80 | (code & 0x3F));
                    }
                } else {
                    /* Unknown named entity -- copy verbatim. */
                    memcpy(out + j, in + i, k - i + 1);
                    j += k - i + 1;
                }
                i = k + 1;
                continue;
            }
            /* No matching ';' -- fall through and copy '&' literally. */
        }

        out[j++] = c;
        i++;
    }
    out[j] = 0;
    return out;
}

/* ------------------------------------------------------------------ */
/* Speak                                                              */
/* ------------------------------------------------------------------ */
void module_speak_sync(const char *data, size_t bytes, SPDMessageType msgtype) {
    (void)bytes;
    if (!h_engine) {
        module_speak_error();
        return;
    }
    DBG("module_speak_sync (%d bytes, type=%d): '%.60s%s'",
        (int)bytes, msgtype, data, bytes > 60 ? "..." : "");

    /* Confirm the request was accepted (frees the server to start sending
     * STOP requests if needed). */
    g_stop_requested = 0;
    module_speak_ok();

    module_report_event_begin();

    /* The engine is already idle by the time we land here -- the
     * previous module_speak_sync blocked in eciSynchronize and the
     * explicit-interrupt path is module_stop, not this function.
     * Re-bind the callback + output buffer (cheap, idempotent) and
     * proceed to queue the new utterance. */
    eci.RegisterCallback(h_engine, eci_cb, NULL);
    eci.SetOutputBuffer(h_engine, PCM_CHUNK_SAMPLES, g_pcm_chunk);

    /* SPD passes the text as raw bytes (may contain SSML). Strip markup
     * + decode entities before feeding to ECI (which doesn't parse SSML
     * and would speak the literal tags otherwise). */
    char *text = strip_ssml(data, bytes);
    if (!text) {
        module_report_event_stop();
        return;
    }
    DBG("speak (post-strip): '%.80s%s'", text, strlen(text) > 80 ? "..." : "");

    /* For SPD_MSGTYPE_CHAR / SPD_MSGTYPE_KEY, each char is spelled out.
     * Mode 2 (verbatim) speaks "T" as "T" and "." as "period"; mode 3
     * is the NATO phonetic alphabet ("Tango") which isn't what users
     * want for letter-by-letter readout. */
    if (msgtype == SPD_MSGTYPE_CHAR || msgtype == SPD_MSGTYPE_KEY) {
        eci.SetParam(h_engine, eciTextMode, 2);
    } else {
        eci.SetParam(h_engine, eciTextMode, 0);
    }

    if (eci.AddText(h_engine, text) == ECIFalse) {
        DBG("eciAddText rejected the input (engine error %#x)",
            eci.ProgStatus(h_engine));
    }
    free(text);

    eci.Synthesize(h_engine);
    eci.Synchronize(h_engine);

    /* Flush any tail samples held by the resampler so we don't clip the end
     * of the last utterance. */
    resampler_flush();

    if (g_stop_requested)
        module_report_event_stop();
    else
        module_report_event_end();
}

/* Asynchronous variant — speech-dispatcher's main loop has a model where
 * EITHER speak() OR speak_sync() is used. We implement sync (simpler) and
 * leave speak() unimplemented so the loop calls speak_sync(). */
int module_speak(char *data, size_t bytes, SPDMessageType msgtype) {
    (void)data; (void)bytes; (void)msgtype;
    return -1;
}

/* Optional report hooks — we already emit events inline. */
void module_speak_begin(void) {}
void module_speak_end(void) {}
void module_speak_pause(void) {}
void module_speak_stop(void) {}

int module_stop(void) {
    g_stop_requested = 1;
    if (h_engine) eci.Stop(h_engine);
    return 0;
}

size_t module_pause(void) {
    if (h_engine) eci.Pause(h_engine, 1);
    return 0;
}

/*
 * Default main loop: just run the protocol until the server sends QUIT.
 * libspeechd_module's main() expects this symbol to be defined by the module.
 */
int module_loop(void) {
    return module_process(STDIN_FILENO, 1);
}
