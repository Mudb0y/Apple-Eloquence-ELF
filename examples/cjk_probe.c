/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * cjk_probe -- minimum reproducer for the CJK first-AddText crashes.
 *
 * Usage: cjk_probe <path/to/eci.so> <dialect-hex>
 *   e.g. cjk_probe /usr/lib/eloquence/eci.so 0x00060000   # zh-CN Mandarin
 *
 * Loads eci.so, calls eciNewEx(dialect), registers a callback, sends a tiny
 * dialect-appropriate test phrase, Synchronizes, prints PCM stats or
 * (more usefully) crashes inside the engine so we can characterize.
 *
 * Build under valgrind:  valgrind --leak-check=no --read-var-info=yes \
 *                          build/examples/cjk_probe ./eci.so 0x00060000
 * Build under gdb:       gdb --args build/examples/cjk_probe ./eci.so 0x00060000
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *ECIHand;
typedef int   ECIInputText;  /* opaque; we pass char *cast */
typedef enum { eciDataNotProcessed, eciDataProcessed, eciDataAbort } ECICallbackReturn;
typedef ECICallbackReturn (*ECICallback)(ECIHand, int, long, void *);

#define PCM_CAP 8192
static int16_t pcm[PCM_CAP];
static long total_samples = 0;
static long max_sample = 0;

static ECICallbackReturn cb(ECIHand h, int msg, long lp, void *d) {
    (void)h; (void)d;
    if (msg == 0 /* eciWaveformBuffer */ && lp > 0) {
        total_samples += lp;
        for (long i = 0; i < lp; i++) {
            long v = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (v > max_sample) max_sample = v;
        }
    }
    return eciDataProcessed;
}

/* Encoding-specific test phrase ("hello"). */
static const char *phrase_for(int dialect) {
    switch (dialect) {
        case 0x00060000: return "\xC4\xE3\xBA\xC3";  /* 你好 in gb18030 */
        case 0x00060001: return "\xA7\x41\xA6\x6E";  /* 你好 in big5 */
        case 0x00080000: return "\x82\xB1\x82\xF1\x82\xC9\x82\xBF\x82\xCD";  /* こんにちは in cp932 */
        case 0x000A0000: return "\xBE\xC8\xB3\xE7";  /* 안녕 in cp949 */
        default: return "hello";
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <eci.so> <dialect-hex>\n", argv[0]);
        return 2;
    }
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    ECIHand (*NewEx)(int)            = dlsym(lib, "eciNewEx");
    void (*RegisterCallback)(ECIHand, ECICallback, void *) = dlsym(lib, "eciRegisterCallback");
    int  (*SetOutputBuffer)(ECIHand, int, short *) = dlsym(lib, "eciSetOutputBuffer");
    int  (*AddText)(ECIHand, const void *) = dlsym(lib, "eciAddText");
    int  (*Synthesize)(ECIHand)            = dlsym(lib, "eciSynthesize");
    int  (*Synchronize)(ECIHand)           = dlsym(lib, "eciSynchronize");
    int  (*Delete)(ECIHand)                = dlsym(lib, "eciDelete");
    int  (*ProgStatus)(ECIHand)            = dlsym(lib, "eciProgStatus");

    int dialect = (int)strtol(argv[2], NULL, 0);
    fprintf(stderr, "cjk_probe: eciNewEx(0x%08x)...\n", dialect);
    ECIHand h = NewEx(dialect);
    fprintf(stderr, "  -> handle=%p\n", h);
    if (!h) return 1;

    RegisterCallback(h, cb, NULL);
    SetOutputBuffer(h, PCM_CAP, pcm);

    const char *p = phrase_for(dialect);
    fprintf(stderr, "cjk_probe: AddText (%zu bytes)\n", strlen(p));
    AddText(h, p);
    fprintf(stderr, "  ProgStatus = 0x%x\n", ProgStatus(h));

    fprintf(stderr, "cjk_probe: Synthesize + Synchronize...\n");
    Synthesize(h);
    Synchronize(h);
    fprintf(stderr, "cjk_probe: %ld samples, peak %ld\n", total_samples, max_sample);

    Delete(h);
    /* Intentionally NO dlclose; matches the engine wrapper's behavior. */
    return total_samples > 0 ? 0 : 1;
}
