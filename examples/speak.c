/*
 * speak.c — full TTS smoke test using the converted Apple Eloquence engine.
 *
 * Usage:
 *   gcc speak.c -o speak -ldl
 *   ./speak /path/to/eci.so "Hello world."
 *   aplay -r 11025 -f S16_LE /tmp/eci_out.s16
 *
 * Requires eci.ini in cwd pointing at a language module (e.g. enu.so).
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ECI message types */
#define eciWaveformBuffer 0

/* ECI callback return codes */
#define eciDataProcessed 1
#define eciDataAbort     2

typedef int (*ECICallback)(void *hEngine, int msg, long lParam, void *pData);

/* The engine fills g_pcm_chunk; the callback appends each chunk into a
 * growing g_pcm so we capture the whole utterance. */
#define CHUNK_SAMPLES 8192
static int16_t  g_pcm_chunk[CHUNK_SAMPLES];
static int16_t *g_pcm        = NULL;
static long     g_pcm_len    = 0;
static long     g_pcm_cap    = 0;

static int my_callback(void *hEngine, int msg, long lParam, void *pData) {
    (void)hEngine; (void)pData;
    if (msg != eciWaveformBuffer || lParam <= 0) return eciDataProcessed;

    long need = g_pcm_len + lParam;
    if (need > g_pcm_cap) {
        long new_cap = g_pcm_cap ? g_pcm_cap * 2 : 65536;
        while (new_cap < need) new_cap *= 2;
        int16_t *p = realloc(g_pcm, (size_t)new_cap * sizeof(int16_t));
        if (!p) return eciDataAbort;
        g_pcm = p;
        g_pcm_cap = new_cap;
    }
    memcpy(g_pcm + g_pcm_len, g_pcm_chunk, (size_t)lParam * sizeof(int16_t));
    g_pcm_len += lParam;
    return eciDataProcessed;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-eci.so> [text]\n", argv[0]);
        return 1;
    }
    const char *text = argc > 2 ? argv[2] : "Hello world from Apple's Eloquence on Linux.";

    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen FAIL: %s\n", dlerror()); return 1; }

    typedef void*  (*eciNew_fn)(void);
    typedef int    (*eciDelete_fn)(void *);
    typedef int    (*eciSetOutputBuffer_fn)(void *, int, void *);
    typedef int    (*eciAddText_fn)(void *, const char *);
    typedef int    (*eciSynthesize_fn)(void *);
    typedef int    (*eciSynchronize_fn)(void *);
    typedef void   (*eciRegisterCallback_fn)(void *, ECICallback, void *);
    typedef void   (*eciVersion_fn)(char *);

    eciNew_fn             eciNew             = dlsym(h, "eciNew");
    eciDelete_fn          eciDelete          = dlsym(h, "eciDelete");
    eciSetOutputBuffer_fn eciSetOutputBuffer = dlsym(h, "eciSetOutputBuffer");
    eciAddText_fn         eciAddText         = dlsym(h, "eciAddText");
    eciSynthesize_fn      eciSynthesize      = dlsym(h, "eciSynthesize");
    eciSynchronize_fn     eciSynchronize     = dlsym(h, "eciSynchronize");
    eciRegisterCallback_fn eciRegisterCallback = dlsym(h, "eciRegisterCallback");
    eciVersion_fn         eciVersion         = dlsym(h, "eciVersion");

    if (!eciNew || !eciAddText || !eciRegisterCallback) {
        fprintf(stderr, "missing required symbols\n");
        return 1;
    }

    char ver[64] = {0};
    eciVersion(ver);
    printf("eciVersion: '%s'\n", ver);

    void *eci = eciNew();
    if (!eci) {
        fprintf(stderr, "eciNew failed (engine init error). "
                "Check eci.ini Path= and eci.dbg for diagnostics.\n");
        return 1;
    }

    /* RegisterCallback must come before SetOutputBuffer; the engine
     * fails the latter otherwise. */
    eciRegisterCallback(eci, my_callback, NULL);
    eciSetOutputBuffer(eci, CHUNK_SAMPLES, g_pcm_chunk);
    eciAddText(eci, text);
    eciSynthesize(eci);
    eciSynchronize(eci);

    int16_t maxv = 0;
    for (long i = 0; i < g_pcm_len; i++) {
        int16_t a = g_pcm[i] < 0 ? -g_pcm[i] : g_pcm[i];
        if (a > maxv) maxv = a;
    }
    printf("PCM: %ld samples (%.2fs @ 11025Hz), peak amplitude = %d\n",
           g_pcm_len, g_pcm_len / 11025.0, maxv);

    FILE *out = fopen("/tmp/eci_out.s16", "wb");
    if (out) {
        fwrite(g_pcm, 2, g_pcm_len, out);
        fclose(out);
        printf("Wrote /tmp/eci_out.s16 -- play with: aplay -r 11025 -f S16_LE /tmp/eci_out.s16\n");
    }

    eciDelete(eci);
    /* Skip dlclose() and free(g_pcm): the Apple dylibs register C++ atexit
     * destructors that reference engine state. Process exit (__cxa_finalize)
     * runs them cleanly; explicit dlclose can trigger them in the wrong order. */
    return 0;
}
