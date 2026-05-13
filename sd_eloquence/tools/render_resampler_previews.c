/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * sd_eloquence/tools/render_resampler_previews.c
 *
 * Generates WAV samples that demonstrate how each EloquenceResampleRate
 * / Quality / Phase / Steep setting changes the synthesized audio,
 * so users can audition each preset without trial-and-error in the
 * conf file.
 *
 * The tool dlopens eci.so + a language module via a tiny eci.ini in
 * the cwd, synthesizes a fixed pangram once at the engine's native
 * 11025 Hz, then runs that single rendering through sd_eloquence's
 * libsoxr wrapper with each combination requested.  The result is
 * sixteen WAV files covering per-axis sweeps with the other knobs
 * fixed at their eloquence.conf defaults.
 *
 * Usage:
 *   render_resampler_previews <work-dir> <output-dir>
 *
 * where work-dir contains eci.so + enu.so + eci.ini (the CI workflow
 * stages all three before invoking this tool).
 *
 * Links against sd_eloquence/src/audio/resampler.c (which is itself
 * GPL-2.0-or-later), so the resulting binary inherits that license.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "../src/audio/resampler.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A short pangram chosen so listeners can hear sibilants, plosives, and
 * vowel transitions through the resampler in a few seconds.  Kept short
 * so the per-file WAV stays under ~500 KB even at 48 kHz. */
static const char *PREVIEW_TEXT =
    "The quick brown fox jumps over the lazy dog. "
    "Speech synthesis sounds different at different sample rates "
    "and filter settings.";

#define CHUNK_SAMPLES 8192

/* Engine drains PCM into g_chunk per callback; we append into g_pcm so
 * the entire utterance is in one contiguous buffer after Synchronize. */
static int16_t  g_chunk[CHUNK_SAMPLES];
static int16_t *g_pcm     = NULL;
static size_t   g_pcm_len = 0;
static size_t   g_pcm_cap = 0;

typedef int (*ECICallback)(void *eng, int msg, long lParam, void *data);

static int append_pcm_callback(void *eng, int msg, long lParam, void *data) {
    (void)eng; (void)data;
    if (msg != 0 /* eciWaveformBuffer */ || lParam <= 0)
        return 1; /* eciDataProcessed */
    size_t need = g_pcm_len + (size_t)lParam;
    if (need > g_pcm_cap) {
        size_t cap = g_pcm_cap ? g_pcm_cap * 2 : 65536;
        while (cap < need) cap *= 2;
        int16_t *p = realloc(g_pcm, cap * sizeof(int16_t));
        if (!p) return 2; /* eciDataAbort */
        g_pcm = p;
        g_pcm_cap = cap;
    }
    memcpy(g_pcm + g_pcm_len, g_chunk, (size_t)lParam * sizeof(int16_t));
    g_pcm_len += (size_t)lParam;
    return 1;
}

/* Little-endian 16-bit mono PCM WAV.  Modern Linux is always LE so we
 * write the integers directly; if someone ever ports this to a BE
 * platform they'll get malformed WAVs and notice immediately. */
static int write_wav(const char *path, int rate, const int16_t *samples, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "fopen %s: ", path); perror(""); return -1; }
    uint32_t data_bytes  = (uint32_t)(n * 2);
    uint32_t chunk_size  = 36 + data_bytes;
    uint16_t channels    = 1;
    uint16_t fmt_tag     = 1;          /* PCM */
    uint32_t sample_rate = (uint32_t)rate;
    uint32_t byte_rate   = sample_rate * channels * 2;
    uint16_t block_align = channels * 2;
    uint16_t bits        = 16;
    uint32_t fmt_size    = 16;
    fwrite("RIFF", 1, 4, f);
    fwrite(&chunk_size, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_size,    4, 1, f);
    fwrite(&fmt_tag,     2, 1, f);
    fwrite(&channels,    2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate,   4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits,        2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes,  4, 1, f);
    fwrite(samples, 2, n, f);
    int rc = fclose(f);
    return rc == 0 ? 0 : -1;
}

/* output_hz == 0 means pass-through: resampler_new produces an active=0
 * resampler that just memcpy's. */
static int render_one(const char *out_path, int output_hz,
                      int quality, int phase, int steep) {
    char *err = NULL;
    Resampler *r = resampler_new(11025, output_hz, quality, phase, steep, &err);
    if (!r) {
        fprintf(stderr, "resampler_new(%d,%d,%d,%d) failed: %s\n",
                output_hz, quality, phase, steep, err ? err : "?");
        free(err);
        return -1;
    }

    int actual_rate = output_hz ? output_hz : 11025;
    /* Generous bound on output: input length * (out/in) + polyphase tail. */
    size_t bound = g_pcm_len * (size_t)actual_rate / 11025 + 8192;
    int16_t *out = malloc(bound * sizeof(int16_t));
    if (!out) { resampler_free(r); return -1; }

    int n  = resampler_process(r, g_pcm, (int)g_pcm_len, out, (int)bound);
    if (n < 0) { free(out); resampler_free(r); return -1; }
    int nf = resampler_flush(r, out + n, (int)(bound - (size_t)n));
    if (nf < 0) nf = 0;

    int rc = write_wav(out_path, actual_rate, out, (size_t)(n + nf));
    free(out);
    resampler_free(r);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <work-dir-with-eci.so> <output-dir>\n",
                argv[0]);
        return 1;
    }
    const char *work_dir = argv[1];
    const char *out_dir_arg = argv[2];

    /* Resolve out_dir to an absolute path before the chdir below, otherwise
     * the relative path passed in resolves against work_dir at write time. */
    char out_dir[2048];
    if (out_dir_arg[0] == '/') {
        snprintf(out_dir, sizeof(out_dir), "%s", out_dir_arg);
    } else {
        char cwd[1024];
        if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); return 1; }
        snprintf(out_dir, sizeof(out_dir), "%s/%s", cwd, out_dir_arg);
    }

    if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        perror("mkdir output dir");
        return 1;
    }
    if (access(out_dir, W_OK) != 0) {
        fprintf(stderr, "output dir %s not writable\n", out_dir);
        return 1;
    }

    /* The engine reads ./eci.ini at New() time, so anchor cwd to work_dir.
     * The CI workflow drops a one-line ini there pointing at ./enu.so. */
    if (chdir(work_dir) != 0) { perror("chdir"); return 1; }

    /* RTLD_LAZY rather than RTLD_NOW: the converted eci.so references C++
     * runtime symbols (`_Unwind_Resume`, `std::terminate`, ...) that the
     * Apple build expected from libc++ but the aarch64 Linux libc++.so.1
     * doesn't re-export.  None of those fire in the happy-path synthesis
     * we use the tool for, so deferring their resolution lets us run
     * cleanly on both arches without bringing the whole C++ runtime
     * resolution problem into the CI critical path. */
    void *eci_h = dlopen("./eci.so", RTLD_LAZY);
    if (!eci_h) {
        fprintf(stderr, "dlopen eci.so: %s\n", dlerror());
        return 1;
    }

    void *(*eciNew)(void)                          = dlsym(eci_h, "eciNew");
    int   (*eciSetOutputBuffer)(void *, int, void *) = dlsym(eci_h, "eciSetOutputBuffer");
    int   (*eciAddText)(void *, const char *)      = dlsym(eci_h, "eciAddText");
    int   (*eciSynthesize)(void *)                 = dlsym(eci_h, "eciSynthesize");
    int   (*eciSynchronize)(void *)                = dlsym(eci_h, "eciSynchronize");
    void  (*eciRegisterCallback)(void *, ECICallback, void *) =
        dlsym(eci_h, "eciRegisterCallback");

    if (!eciNew || !eciAddText || !eciSynthesize || !eciSynchronize
        || !eciRegisterCallback || !eciSetOutputBuffer) {
        fprintf(stderr, "ECI symbol resolution failed\n");
        return 1;
    }

    void *eng = eciNew();
    if (!eng) { fprintf(stderr, "eciNew failed (check eci.ini)\n"); return 1; }

    eciRegisterCallback(eng, append_pcm_callback, NULL);
    eciSetOutputBuffer(eng, CHUNK_SAMPLES, g_chunk);
    eciAddText(eng, PREVIEW_TEXT);
    eciSynthesize(eng);
    eciSynchronize(eng);

    printf("rendered %zu samples (%.2f sec @ 11025 Hz)\n",
           g_pcm_len, g_pcm_len / 11025.0);

    char path[1024];

    /* Rate sweep: quality=very-high, phase=intermediate, steep=off. */
    static const struct { int hz; const char *label; } rates[] = {
        {     0, "off"  }, { 16000, "16000" }, { 22050, "22050" },
        { 32000, "32000"}, { 44100, "44100" }, { 48000, "48000" },
    };
    for (size_t i = 0; i < sizeof(rates)/sizeof(rates[0]); i++) {
        snprintf(path, sizeof(path), "%s/rate-%s.wav", out_dir, rates[i].label);
        if (render_one(path, rates[i].hz, 4, 0, 0) != 0) return 1;
        printf("  wrote %s\n", path);
    }

    /* Quality sweep at the most common DAC rate. */
    static const char *q_names[] = { "quick", "low", "medium", "high", "very-high" };
    for (int q = 0; q < 5; q++) {
        snprintf(path, sizeof(path), "%s/quality-%s.wav", out_dir, q_names[q]);
        if (render_one(path, 44100, q, 0, 0) != 0) return 1;
        printf("  wrote %s\n", path);
    }

    /* Phase sweep. */
    static const char *p_names[] = { "intermediate", "linear", "minimum" };
    for (int p = 0; p < 3; p++) {
        snprintf(path, sizeof(path), "%s/phase-%s.wav", out_dir, p_names[p]);
        if (render_one(path, 44100, 4, p, 0) != 0) return 1;
        printf("  wrote %s\n", path);
    }

    /* Steep sweep. */
    for (int s = 0; s < 2; s++) {
        snprintf(path, sizeof(path), "%s/steep-%s.wav",
                 out_dir, s ? "on" : "off");
        if (render_one(path, 44100, 4, 0, s) != 0) return 1;
        printf("  wrote %s\n", path);
    }

    return 0;
}
