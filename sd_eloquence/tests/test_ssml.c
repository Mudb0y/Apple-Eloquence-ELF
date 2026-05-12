/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/ssml/ssml.h"
#include "../src/synth/marks.h"
#include "../src/eci/eci.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

static int count(synth_job *j, synth_frame_kind k) {
    int n = 0;
    for (size_t i = 0; i < j->n_frames; i++) if (j->frames[i].kind == k) n++;
    return n;
}

int main(void) {
    marks_init();

    /* Plain text fast-path. */
    {
        const char *t = "Hello world";
        synth_job *j = ssml_parse(t, strlen(t), SPD_MSGTYPE_TEXT,
                                  eciGeneralAmericanEnglish, 1);
        assert(j && j->n_frames == 1);
        assert(j->frames[0].kind == FRAME_TEXT);
        assert(strcmp(j->frames[0].u.text.text, "Hello world") == 0);
        synth_job_free(j);
    }

    /* CHAR mode: TEXTMODE(2) wrapper. */
    {
        const char *t = "A";
        synth_job *j = ssml_parse(t, 1, SPD_MSGTYPE_CHAR, eciGeneralAmericanEnglish, 2);
        assert(j && j->n_frames == 3);
        assert(j->frames[0].kind == FRAME_TEXTMODE);
        assert(j->frames[0].u.textmode.mode == 2);
        assert(j->frames[1].kind == FRAME_TEXT);
        assert(j->frames[2].kind == FRAME_TEXTMODE);
        assert(j->frames[2].u.textmode.mode == 0);
        synth_job_free(j);
    }

    /* SSML with one mark. */
    {
        const char *t = "<speak>Hi <mark name=\"m1\"/> there</speak>";
        synth_job *j = ssml_parse(t, strlen(t), SPD_MSGTYPE_TEXT,
                                  eciGeneralAmericanEnglish, 3);
        assert(j);
        assert(count(j, FRAME_MARK) == 1);
        assert(count(j, FRAME_TEXT) >= 1);
        synth_job_free(j);
    }

    /* SSML with break. */
    {
        const char *t = "<speak>Hi<break time=\"500ms\"/>there</speak>";
        synth_job *j = ssml_parse(t, strlen(t), SPD_MSGTYPE_TEXT,
                                  eciGeneralAmericanEnglish, 4);
        assert(j);
        int found = 0;
        for (size_t i = 0; i < j->n_frames; i++) {
            if (j->frames[i].kind == FRAME_BREAK) {
                assert(j->frames[i].u.brk.millis == 500);
                found = 1; break;
            }
        }
        assert(found);
        synth_job_free(j);
    }

    /* SSML with prosody rate=80%. */
    {
        const char *t = "<speak><prosody rate=\"80%\">fast text</prosody></speak>";
        synth_job *j = ssml_parse(t, strlen(t), SPD_MSGTYPE_TEXT,
                                  eciGeneralAmericanEnglish, 5);
        assert(j);
        assert(count(j, FRAME_PROSODY_PUSH) == 1);
        assert(count(j, FRAME_PROSODY_POP)  == 1);
        synth_job_free(j);
    }

    /* SSML with <lang xml:lang="de-DE">. */
    {
        const char *t = "<speak>hi <lang xml:lang=\"de-DE\">guten tag</lang></speak>";
        synth_job *j = ssml_parse(t, strlen(t), SPD_MSGTYPE_TEXT,
                                  eciGeneralAmericanEnglish, 6);
        assert(j);
        int found = 0;
        for (size_t i = 0; i < j->n_frames; i++) {
            if (j->frames[i].kind == FRAME_LANG_PUSH &&
                j->frames[i].u.lang.dialect == eciStandardGerman) {
                found = 1; break;
            }
        }
        assert(found);
        synth_job_free(j);
    }

    /* SSML with malformed XML -> plain-text fallback. */
    {
        const char *t = "<speak>unclosed";
        synth_job *j = ssml_parse(t, strlen(t), SPD_MSGTYPE_TEXT,
                                  eciGeneralAmericanEnglish, 7);
        assert(j);
        /* Plain text fallback puts the raw input as the only TEXT frame. */
        assert(j->n_frames == 1);
        assert(j->frames[0].kind == FRAME_TEXT);
        synth_job_free(j);
    }

    puts("test_ssml: OK");
    return 0;
}
