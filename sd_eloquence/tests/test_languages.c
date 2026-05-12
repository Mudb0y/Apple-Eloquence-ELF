/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/eci/languages.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    /* By dialect. */
    const LangEntry *en = lang_by_dialect(eciGeneralAmericanEnglish);
    assert(en);
    assert(strcmp(en->iso_lang, "en") == 0);
    assert(strcmp(en->iso_variant, "us") == 0);
    assert(strcmp(en->langid, "enu") == 0);
    assert(lang_by_dialect(0xDEADBEEF) == NULL);

    /* By IETF tag -- exact match wins. */
    const LangEntry *ja = lang_by_iso("ja-JP");
    assert(ja && ja->eci_dialect == eciStandardJapanese);
    assert(lang_by_iso("en-US") == en);
    assert(lang_by_iso("en_us") == en);

    /* Bare-lang prefix falls back to the first matching region. */
    assert(lang_by_iso("en") && lang_by_iso("en")->iso_lang[0] == 'e');

    /* Index. */
    assert(lang_index(en) == 0);
    assert(lang_index(NULL) == -1);

    /* Encoding. */
    assert(strcmp(lang_encoding_for(eciGeneralAmericanEnglish), "cp1252") == 0);
    assert(strcmp(lang_encoding_for(eciMandarinChinese),        "gb18030") == 0);
    assert(strcmp(lang_encoding_for(eciStandardJapanese),       "cp932") == 0);
    assert(strcmp(lang_encoding_for(eciStandardKorean),         "cp949") == 0);

    puts("test_languages: OK");
    return 0;
}
