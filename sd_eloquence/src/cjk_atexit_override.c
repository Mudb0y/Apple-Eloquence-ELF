/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * cjk_atexit_override.c -- in-module __cxa_atexit override.
 *
 * Apple's converted romanizer .so files (chsrom / chtrom / jpnrom) register
 * C++ static destructors via __cxa_atexit using bad function pointers: unslid
 * Mach-O virtual addresses (chs/cht — destination falls in .m2e_cstring) or
 * pointers into freed anonymous heap regions (jpn). At process exit, libc
 * dereferences those pointers, which are invalid → SIGSEGV. korrom.so does
 * not register any destructors and exits cleanly. Background:
 *
 *   docs/cjk-investigation/2026-05-12-phase0-findings.md
 *   docs/cjk-investigation/2026-05-12-phase1-atexit-shim.md
 *
 * This file is linked into sd_eloquence; CMake passes -rdynamic so the
 * dynamic linker resolves chsrom/cht/jpnrom's __cxa_atexit references (and
 * DT_FINI_ARRAY registrations applied by ld-linux on their behalf) to OUR
 * override instead of libc's. The override forwards every legitimate
 * registration to libc's real __cxa_atexit; it only suppresses the ones we
 * know are bogus, via three detection strategies:
 *
 *   1. dladdr(return_address) -- direct calls from inside the converted
 *      romanizers' code.
 *   2. dladdr(func) -- DT_FINI_ARRAY entries applied by ld-linux, whose
 *      apparent caller is ld-linux itself but whose func pointer falls
 *      inside a suspect .so.
 *   3. Unslid-static-VA heuristic (func < 0x200000) -- catches chs/cht where
 *      the func pointer is a raw Mach-O VA that doesn't dladdr-resolve at
 *      all (the pointer doesn't fall in any mapped region).
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef int (*cxa_atexit_t)(void (*func)(void *), void *arg, void *dso_handle);
static cxa_atexit_t real_cxa_atexit = NULL;

static int matches_suspect(const char *path) {
    if (!path) return 0;
    return strstr(path, "chsrom.so") != NULL
        || strstr(path, "chtrom.so") != NULL
        || strstr(path, "jpnrom.so") != NULL
        || strstr(path, "korrom.so") != NULL;
}

int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle) {
    if (!real_cxa_atexit)
        real_cxa_atexit = (cxa_atexit_t)dlsym(RTLD_NEXT, "__cxa_atexit");

    Dl_info info;
    const char *source = NULL;

    void *ret_addr = __builtin_return_address(0);
    if (dladdr(ret_addr, &info) && matches_suspect(info.dli_fname))
        source = info.dli_fname;

    if (!source && dladdr((void *)func, &info) && matches_suspect(info.dli_fname))
        source = info.dli_fname;

    if (!source && (uintptr_t)func < 0x200000)
        source = "<unslid-static-VA>";

    if (source) {
        fprintf(stderr, "sd_eloquence: suppressed __cxa_atexit from %s "
                "(func=%p arg=%p dso=%p)\n",
                source, (void *)func, arg, dso_handle);
        return 0;
    }

    if (real_cxa_atexit)
        return real_cxa_atexit(func, arg, dso_handle);
    return -1;
}
