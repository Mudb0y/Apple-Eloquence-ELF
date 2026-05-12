/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * test_voices -- preset-table lookup tests (no engine needed).
 */
#include "../src/eci/voices.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    /* English: slot 0 is Reed. */
    assert(strcmp(voice_display_name(0, "en"), "Reed") == 0);
    /* French: slot 0 is Jacques. */
    assert(strcmp(voice_display_name(0, "fr"), "Jacques") == 0);
    /* Slot 1 has no French override -- stays Shelley. */
    assert(strcmp(voice_display_name(1, "fr"), "Shelley") == 0);
    /* Out of range -> NULL. */
    assert(voice_display_name(-1, "en") == NULL);
    assert(voice_display_name(N_VOICE_PRESETS, "en") == NULL);

    /* Name lookup. */
    assert(voice_find_by_name("Reed")    == 0);
    assert(voice_find_by_name("reed")    == 0);
    assert(voice_find_by_name("Jacques") == 0);  /* French alias of Reed */
    assert(voice_find_by_name("Eddy")    == 7);
    assert(voice_find_by_name("Nope")    == -1);
    assert(voice_find_by_name(NULL)      == -1);

    /* Voice-type lookup. */
    assert(voice_find_by_voice_type("MALE1")   == 0);
    assert(voice_find_by_voice_type("FEMALE3") == 5);
    assert(voice_find_by_voice_type("CHILD_MALE")   == 2);
    assert(voice_find_by_voice_type("CHILD_FEMALE") == 2);
    assert(voice_find_by_voice_type("ROBOT")   == -1);
    assert(voice_find_by_voice_type(NULL)      == -1);

    puts("test_voices: OK");
    return 0;
}
