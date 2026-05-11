/*
 * eci_runtime.c -- runtime loader for ECI Eloquence (eci.so + language dylibs)
 *
 * dlopen()s the eci.so library and populates a function-pointer table so the
 * rest of the module can call the engine without link-time dependencies on it.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 * SPDX-License-Identifier: MIT
 */

#include "eci.h"

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

    /* Resolve every entry point. If any are missing, the engine is
     * incompatible and we bail out. */
    LOAD(New);
    LOAD(NewEx);
    LOAD(Delete);
    LOAD(Version);
    LOAD(ProgStatus);
    LOAD(ErrorMessage);
    LOAD(Speaking);
    LOAD(Stop);
    LOAD(Reset);
    LOAD(Synchronize);
    LOAD(Synthesize);
    LOAD(AddText);
    LOAD(ClearInput);
    LOAD(Pause);
    LOAD(InsertIndex);
    LOAD(GetParam);
    LOAD(SetParam);
    LOAD(GetVoiceParam);
    LOAD(SetVoiceParam);
    LOAD(GetDefaultParam);
    LOAD(SetDefaultParam);
    LOAD(GetVoiceName);
    LOAD(SetVoiceName);
    LOAD(CopyVoice);
    LOAD(RegisterCallback);
    LOAD(SetOutputBuffer);
    LOAD(SetOutputDevice);
    LOAD(SetOutputFilename);

    return 0;
}

void eci_runtime_close(void) {
    /* The ECI engine registers global C++ destructors on dlopen; calling
     * dlclose can run them in an order that crashes inside libc atexit
     * (observed on Apple's eci.dylib on Linux). Leave the library mapped
     * until process exit. */
    g_handle = NULL;
}
