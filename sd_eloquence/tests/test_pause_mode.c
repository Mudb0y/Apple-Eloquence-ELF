/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/synth/pause_mode.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(const char *src, const char *expected) {
    char *out = pause_mode_rewrite(src);
    if (strcmp(out, expected) != 0) {
        fprintf(stderr, "pause_mode_rewrite mismatch\n  input:    %s\n  output:   %s\n  expected: %s\n",
                src, out, expected);
        free(out);
        exit(1);
    }
    free(out);
}

int main(void) {
#ifdef HAVE_PCRE2
    /* Comma followed by a space + word: rewrite inserts " `p1" before the
     * comma. */
    check("Hello, world", "Hello `p1, world");

    /* Sentence end + sentence end: both end-of-line periods get rewritten. */
    check("First. Second.", "First `p1. Second `p1.");

    /* No trailing space and no end-of-input punctuation: nothing fires. */
    check("a,b", "a,b");

    /* A run of identical punctuation: group 3 captures the repeats so they
     * stay glued together. */
    check("Wait... done", "Wait `p1... done");

    /* Empty + plain-text inputs survive unchanged. */
    check("", "");
    check("just words", "just words");

    /* Backquote tags in the input are not punctuation, so they pass
     * through untouched. */
    check("hi `pp1 there", "hi `pp1 there");
#else
    /* Without pcre2 the function is identity. */
    check("Hello, world", "Hello, world");
#endif

    puts("test_pause_mode: OK");
    return 0;
}
