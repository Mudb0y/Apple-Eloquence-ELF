/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/runtime.c -- dlopen the ECI runtime + populate an EciApi function table.
 *
 * The ECI engine registers global C++ destructors on dlopen; calling dlclose
 * runs them in an order that crashes inside libc atexit on Apple's eci.dylib
 * Linux conversion. We leave the library mapped until process exit.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */

#include "runtime.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *g_handle = NULL;

#define LOAD(name)                                                          \
    do {                                                                    \
        void *sym = dlsym(g_handle, "eci" #name);                           \
        if (!sym) {                                                         \
            if (errmsg) {                                                   \
                char buf[512];                                              \
                snprintf(buf, sizeof(buf),                                  \
                         "eci_runtime_open: dlsym(eci" #name ") failed: %s",\
                         dlerror());                                        \
                *errmsg = strdup(buf);                                      \
            }                                                               \
            dlclose(g_handle);                                              \
            g_handle = NULL;                                                \
            return -1;                                                      \
        }                                                                   \
        api->name = sym;                                                    \
    } while (0)

int eci_runtime_open(const char *eci_so_path, EciApi *api, char **errmsg) {
    if (g_handle) {
        if (errmsg) *errmsg = strdup("eci_runtime_open: already loaded");
        return -1;
    }
    memset(api, 0, sizeof(*api));

    g_handle = dlopen(eci_so_path, RTLD_NOW | RTLD_GLOBAL);
    if (!g_handle) {
        if (errmsg) {
            char buf[1024];
            snprintf(buf, sizeof(buf), "dlopen(%s): %s",
                     eci_so_path, dlerror());
            *errmsg = strdup(buf);
        }
        return -1;
    }

    /* Resolve every IBM-documented entry point. If any are missing, the
     * runtime doesn't conform to the IBM ABI and we refuse to load. */

    /* Lifecycle */
    LOAD(New);
    LOAD(NewEx);
    LOAD(Delete);
    LOAD(Reset);
    LOAD(IsBeingReentered);
    LOAD(Version);

    /* Diagnostics */
    LOAD(ProgStatus);
    LOAD(ErrorMessage);
    LOAD(ClearErrors);
    LOAD(TestPhrase);

    /* Single-shot speak */
    LOAD(SpeakText);
    LOAD(SpeakTextEx);

    /* Parameters */
    LOAD(GetParam);
    LOAD(SetParam);
    LOAD(GetDefaultParam);
    LOAD(SetDefaultParam);

    /* Voices */
    LOAD(CopyVoice);
    LOAD(GetVoiceName);
    LOAD(SetVoiceName);
    LOAD(GetVoiceParam);
    LOAD(SetVoiceParam);

    /* Synthesis queue */
    LOAD(AddText);
    LOAD(InsertIndex);
    LOAD(Synthesize);
    LOAD(SynthesizeFile);
    LOAD(ClearInput);
    LOAD(GeneratePhonemes);
    LOAD(GetIndex);

    /* Playback control */
    LOAD(Stop);
    LOAD(Speaking);
    LOAD(Synchronize);
    LOAD(Pause);

    /* Output routing */
    LOAD(SetOutputBuffer);
    LOAD(SetOutputFilename);
    LOAD(SetOutputDevice);

    /* Callbacks */
    LOAD(RegisterCallback);

    /* Languages */
    LOAD(GetAvailableLanguages);

    /* Dictionaries */
    LOAD(NewDict);
    LOAD(GetDict);
    LOAD(SetDict);
    LOAD(DeleteDict);
    LOAD(LoadDict);

    return 0;
}

void eci_runtime_close(void) {
    /* See file header: we deliberately leak the dlopen handle. */
    g_handle = NULL;
}
