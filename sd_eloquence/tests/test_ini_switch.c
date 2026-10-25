/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Regression test for Apple's ini-parser section-skipping bug: on a stock
 * (unpatched) engine the language table built from eci.ini only registers
 * every other section, so eciSetParam(eciLanguageDialect) refuses to switch
 * to the dropped languages and speech silently stays in the initial dialect.
 * See tools/patch_ini_parser.py and docs/05-troubleshooting.md.
 *
 * Needs real engine artefacts; set both to run, otherwise the test skips:
 *   ECI_SO       path to the converted eci.so
 *   ECI_INI_DIR  directory holding eci.ini and the language .so files
 * (engine reads eci.ini from the process cwd, hence the chdir).
 */
#include "../src/eci/languages.h"

#include <unistd.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *eciNewEx_fn(int);
typedef int eciSetParam_fn(void *, int, int);
typedef int eciGetParam_fn(void *, int);
typedef void eciDelete_fn(void *);

#define ECI_LANGUAGE_DIALECT 9
#define DIALECT_EN_US 0x00010000

int main(void) {
    const char *so_path = getenv("ECI_SO");
    const char *ini_dir = getenv("ECI_INI_DIR");
    if (!so_path || !ini_dir) {
        puts("test_ini_switch: SKIP (ECI_SO / ECI_INI_DIR not set)");
        return 77;
    }
    if (chdir(ini_dir) != 0) {
        printf("test_ini_switch: FAIL chdir(%s)\n", ini_dir);
        return 1;
    }

    void *h = dlopen(so_path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        printf("test_ini_switch: FAIL dlopen(%s): %s\n", so_path, dlerror());
        return 1;
    }
    eciNewEx_fn *eciNewEx = dlsym(h, "eciNewEx");
    eciSetParam_fn *eciSetParam = dlsym(h, "eciSetParam");
    eciGetParam_fn *eciGetParam = dlsym(h, "eciGetParam");
    eciDelete_fn *eciDelete = dlsym(h, "eciDelete");
    if (!eciNewEx || !eciSetParam || !eciGetParam || !eciDelete) {
        puts("test_ini_switch: FAIL missing ECI symbols");
        return 1;
    }

    int failures = 0, checked = 0;
    for (int i = 0; i < N_LANGS; i++) {
        const LangEntry *L = &g_langs[i];
        /* Same gate as module_init: CJK dialects are not wired up in v1. */
        if (strcmp(L->so_name, "jpn.so") == 0 || strcmp(L->so_name, "kor.so") == 0 ||
            strcmp(L->so_name, "chs.so") == 0 || strcmp(L->so_name, "cht.so") == 0)
            continue;

        void *e = eciNewEx(DIALECT_EN_US);
        if (!e) {
            printf("test_ini_switch: FAIL eciNewEx(en-US) returned NULL\n");
            return 1;
        }
        eciSetParam(e, ECI_LANGUAGE_DIALECT, L->eci_dialect);
        int now = eciGetParam(e, ECI_LANGUAGE_DIALECT);
        checked++;
        if (now != L->eci_dialect) {
            printf("test_ini_switch: FAIL %s: engine stuck on %#x "
                   "(want %#x) -- unpatched engine or incomplete eci.ini?\n",
                   L->human, now, L->eci_dialect);
            failures++;
        }
        eciDelete(e);
    }

    if (failures) {
        printf("test_ini_switch: %d/%d dialects refused\n", failures, checked);
        return 1;
    }
    printf("test_ini_switch: OK (%d dialects switch)\n", checked);
    return 0;
}
