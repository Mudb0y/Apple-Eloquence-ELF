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

#define DBG(fmt, ...) \
    do { if (g_debug) fprintf(stderr, "sd_eloquence: " fmt "\n", ##__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ */
/* Configuration loaded from eloquence.conf                           */
/* ------------------------------------------------------------------ */
static int   g_debug = 0;
static char  g_eci_so[512]    = "/usr/lib/eci.so";
static char  g_voice_path[512] = "";   /* If set, used for [1.0]Path= in eci.ini */
static int   g_default_sample_rate = 1;  /* 0=8k, 1=11k, 2=22k */
static int   g_default_voice_slot  = 0;
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
static volatile int g_stop_requested    = 0;

/* PCM buffer the engine fills via eciSetOutputBuffer + our callback. */
#define PCM_CHUNK_SAMPLES   8192        /* per-chunk request, in samples */
static int16_t  g_pcm_chunk[PCM_CHUNK_SAMPLES];

#ifdef HAVE_SOXR
static soxr_t  g_soxr = NULL;
/* Output buffer for resampled PCM. Sized for the worst case (22050->48000 ≈ 2.18x). */
#define RESAMPLED_MAX_SAMPLES (PCM_CHUNK_SAMPLES * 5)
static int16_t  g_resampled[RESAMPLED_MAX_SAMPLES];
#endif

/* Voice presets — ECI ships 8 named slots */
static const char *g_voice_names[ECI_PRESET_VOICES] = {
    "Wade",     /* 0  Adult Male 1   - default */
    "Flo",      /* 1  Adult Female 1 */
    "Bobbie",   /* 2  Child */
    "Reed",     /* 3  Adult Male 2 */
    "Brian",    /* 4  Adult Male 3 */
    "Dawn",     /* 5  Adult Female 2 */
    "Grandma",  /* 6  Elderly Female */
    "Grandpa",  /* 7  Elderly Male */
};

/* Map ECI language-dialect code -> SPDVoice "language" / "variant" strings. */
typedef struct {
    int   eci_dialect;
    const char *language;   /* ISO 639-1 */
    const char *variant;    /* short region tag we use in SPDVoice */
    const char *human;      /* display name */
} LangEntry;

static const LangEntry g_langs[] = {
    { eciGeneralAmericanEnglish, "en", "us",     "American English" },
    { eciBritishEnglish,         "en", "gb",     "British English" },
    { eciStandardGerman,         "de", "de",     "German" },
    { eciCastilianSpanish,       "es", "es",     "Castilian Spanish" },
    { eciMexicanSpanish,         "es", "mx",     "Latin American Spanish" },
    { eciStandardFrench,         "fr", "fr",     "French" },
    { eciCanadianFrench,         "fr", "ca",     "Canadian French" },
    { eciStandardItalian,        "it", "it",     "Italian" },
    { eciStandardFinnish,        "fi", "fi",     "Finnish" },
    { eciBrazilianPortuguese,    "pt", "br",     "Brazilian Portuguese" },
    { eciStandardJapanese,       "ja", "jp",     "Japanese" },
    { eciStandardKorean,         "ko", "kr",     "Korean" },
    { eciMandarinChinese,        "zh", "cn",     "Mandarin Chinese (Simplified)" },
    { eciTaiwaneseMandarin,      "zh", "tw",     "Mandarin Chinese (Traditional)" },
};
#define N_LANGS (sizeof(g_langs)/sizeof(g_langs[0]))

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
static int eci_cb(ECIHand h, int msg, long lParam, void *pData) {
    (void)h; (void)pData;

    if (g_stop_requested)
        return eciDataAbort;

    if (msg != eciWaveformBuffer)
        return eciDataProcessed;

    if (lParam <= 0)
        return eciDataProcessed;

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
        } else if (strcasecmp(p, "EciLibrary") == 0) {
            strncpy(g_eci_so, val, sizeof(g_eci_so)-1);
            g_eci_so[sizeof(g_eci_so)-1] = 0;
        } else if (strcasecmp(p, "EciVoicePath") == 0) {
            strncpy(g_voice_path, val, sizeof(g_voice_path)-1);
            g_voice_path[sizeof(g_voice_path)-1] = 0;
        } else if (strcasecmp(p, "EloquenceSampleRate") == 0) {
            g_default_sample_rate = atoi(val);
            if (g_default_sample_rate < 0 || g_default_sample_rate > 2)
                g_default_sample_rate = 1;
        } else if (strcasecmp(p, "EloquenceDefaultVoice") == 0) {
            g_default_voice_slot = atoi(val) & 7;
        } else if (strcasecmp(p, "EloquenceDefaultLanguage") == 0) {
            /* Allow decimal or 0x... hex */
            g_default_language = (int)strtol(val, NULL, 0);
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
    DBG("config: debug=%d eci=%s rate=%d voice=%d lang=%#x",
        g_debug, g_eci_so, g_default_sample_rate, g_default_voice_slot,
        g_default_language);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Engine init / lifecycle                                            */
/* ------------------------------------------------------------------ */

/* Write a minimal eci.ini next to the .so if EciVoicePath is set and no
 * eci.ini already exists in cwd. This is a convenience for users who don't
 * want to manage the ini themselves. */
static void maybe_write_eci_ini(void) {
    if (!g_voice_path[0]) return;
    if (access("eci.ini", F_OK) == 0) return;  /* user-provided ini; leave alone */

    FILE *f = fopen("eci.ini", "w");
    if (!f) {
        DBG("cannot write eci.ini in cwd: %s", strerror(errno));
        return;
    }
    fprintf(f, "[1.0]\nPath=%s\nVersion=6.1\n", g_voice_path);
    fclose(f);
    DBG("auto-wrote eci.ini -> Path=%s", g_voice_path);
}

int module_init(char **msg) {
    /* The ECI engine parses eci.ini in a static constructor invoked during
     * dlopen, not in eciNew() -- so our auto-write must happen BEFORE the
     * library load, otherwise the engine will see no ini and eciNew returns
     * NULL.  */
    maybe_write_eci_ini();

    DBG("module_init: opening eci library at '%s'", g_eci_so);
    char *err = NULL;
    if (eci_runtime_open(g_eci_so, &eci, &err) != 0) {
        if (msg) *msg = err ? err : strdup("ECI library open failed");
        return -1;
    }

    h_engine = eci.New();
    if (!h_engine) {
        if (msg) *msg = strdup(
            "eciNew returned NULL -- check eci.ini Path= and that the language "
            "module .so is at that absolute path");
        return -1;
    }

    /* Apply default sample rate (param 5 per SDK). eciSetParam returns the
     * previous value on success and -1 on rejection. If the engine rejects
     * the requested rate (Apple's eci.dylib 6.1 refuses 22050), fall back
     * to 11025 and re-issue SetParam so the engine state matches what we
     * report downstream. */
    if (eci.SetParam(h_engine, eciSampleRate, g_default_sample_rate) < 0) {
        DBG("setParam(eciSampleRate=%d) rejected; falling back to 11025 Hz",
            g_default_sample_rate);
        g_default_sample_rate = 1;
        if (eci.SetParam(h_engine, eciSampleRate, g_default_sample_rate) < 0) {
            DBG("setParam(eciSampleRate=1) also rejected; engine is in an unknown state");
        }
    }
    switch (g_default_sample_rate) {
        case 0: h_engine_sample_rate_hz = 8000;  break;
        case 1: h_engine_sample_rate_hz = 11025; break;
        case 2: h_engine_sample_rate_hz = 22050; break;
    }
    DBG("engine sample rate = %d Hz (param %d)",
        h_engine_sample_rate_hz, g_default_sample_rate);

    /* Set up output resampler if user configured one. */
    if (resampler_setup(h_engine_sample_rate_hz) != 0) {
        if (msg) *msg = strdup("resampler setup failed");
        return -1;
    }

    /* Default voice slot */
    eci.SetParam(h_engine, eciLanguageDialect, g_default_language);

    /* Audio output: stream wave events to speechd's audio server */
    module_audio_set_server();
    eci.RegisterCallback(h_engine, eci_cb, NULL);
    eci.SetOutputBuffer(h_engine, PCM_CHUNK_SAMPLES, g_pcm_chunk);

    char ver[64] = {0};
    eci.Version(ver);
    if (msg) {
        char *out = malloc(128);
        if (out) {
            snprintf(out, 128, "ETI Eloquence %s -- ready (rate=%d Hz)",
                     ver, h_engine_sample_rate_hz);
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
SPDVoice **module_list_voices(void) {
    int n = (int)N_LANGS * ECI_PRESET_VOICES;
    SPDVoice **list = calloc(n + 1, sizeof(SPDVoice *));
    if (!list) return NULL;
    int idx = 0;
    for (size_t li = 0; li < N_LANGS; li++) {
        for (int vi = 0; vi < ECI_PRESET_VOICES; vi++) {
            SPDVoice *v = calloc(1, sizeof(SPDVoice));
            if (!v) continue;  /* OOM: skip this voice rather than abort */
            char name[64];
            /* Format: "Wade (American English)". The voice name alone isn't
             * unique across the 14*8 grid -- the language disambiguates. */
            snprintf(name, sizeof(name), "%s (%s)",
                     g_voice_names[vi], g_langs[li].human);
            v->name     = strdup(name);
            v->language = strdup(g_langs[li].language);
            v->variant  = strdup(g_langs[li].variant);
            list[idx++] = v;
        }
    }
    list[idx] = NULL;
    return list;
}

/* ------------------------------------------------------------------ */
/* SPD parameter -> ECI mapping                                       */
/* ------------------------------------------------------------------ */

/* SPD rate/pitch/volume range: -100..+100. ECI uses 0..100 for these. */
static int spd_to_eci_pct(int spd_val) {
    /* Linear map -100..+100 -> 0..100 */
    int v = (spd_val + 100) / 2;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

static int current_voice_slot = 0;

int module_set(const char *var, const char *val) {
    if (!h_engine) return -1;
    DBG("module_set: %s = %s", var, val);

    if (strcasecmp(var, "rate") == 0) {
        int v = spd_to_eci_pct(atoi(val));
        eci.SetVoiceParam(h_engine, current_voice_slot, eciSpeed, v);
        return 0;
    }
    if (strcasecmp(var, "pitch") == 0) {
        int v = spd_to_eci_pct(atoi(val));
        eci.SetVoiceParam(h_engine, current_voice_slot, eciPitchBaseline, v);
        return 0;
    }
    if (strcasecmp(var, "volume") == 0) {
        int v = spd_to_eci_pct(atoi(val));
        eci.SetVoiceParam(h_engine, current_voice_slot, eciVolume, v);
        return 0;
    }
    if (strcasecmp(var, "synthesis_voice") == 0) {
        /* Voice names of form "Wade", "Flo", etc. */
        for (int i = 0; i < ECI_PRESET_VOICES; i++) {
            if (strstr(val, g_voice_names[i]) != NULL) {
                current_voice_slot = i;
                /* No direct "activate this preset" -- the per-voice params
                 * we set above are applied to current_voice_slot. */
                return 0;
            }
        }
        DBG("unknown synthesis_voice '%s', keeping slot %d", val, current_voice_slot);
        return 0;
    }
    if (strcasecmp(var, "language") == 0) {
        /* Match on ISO code prefix */
        for (size_t i = 0; i < N_LANGS; i++) {
            if (strncasecmp(val, g_langs[i].language, 2) == 0) {
                eci.SetParam(h_engine, eciLanguageDialect, g_langs[i].eci_dialect);
                DBG("language -> %s (%#x)", g_langs[i].human, g_langs[i].eci_dialect);
                return 0;
            }
        }
        DBG("unknown language '%s'", val);
        return 0;
    }
    if (strcasecmp(var, "voice_type") == 0) {
        /* SPD voice types: MALE1, MALE2, MALE3, FEMALE1, FEMALE2, FEMALE3, CHILD_MALE, CHILD_FEMALE */
        int slot = 0;
        if      (!strcasecmp(val, "MALE1"))         slot = 0;  /* Wade */
        else if (!strcasecmp(val, "FEMALE1"))       slot = 1;  /* Flo */
        else if (!strcasecmp(val, "CHILD_MALE") ||
                 !strcasecmp(val, "CHILD_FEMALE"))  slot = 2;  /* Bobbie */
        else if (!strcasecmp(val, "MALE2"))         slot = 3;
        else if (!strcasecmp(val, "MALE3"))         slot = 4;
        else if (!strcasecmp(val, "FEMALE2"))       slot = 5;
        else if (!strcasecmp(val, "FEMALE3"))       slot = 6;  /* Grandma */
        else                                        slot = 0;
        current_voice_slot = slot;
        return 0;
    }
    if (strcasecmp(var, "punctuation_mode") == 0) {
        /* SPD: none / some / most / all -> ECI eciTextMode */
        int mode = 0;
        if (!strcasecmp(val, "none")) mode = 1;        /* skip punctuation */
        else if (!strcasecmp(val, "some")) mode = 0;   /* default */
        else if (!strcasecmp(val, "most")) mode = 0;
        else if (!strcasecmp(val, "all"))  mode = 3;   /* spell punctuation */
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

    /* Tell engine to stop anything in flight (defensive) and queue this text. */
    eci.Stop(h_engine);
    /* Reset the output buffer since eciStop can detach it on some builds. */
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

    /* For msgtype == SPD_MSGTYPE_CHAR / SPD_MSGTYPE_KEY: use spell mode. */
    if (msgtype == SPD_MSGTYPE_CHAR || msgtype == SPD_MSGTYPE_KEY) {
        eci.SetParam(h_engine, eciTextMode, 3);   /* spell */
    } else {
        eci.SetParam(h_engine, eciTextMode, 0);   /* normal */
    }

    /* eciAddText returns nonzero on success, 0 on failure. */
    if (!eci.AddText(h_engine, text)) {
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
