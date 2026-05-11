/*
 * sd_eloquence.c -- speech-dispatcher output module for ECI 6.x.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 * SPDX-License-Identifier: MIT
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

/* spd_module_main.h does an unqualified `#include <spd_audio.h>`; we
 * vendor a compatible copy because the Arch speech-dispatcher package
 * doesn't ship it. */
#include "spd_audio.h"
#include <speech-dispatcher/spd_module_main.h>

#ifdef HAVE_SOXR
#include <soxr.h>
#endif

#include "eci.h"
#include "voice_presets.h"

#define DBG(fmt, ...) \
    do { if (g_debug) fprintf(stderr, "sd_eloquence: " fmt "\n", ##__VA_ARGS__); } while (0)

/* Configuration loaded from eloquence.conf. */
static int   g_debug = 0;
static char  g_data_dir[512]       = "/usr/lib/eloquence";
static int   g_default_sample_rate = 1;  /* 0=8k, 1=11k, 2=22k */
static int   g_default_voice_slot  = 0;
static int   g_default_language    = eciGeneralAmericanEnglish;

static int   g_resample_rate    = 0;   /* 0 = pass-through */
static int   g_resample_quality = 4;   /* 0=quick .. 4=very-high */
static int   g_resample_phase   = 0;   /* 0=intermediate 1=linear 2=minimum */
static int   g_resample_steep   = 0;

static EciApi   eci;
static ECIHand  h_engine = NULL;
static int      h_engine_sample_rate_hz = 11025;
static int      g_current_language      = eciGeneralAmericanEnglish;
static int      g_current_voice_slot    = 0;
static volatile int g_stop_requested    = 0;

/* Per-voice SPD rate/pitch/volume; INT_MIN means "use the preset default". */
static int g_spd_rate[N_VOICE_PRESETS];
static int g_spd_pitch[N_VOICE_PRESETS];
static int g_spd_volume[N_VOICE_PRESETS];

/* Parallel to g_langs[]. Set by mark_available_languages from a static
 * known-good list -- probing with eciNewEx + eciDelete at init triggers
 * the dlclose-during-destructor bug also noted in eci_runtime.c. */
static int g_lang_available[14];

/* PCM buffer the engine fills via eciSetOutputBuffer. */
#define PCM_CHUNK_SAMPLES   8192
static int16_t  g_pcm_chunk[PCM_CHUNK_SAMPLES];

#ifdef HAVE_SOXR
static soxr_t  g_soxr = NULL;
/* 8x covers 11025 -> 48000 with headroom for soxr's polyphase flush
 * boundaries; less than that and soxr can write past the end of the
 * buffer. */
#define RESAMPLED_MAX_SAMPLES (PCM_CHUNK_SAMPLES * 8)
static int16_t  g_resampled[RESAMPLED_MAX_SAMPLES];
#endif

typedef struct {
    int   eci_dialect;
    int   ini_major;
    int   ini_minor;
    const char *so_name;
    const char *iso_lang;
    const char *iso_variant;
    const char *human;
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

/* Accepts either an ISO 639-1 prefix ("en", "de") or a full IETF tag
 * ("en-US", "de-DE"); full tag wins if both match. */
static const LangEntry *find_lang_by_iso(const char *tag) {
    for (size_t i = 0; i < N_LANGS; i++) {
        char ietf[16];
        snprintf(ietf, sizeof(ietf), "%s-%s",
                 g_langs[i].iso_lang, g_langs[i].iso_variant);
        if (strcasecmp(tag, ietf) == 0) return &g_langs[i];
    }
    for (size_t i = 0; i < N_LANGS; i++) {
        size_t lang_len = strlen(g_langs[i].iso_lang);
        if (strncasecmp(tag, g_langs[i].iso_lang, lang_len) == 0 &&
            (tag[lang_len] == 0 || tag[lang_len] == '-' ||
             tag[lang_len] == '_'))
            return &g_langs[i];
    }
    return NULL;
}

static const char *voice_display_name(int slot, const LangEntry *lang) {
    const VoicePreset *v = &g_voice_presets[slot];
    if (v->name_fr && lang && strcmp(lang->iso_lang, "fr") == 0)
        return v->name_fr;
    return v->name;
}

#ifdef HAVE_SOXR
static int resampler_setup(int input_hz) {
    if (g_soxr) {
        soxr_delete(g_soxr);
        g_soxr = NULL;
    }
    if (g_resample_rate <= 0 || g_resample_rate == input_hz) {
        DBG("resampler: pass-through (input=%d Hz)", input_hz);
        return 0;
    }

    static const unsigned long quality_table[] = {
        SOXR_QQ, SOXR_LQ, SOXR_MQ, SOXR_HQ, SOXR_VHQ
    };
    int qi = g_resample_quality;
    if (qi < 0 || qi >= (int)(sizeof(quality_table)/sizeof(quality_table[0])))
        qi = 4;
    unsigned long recipe = quality_table[qi];

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

/* Engine -> speechd PCM forwarder. Optionally resamples first. */
static enum ECICallbackReturn eci_cb(ECIHand h, enum ECIMessage msg,
                                     long lParam, void *pData) {
    (void)h; (void)pData;

    if (g_stop_requested)
        return eciDataAbort;

    if (msg != eciWaveformBuffer || lParam <= 0)
        return eciDataProcessed;

    /* End-of-utterance residuals can exceed PCM_CHUNK_SAMPLES on
     * some builds; clamping prevents libsoxr from reading past the
     * end of g_pcm_chunk and crashing in its polyphase loop. */
    if (lParam > PCM_CHUNK_SAMPLES) {
        DBG("eci_cb: clamping lParam=%ld to %d", lParam, PCM_CHUNK_SAMPLES);
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

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    if (*s == '"' && e > s+1 && e[-1] == '"') { *--e = 0; s++; }
    return s;
}

int module_config(const char *configfile) {
    if (!configfile) return 0;
    FILE *f = fopen(configfile, "r");
    if (!f) {
        fprintf(stderr, "sd_eloquence: cannot open %s: %s\n",
                configfile, strerror(errno));
        return 0;
    }

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (*p == 0 || *p == '#') continue;
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
            /* Legacy keys; the install-generated eci.ini superseded them. */
            DBG("ignoring legacy config key '%s'", p);
        } else if (strcasecmp(p, "EloquenceSampleRate") == 0) {
            g_default_sample_rate = atoi(val);
            if (g_default_sample_rate < 0 || g_default_sample_rate > 2)
                g_default_sample_rate = 1;
        } else if (strcasecmp(p, "EloquenceDefaultVoice") == 0) {
            /* Accepts a 0..7 slot index or a voice name. */
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
            /* Accepts a hex/decimal dialect code or an IETF tag. */
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

static int spd_to_eci_pct(int spd_val);
static int spd_to_eci_speed(int spd_val);

/* Apple's eci.dylib synthesizes from voice slot 0; slots 1..7 hold
 * inert preset data unless an explicit eciCopyVoice promotes them.
 * We always write the chosen preset's parameters into slot 0. */
#define ECI_ACTIVE_SLOT 0

/* Push all 8 per-voice parameters for `slot` into the active engine
 * slot. Every param is written every time -- letting individual params
 * carry over from the previous voice (e.g. Reed's pitch into Sandy)
 * was the source of voices sounding alike on Orca's preset picker.
 * Speed defaults to Apple's plist value (50); rate/pitch/volume SPD
 * overrides win over the preset defaults. */
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
}

static void apply_engine_state(ECIHand h) {
    if (eci.SetParam(h, eciSampleRate, g_default_sample_rate) < 0) {
        DBG("eciSampleRate=%d rejected, falling back to 11025 Hz",
            g_default_sample_rate);
        g_default_sample_rate = 1;
        eci.SetParam(h, eciSampleRate, g_default_sample_rate);
    }
    activate_voice_preset(h, g_current_voice_slot);
    eci.RegisterCallback(h, eci_cb, NULL);
    eci.SetOutputBuffer(h, PCM_CHUNK_SAMPLES, g_pcm_chunk);
}

/* First call: eciNewEx (only way to set initial language).
 * Subsequent calls: eciSetParam(eciLanguageDialect) -- recreating via
 * eciDelete + eciNewEx crashes Apple's build on the second reload of
 * a language .so. SetParam sometimes reports -1 but the engine
 * synthesizes in the new dialect anyway, so we trust that path. */
static int rebuild_engine_for_language(int dialect) {
    if (!h_engine) {
        h_engine = eci.NewEx(dialect);
        if (!h_engine) {
            const LangEntry *lang = find_lang_by_dialect(dialect);
            fprintf(stderr, "sd_eloquence: eciNewEx(%#x %s) failed\n",
                    dialect, lang ? lang->human : "?");
            return -1;
        }
        g_current_language = dialect;
        apply_engine_state(h_engine);
        DBG("engine created for %s",
            find_lang_by_dialect(dialect)
                ? find_lang_by_dialect(dialect)->human : "?");
        return 0;
    }
    eci.Stop(h_engine);
    eci.Synchronize(h_engine);
    int prev = eci.SetParam(h_engine, eciLanguageDialect, dialect);
    g_current_language = dialect;
    DBG("language -> %s via SetParam (prev=%#x)",
        find_lang_by_dialect(dialect)
            ? find_lang_by_dialect(dialect)->human : "?", prev);
    return 0;
}

/* CJK gated out: chs.so SIGSEGVs in reset_sent_vars on first AddText
 * after a switch to zh-CN, jpn.so destabilises Orca, and shipping
 * only the working subset (cht.so / ko-KR) was more confusing than
 * helpful. Re-enable once the RomanizerManager boundary is sorted. */
static void mark_available_languages(void) {
    for (size_t li = 0; li < N_LANGS; li++) {
        const char *so = g_langs[li].so_name;
        int cjk = (strcmp(so, "jpn.so") == 0 ||
                   strcmp(so, "kor.so") == 0 ||
                   strcmp(so, "chs.so") == 0 ||
                   strcmp(so, "cht.so") == 0);
        g_lang_available[li] = !cjk;
        DBG("%-30s %s", g_langs[li].human,
            g_lang_available[li] ? "available" : "unavailable (CJK gated)");
    }
}

/* chdir to g_data_dir so the engine's static constructor finds
 * eci.ini there at dlopen time. */
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
    for (size_t i = 0; i < N_VOICE_PRESETS; i++) {
        g_spd_rate[i]   = INT_MIN;
        g_spd_pitch[i]  = INT_MIN;
        g_spd_volume[i] = INT_MIN;
    }
    g_current_voice_slot = g_default_voice_slot;

    char *err = NULL;
    if (enter_data_dir(&err) != 0) {
        if (msg) *msg = err ? err : strdup("EloquenceDataDir unusable");
        return -1;
    }
    char eci_so_path[1024];
    snprintf(eci_so_path, sizeof(eci_so_path), "%s/eci.so", g_data_dir);
    if (eci_runtime_open(eci_so_path, &eci, &err) != 0) {
        if (msg) *msg = err ? err : strdup("ECI library open failed");
        return -1;
    }

    mark_available_languages();

    /* Fall back to the first available language if the configured
     * default is gated out. */
    const LangEntry *def = find_lang_by_dialect(g_default_language);
    int def_idx = def ? (int)(def - g_langs) : -1;
    if (def_idx < 0 || !g_lang_available[def_idx]) {
        for (size_t li = 0; li < N_LANGS; li++) {
            if (g_lang_available[li]) {
                DBG("default language unavailable, using %s",
                    g_langs[li].human);
                g_default_language = g_langs[li].eci_dialect;
                break;
            }
        }
    }

    if (rebuild_engine_for_language(g_default_language) != 0) {
        if (msg) *msg = strdup(
            "no language module loadable -- check EloquenceDataDir");
        return -1;
    }

    switch (g_default_sample_rate) {
        case 0: h_engine_sample_rate_hz = 8000;  break;
        case 1: h_engine_sample_rate_hz = 11025; break;
        case 2: h_engine_sample_rate_hz = 22050; break;
    }

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

static void uppercase_region(char *p) {
    for (; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
}

/* Orca groups voices by `language` (full IETF tag -- "en" alone collapses
 * en-US and en-GB into one entry) and displays `variant` as the "Person"
 * label. `name` is the unique id passed back as synthesis_voice. */
SPDVoice **module_list_voices(void) {
    int cap = (int)N_LANGS * (int)N_VOICE_PRESETS;
    SPDVoice **list = calloc(cap + 1, sizeof(SPDVoice *));
    if (!list) return NULL;
    int idx = 0;
    for (size_t li = 0; li < N_LANGS; li++) {
        if (!g_lang_available[li]) continue;
        for (int vi = 0; vi < (int)N_VOICE_PRESETS; vi++) {
            SPDVoice *v = calloc(1, sizeof(SPDVoice));
            if (!v) continue;
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

/* SPD ranges rate/pitch/volume as -100..+100. Pitch and volume map
 * linearly to ECI's 0..100; speed gets a wider 0..200 range because
 * Apple's eci.dylib accepts eciSpeed up to 250 and capping at 100 made
 * "+100 in Orca" feel slow. */
static int spd_to_eci_pct(int spd_val) {
    int v = (spd_val + 100) / 2;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

static int spd_to_eci_speed(int spd_val) {
    int v = spd_val + 100;
    if (v < 0) v = 0;
    if (v > 200) v = 200;
    return v;
}

int module_set(const char *var, const char *val) {
    if (!h_engine) return -1;
    DBG("module_set: %s = %s", var, val);

    /* rate/pitch/volume target the active slot but are cached by current
     * preset slot so the user's adjustment travels with that voice. */
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
        /* Voice names look like "Reed-en-US" / "Jacques-fr-fr". Orca
         * sends this without a separate language= setting, so we have
         * to extract both the preset and the dialect. */
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
        DBG("synthesis_voice -> %s", g_voice_presets[matched_slot].name);

        if (val[name_len] == '-' && val[name_len + 1]) {
            const char *lang_tail = val + name_len + 1;
            const LangEntry *L = find_lang_by_iso(lang_tail);
            if (L) {
                int idx = (int)(L - g_langs);
                if (g_lang_available[idx] &&
                    L->eci_dialect != g_current_language)
                    rebuild_engine_for_language(L->eci_dialect);
            }
        }
        if (h_engine)
            activate_voice_preset(h_engine, g_current_voice_slot);
        return 0;
    }
    if (strcasecmp(var, "language") == 0) {
        const LangEntry *L = find_lang_by_iso(val);
        if (!L) { DBG("unknown language '%s'", val); return 0; }
        int idx = (int)(L - g_langs);
        if (!g_lang_available[idx]) { DBG("language %s gated", L->human); return 0; }
        if (L->eci_dialect == g_current_language) return 0;
        rebuild_engine_for_language(L->eci_dialect);
        return 0;
    }
    if (strcasecmp(var, "voice_type") == 0) {
        int slot = g_current_voice_slot;
        for (int i = 0; i < (int)N_VOICE_PRESETS; i++) {
            if (g_voice_presets[i].spd_voice_type &&
                !strcasecmp(val, g_voice_presets[i].spd_voice_type)) {
                slot = i; break;
            }
        }
        /* SPD has CHILD_MALE and CHILD_FEMALE; Apple ships only Sandy
         * (slot 2). Map both child tags to her. */
        if (!strcasecmp(val, "CHILD_MALE") || !strcasecmp(val, "CHILD_FEMALE"))
            slot = 2;
        g_current_voice_slot = slot;
        activate_voice_preset(h_engine, g_current_voice_slot);
        return 0;
    }
    if (strcasecmp(var, "punctuation_mode") == 0) {
        /* ECI eciTextMode: 0=normal, 1=alphanumeric, 2=verbatim
         * (reads punctuation by name), 3=phonetic alphabet. We never
         * use mode 3 -- it's NATO ("Tango" for T), not "spell
         * punctuation". */
        int mode = !strcasecmp(val, "all") ? 2 : 0;
        eci.SetParam(h_engine, eciTextMode, mode);
        return 0;
    }
    return 0;
}

int module_loglevel_set(const char *var, const char *val) {
    if (strcasecmp(var, "log_level") == 0) {
        g_debug = atoi(val) > 0;
    }
    return 0;
}

int module_audio_set(const char *var, const char *val) {
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

/* Strip SSML tags and decode XML entities from speechd-supplied text;
 * ECI doesn't parse SSML and would speak the literal tags otherwise.
 *
 * TODO: real SSML support -- <mark> -> eciInsertIndex, <prosody> ->
 * push/pop eciSetVoiceParam, <break> -> silence, <voice> -> slot swap. */
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

void module_speak_sync(const char *data, size_t bytes, SPDMessageType msgtype) {
    if (!h_engine) {
        module_speak_error();
        return;
    }
    DBG("module_speak_sync (%d bytes, type=%d): '%.60s%s'",
        (int)bytes, msgtype, data, bytes > 60 ? "..." : "");

    g_stop_requested = 0;
    module_speak_ok();
    module_report_event_begin();

    eci.RegisterCallback(h_engine, eci_cb, NULL);
    eci.SetOutputBuffer(h_engine, PCM_CHUNK_SAMPLES, g_pcm_chunk);

    char *text = strip_ssml(data, bytes);
    if (!text) {
        module_report_event_stop();
        return;
    }
    DBG("speak (post-strip): '%.80s%s'", text, strlen(text) > 80 ? "..." : "");

    /* CHAR/KEY: text mode 2 (verbatim) speaks "T" as "T" and "." as
     * "period". Mode 3 is the NATO phonetic alphabet ("Tango"), not
     * what spell-out should produce. */
    eci.SetParam(h_engine, eciTextMode,
                 (msgtype == SPD_MSGTYPE_CHAR || msgtype == SPD_MSGTYPE_KEY)
                 ? 2 : 0);

    if (eci.AddText(h_engine, text) == ECIFalse) {
        DBG("eciAddText rejected (status=%#x)", eci.ProgStatus(h_engine));
    }
    free(text);

    eci.Synthesize(h_engine);
    eci.Synchronize(h_engine);
    resampler_flush();

    if (g_stop_requested)
        module_report_event_stop();
    else
        module_report_event_end();
}

/* libspeechd_module's main loop calls EITHER module_speak() OR
 * module_speak_sync(); returning -1 here routes it to the sync path. */
int module_speak(char *data, size_t bytes, SPDMessageType msgtype) {
    (void)data; (void)bytes; (void)msgtype;
    return -1;
}

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

int module_loop(void) {
    return module_process(STDIN_FILENO, 1);
}
