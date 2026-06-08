/*
 * SPDX-License-Identifier: MIT
 *
 * eloquence_jni.c -- JNI bridge between an Android TextToSpeechService and the
 * converted Apple Eloquence engine (eci.so + language modules).
 *
 * The engine is dlopen'd at runtime from the app's data dir; its ECI C API is
 * resolved by dlsym. Synthesis is pull-based: AddText + Synthesize, with the
 * engine delivering 16-bit PCM through an ECI callback that we accumulate into
 * a Java short[]. The Java side (EloquenceTtsService) feeds that to the
 * platform SynthesisCallback.
 *
 * Engine quirks handled here mirror examples/speak.c and sd_eloquence:
 *   - RegisterCallback must precede SetOutputBuffer.
 *   - Synchronize is non-blocking on Apple's build; we drain via Speaking().
 *   - eci.ini (with absolute Path= entries) must be discoverable; the Java
 *     side writes it into dataDir and we chdir there before eciNew.
 */
#include <jni.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <android/log.h>

#define LOG_TAG "EloquenceJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* --- Minimal ECI ABI (mirrors sd_eloquence/src/eci/eci.h) ----------------- */
typedef void *ECIHand;
typedef const void *ECIInputText;
enum ECIMessage        { eciWaveformBuffer = 0 };
enum ECICallbackReturn { eciDataNotProcessed = 0, eciDataProcessed = 1, eciDataAbort = 2 };
enum ECIParam          { eciSampleRate = 5, eciLanguageDialect = 9 };
enum ECIVoiceParam     { eciPitchBaseline = 2, eciSpeed = 6, eciVolume = 7 };
typedef enum ECICallbackReturn (*ECICallback)(ECIHand, enum ECIMessage, long, void *);

/* Resolved entry points (names as exported by the converted eci.so). */
typedef ECIHand (*fn_New)(void);
typedef ECIHand (*fn_NewEx)(int);
typedef int     (*fn_Delete)(ECIHand);
typedef void    (*fn_Version)(char *);
typedef int     (*fn_SetParam)(ECIHand, int, int);
typedef int     (*fn_SetVoiceParam)(ECIHand, int, int, int);
typedef int     (*fn_AddText)(ECIHand, ECIInputText);
typedef int     (*fn_Synthesize)(ECIHand);
typedef int     (*fn_Synchronize)(ECIHand);
typedef int     (*fn_Speaking)(ECIHand);
typedef int     (*fn_Stop)(ECIHand);
typedef void    (*fn_RegisterCallback)(ECIHand, ECICallback, void *);
typedef int     (*fn_SetOutputBuffer)(ECIHand, int, short *);

/* One engine instance per native handle. */
#define CHUNK_SAMPLES 8192
typedef struct {
    void *lib;                 /* dlopen handle for eci.so                    */
    ECIHand eci;               /* engine handle                              */
    fn_Delete         Delete;
    fn_SetParam       SetParam;
    fn_SetVoiceParam  SetVoiceParam;
    fn_AddText        AddText;
    fn_Synthesize     Synthesize;
    fn_Synchronize    Synchronize;
    fn_Speaking       Speaking;
    fn_Stop           Stop;
    fn_SetOutputBuffer SetOutputBuffer;
    short  chunk[CHUNK_SAMPLES];
    short *pcm;                /* accumulation buffer for the active utterance */
    long   pcm_len;
    long   pcm_cap;
    int    abort;             /* set by Java onStop to short-circuit          */
} Engine;

static enum ECICallbackReturn pcm_cb(ECIHand h, enum ECIMessage msg,
                                     long lParam, void *pData) {
    (void)h;
    Engine *e = (Engine *)pData;
    if (e->abort) return eciDataAbort;
    if (msg != eciWaveformBuffer || lParam <= 0) return eciDataProcessed;
    long need = e->pcm_len + lParam;
    if (need > e->pcm_cap) {
        long ncap = e->pcm_cap ? e->pcm_cap * 2 : 65536;
        while (ncap < need) ncap *= 2;
        short *p = (short *)realloc(e->pcm, (size_t)ncap * sizeof(short));
        if (!p) return eciDataAbort;
        e->pcm = p;
        e->pcm_cap = ncap;
    }
    memcpy(e->pcm + e->pcm_len, e->chunk, (size_t)lParam * sizeof(short));
    e->pcm_len += lParam;
    return eciDataProcessed;
}

static void *sym(void *lib, const char *name) {
    void *p = dlsym(lib, name);
    if (!p) LOGE("missing symbol: %s", name);
    return p;
}

/* nativeInit(dataDir, dialect) -> handle (0 on failure).
 * dataDir holds eci.so + the language .so files + eci.ini (abs Path=). */
JNIEXPORT jlong JNICALL
Java_com_eloquence_tts_EloquenceNative_nativeInit(JNIEnv *env, jclass clazz,
                                                  jstring jDataDir, jint dialect) {
    (void)clazz;
    const char *dataDir = (*env)->GetStringUTFChars(env, jDataDir, NULL);

    /* The engine resolves eci.ini relative to cwd. */
    if (chdir(dataDir) != 0) LOGE("chdir(%s) failed", dataDir);

    char path[1024];
    snprintf(path, sizeof path, "%s/eci.so", dataDir);
    void *lib = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    (*env)->ReleaseStringUTFChars(env, jDataDir, dataDir);
    if (!lib) { LOGE("dlopen eci.so: %s", dlerror()); return 0; }

    Engine *e = (Engine *)calloc(1, sizeof(Engine));
    if (!e) { dlclose(lib); return 0; }
    e->lib = lib;

    fn_NewEx            NewEx            = (fn_NewEx)sym(lib, "eciNewEx");
    fn_New              New              = (fn_New)sym(lib, "eciNew");
    fn_RegisterCallback RegisterCallback = (fn_RegisterCallback)sym(lib, "eciRegisterCallback");
    e->Delete          = (fn_Delete)sym(lib, "eciDelete");
    e->SetParam        = (fn_SetParam)sym(lib, "eciSetParam");
    e->SetVoiceParam   = (fn_SetVoiceParam)sym(lib, "eciSetVoiceParam");
    e->AddText         = (fn_AddText)sym(lib, "eciAddText");
    e->Synthesize      = (fn_Synthesize)sym(lib, "eciSynthesize");
    e->Synchronize     = (fn_Synchronize)sym(lib, "eciSynchronize");
    e->Speaking        = (fn_Speaking)sym(lib, "eciSpeaking");
    e->Stop            = (fn_Stop)sym(lib, "eciStop");
    e->SetOutputBuffer = (fn_SetOutputBuffer)sym(lib, "eciSetOutputBuffer");

    if (!RegisterCallback || !e->AddText || !e->Synthesize || !e->SetOutputBuffer) {
        LOGE("required ECI symbols missing");
        free(e); dlclose(lib); return 0;
    }

    /* Prefer eciNewEx(dialect) so the language module loads up front. */
    e->eci = (NewEx && dialect) ? NewEx(dialect) : (New ? New() : NULL);
    if (!e->eci) { LOGE("eciNew/eciNewEx failed (check eci.ini + ProgStatus)"); free(e); dlclose(lib); return 0; }

    RegisterCallback(e->eci, pcm_cb, e);               /* before SetOutputBuffer */
    e->SetOutputBuffer(e->eci, CHUNK_SAMPLES, e->chunk);
    if (e->SetParam) e->SetParam(e->eci, eciSampleRate, 1);  /* 11025 Hz (Apple) */

    char ver[64] = {0};
    fn_Version Version = (fn_Version)dlsym(lib, "eciVersion");
    if (Version) { Version(ver); LOGI("Eloquence engine %s ready (dialect=0x%x)", ver, dialect); }
    return (jlong)(intptr_t)e;
}

JNIEXPORT void JNICALL
Java_com_eloquence_tts_EloquenceNative_nativeSetProsody(JNIEnv *env, jclass c,
        jlong handle, jint rate, jint pitch, jint volume) {
    (void)env; (void)c;
    Engine *e = (Engine *)(intptr_t)handle;
    if (!e || !e->SetVoiceParam) return;
    if (rate   >= 0) e->SetVoiceParam(e->eci, 0, eciSpeed,        rate);
    if (pitch  >= 0) e->SetVoiceParam(e->eci, 0, eciPitchBaseline, pitch);
    if (volume >= 0) e->SetVoiceParam(e->eci, 0, eciVolume,       volume);
}

/* Synthesize one utterance synchronously; return PCM as a short[] (11025 Hz,
 * mono, S16). Returns null on failure/abort. */
JNIEXPORT jshortArray JNICALL
Java_com_eloquence_tts_EloquenceNative_nativeSynthesize(JNIEnv *env, jclass c,
        jlong handle, jstring jText) {
    (void)c;
    Engine *e = (Engine *)(intptr_t)handle;
    if (!e) return NULL;
    const char *text = (*env)->GetStringUTFChars(env, jText, NULL);

    e->pcm_len = 0;
    e->abort = 0;
    e->AddText(e->eci, (ECIInputText)text);
    e->Synthesize(e->eci);
    if (e->Synchronize) e->Synchronize(e->eci);
    /* Apple's Synchronize is non-blocking; drain the worker thread. */
    if (e->Speaking) { int guard = 0; while (e->Speaking(e->eci) && !e->abort && guard++ < 100000) usleep(1000); }
    (*env)->ReleaseStringUTFChars(env, jText, text);

    if (e->abort || e->pcm_len == 0) return NULL;
    jshortArray out = (*env)->NewShortArray(env, (jsize)e->pcm_len);
    if (out) (*env)->SetShortArrayRegion(env, out, 0, (jsize)e->pcm_len, e->pcm);
    return out;
}

JNIEXPORT void JNICALL
Java_com_eloquence_tts_EloquenceNative_nativeStop(JNIEnv *env, jclass c, jlong handle) {
    (void)env; (void)c;
    Engine *e = (Engine *)(intptr_t)handle;
    if (!e) return;
    e->abort = 1;
    if (e->Stop) e->Stop(e->eci);
}

JNIEXPORT void JNICALL
Java_com_eloquence_tts_EloquenceNative_nativeShutdown(JNIEnv *env, jclass c, jlong handle) {
    (void)env; (void)c;
    Engine *e = (Engine *)(intptr_t)handle;
    if (!e) return;
    if (e->Delete) e->Delete(e->eci);
    free(e->pcm);
    /* Intentionally NOT dlclose(e->lib): the engine registers C++ atexit
     * destructors referencing engine state; process exit runs them cleanly,
     * an explicit dlclose can fire them in the wrong order. (Same rationale as
     * examples/speak.c.) */
    free(e);
}
