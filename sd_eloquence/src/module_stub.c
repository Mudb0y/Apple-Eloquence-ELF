/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 *
 * Temporary stub so the `sd_eloquence` CMake target exists during the rewrite.
 * Replaced by module.c in Task G3.
 */
#include <unistd.h>
#include <speech-dispatcher/spd_module_main.h>

int module_init(char **msg) { (void)msg; return -1; }
int module_close(void)      { return 0; }
int module_config(const char *configfile) { (void)configfile; return 0; }
int module_audio_init(char **s) { if (s) *s = NULL; return 0; }
int module_audio_set(const char *v, const char *l) { (void)v; (void)l; return 0; }
int module_set(const char *v, const char *l)       { (void)v; (void)l; return 0; }
int module_loglevel_set(const char *v, const char *l) { (void)v; (void)l; return 0; }
int module_debug(int e, const char *f) { (void)e; (void)f; return 0; }
SPDVoice **module_list_voices(void) { return NULL; }
int  module_speak(char *d, size_t b, SPDMessageType t) { (void)d; (void)b; (void)t; return -1; }
void module_speak_sync(const char *d, size_t b, SPDMessageType t) { (void)d; (void)b; (void)t; }
void module_speak_begin(void) {}
void module_speak_end(void)   {}
void module_speak_pause(void) {}
void module_speak_stop(void)  {}
int  module_stop(void)        { return 0; }
size_t module_pause(void)     { return 0; }
int  module_loop(void)        { return module_process(STDIN_FILENO, 1); }
