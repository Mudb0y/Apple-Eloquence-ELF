/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/synth/marks.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    marks_init();

    /* Register + resolve. */
    uint32_t a = marks_register("hello", 1);
    uint32_t b = marks_register("world", 1);
    assert(marks_job_of(a) == 1);
    assert(marks_job_of(b) == 1);
    assert(marks_idx_of(a) == 0);
    assert(marks_idx_of(b) == 1);

    const char *r = marks_resolve(a);
    assert(r && strcmp(r, "hello") == 0);
    /* Resolution consumes; second time is NULL. */
    assert(marks_resolve(a) == NULL);

    /* End sentinel for job 1. */
    uint32_t end1 = marks_make_end(1);
    assert(marks_idx_of(end1) == END_STRING_ID);

    /* Different job, same idx -- different id. */
    uint32_t c = marks_register("hi-from-2", 2);
    assert(marks_job_of(c) == 2);
    assert(marks_idx_of(c) == 0);

    /* Stale: a was job 1; a different encoded id with job 2 doesn't resolve to job 1's entries. */
    uint32_t fake = (3u << 16) | 0;
    assert(marks_resolve(fake) == NULL);

    /* Release job 1; its marks vanish. */
    marks_release_job(1);
    assert(marks_resolve(b) == NULL);
    /* But job 2's mark still resolves. */
    const char *cr = marks_resolve(c);
    assert(cr && strcmp(cr, "hi-from-2") == 0);

    puts("test_marks: OK");
    return 0;
}
