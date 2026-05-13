/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/config.h"
#include "../src/eci/eci.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    EloqConfig c;
    config_defaults(&c);

    /* Defaults. */
    assert(c.debug == 0);
    assert(strcmp(c.data_dir, "/usr/lib/eloquence") == 0);
    assert(c.default_sample_rate == 1);
    assert(c.default_voice_slot == 0);
    assert(c.default_language == eciGeneralAmericanEnglish);
    assert(c.use_dictionaries == 1);
    assert(c.backquote_tags == 0);

    /* Single key=value updates. */
    assert(config_apply_kv(&c, "Debug", "1") == 0 && c.debug == 1);
    assert(config_apply_kv(&c, "EloquenceDataDir", "/tmp/x") == 0);
    assert(strcmp(c.data_dir, "/tmp/x") == 0);
    assert(config_apply_kv(&c, "EloquenceDefaultVoice", "Shelley") == 0);
    assert(c.default_voice_slot == 1);
    assert(config_apply_kv(&c, "EloquenceDefaultLanguage", "de-DE") == 0);
    assert(c.default_language == eciStandardGerman);
    assert(config_apply_kv(&c, "EloquenceSampleRate", "1") == 0);

    /* Invalid values rejected. */
    assert(config_apply_kv(&c, "EloquenceSampleRate", "9") == -1);
    assert(config_apply_kv(&c, "EloquenceDefaultVoice", "Nobody") == -1);
    assert(config_apply_kv(&c, "EloquenceDefaultLanguage", "xx-XX") == -1);
    assert(config_apply_kv(&c, "Nonsense", "yes") == -1);

    /* Effective dict dir. */
    config_defaults(&c);
    assert(strcmp(config_effective_dict_dir(&c), "/usr/lib/eloquence/dicts") == 0);
    config_apply_kv(&c, "EloquenceDictionaryDir", "/var/eloq");
    assert(strcmp(config_effective_dict_dir(&c), "/var/eloq") == 0);

    /* File parsing -- write temp conf, parse, check. */
    const char *tmp = "/tmp/test_config_eloq.conf";
    FILE *f = fopen(tmp, "w");
    fprintf(f, "# comment\n\n");
    fprintf(f, "Debug 1\n");
    fprintf(f, "EloquenceBackquoteTags 1\n");
    fprintf(f, "EloquencePhrasePrediction 1\n");
    fprintf(f, "BogusKey something\n");  /* logged but not fatal */
    fclose(f);
    config_defaults(&c);
    assert(config_parse_file(&c, tmp) == 0);
    assert(c.debug == 1);
    assert(c.backquote_tags == 1);
    assert(c.phrase_prediction == 1);

    /* Missing file is OK. */
    config_defaults(&c);
    assert(config_parse_file(&c, "/nonexistent.conf") == 0);

    puts("test_config: OK");
    return 0;
}
