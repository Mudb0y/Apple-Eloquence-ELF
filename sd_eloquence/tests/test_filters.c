/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/filters/filters.h"
#include "../src/eci/eci.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static void check(const char *in, const char *want, int dialect, const char *label) {
    char *out = filters_apply(in, dialect);
    if (strcmp(out, want) != 0) {
        fprintf(stderr, "%s: got '%s' want '%s'\n", label, out, want);
        free(out);
        abort();
    }
    free(out);
}

int main(void) {
#ifdef HAVE_PCRE2
    /* Global: bracket double-space collapse. */
    check("hello  ) world", "hello) world", eciGeneralAmericanEnglish, "global-bracket");
    /* Space before punctuation. */
    check("words .", "words.", eciGeneralAmericanEnglish, "global-space-punc");
    /* E3: English Mc rule (IBM). */
    check("Mc Donald", "McDonald", eciGeneralAmericanEnglish, "en-mc-rule");
#endif
    /* Filters always preserve identity on pass-through. */
    char *out = filters_apply("plain text", eciGeneralAmericanEnglish);
    assert(strstr(out, "plain text") != NULL);
    free(out);
    puts("test_filters: OK");
    return 0;
}
