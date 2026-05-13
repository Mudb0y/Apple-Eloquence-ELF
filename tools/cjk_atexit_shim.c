/* SPDX-License-Identifier: MIT
 *
 * cjk_atexit_shim.c -- LD_PRELOAD shim that suppresses __cxa_atexit
 * registrations originating from the converted romanizer .so files
 * (chsrom.so / chtrom.so / jpnrom.so / korrom.so).
 *
 * Used to confirm CJK Phase 0's hypothesis: that the only thing crashing
 * at exit is bogus destructor pointers registered by those .so's C++
 * static initializers via __cxa_atexit. Build via:
 *
 *   gcc -shared -fPIC -ldl -o cjk_atexit_shim.so tools/cjk_atexit_shim.c
 *
 * Then:
 *
 *   LD_PRELOAD=$PWD/cjk_atexit_shim.so build/examples/cjk_probe /usr/lib/eloquence/eci.so 0x00060000
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
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

    /*
     * Two interception strategies, applied in order:
     *
     * 1. Caller-based: the __cxa_atexit call originates directly from a suspect
     *    .so (covers explicit registrations from C++ static initializers).
     *
     * 2. Func-based: the function pointer itself resolves to a suspect .so, even
     *    when the immediate caller is ld-linux (the dynamic linker registers
     *    .fini_array/.DT_FINI_ARRAY entries via __cxa_atexit on behalf of the
     *    .so being dlopen'd — those pass through ld-linux as caller but carry a
     *    func pointer that lives inside the suspect .so).
     */
    Dl_info info;
    const char *source = NULL;

    /* Strategy 1: check the call-site return address */
    void *ret_addr = __builtin_return_address(0);
    if (dladdr(ret_addr, &info) && matches_suspect(info.dli_fname))
        source = info.dli_fname;

    /* Strategy 2: check where func itself lives */
    if (!source && dladdr((void *)func, &info) && matches_suspect(info.dli_fname))
        source = info.dli_fname;

    /* Strategy 3: func pointer is an unslid static VA (< 0x200000) — no valid
     * mapping exists; treat any such registration as suspect regardless of
     * caller, since legitimate funcs always have a high canonical VA. */
    if (!source && (uintptr_t)func < 0x200000)
        source = "<unslid-static-VA>";

    if (source) {
        fprintf(stderr, "cjk_atexit_shim: suppressed __cxa_atexit from %s "
                "(func=%p arg=%p dso=%p)\n",
                source, (void *)func, arg, dso_handle);
        return 0;  /* claim success, no real registration */
    }

    if (real_cxa_atexit)
        return real_cxa_atexit(func, arg, dso_handle);
    return -1;
}
