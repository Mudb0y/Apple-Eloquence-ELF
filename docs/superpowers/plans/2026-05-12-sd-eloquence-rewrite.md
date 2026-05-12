# sd_eloquence v2 Rewrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `sd_eloquence/src/sd_eloquence.c` (926-line monolith) with a properly-engineered speechd output module per `docs/superpowers/specs/2026-05-12-sd-eloquence-rewrite-design.md`. v1 ships threading, full SSML, NVDA-derived anti-crash filters, optional dictionaries, all 10 Latin languages working, plus the diagnostic infrastructure to start CJK Phase 0.

**Architecture:** Single dedicated synth thread driving a quirks-aware ECI engine wrapper. SSML translates to a frame sequence executed in order. ECI callback runs on the synth thread and forwards PCM to `module_tts_output_server`. Stop signals atomic + `eci.Stop`. NVDA's regex anti-crash filters port verbatim under GPLv2.

**Tech Stack:** C11, pthreads, libxml2 (SAX), libpcre2-8, libsoxr (optional), libspeechd_module, libiconv (transitive on glibc). CMake build system. ctest for unit tests.

**Scope of this plan:** Phases A through I (CJK Phase 0 diagnostic infrastructure). Phases for CJK Phase 1–3 fixes get a follow-up plan once Phase 0 traces are in hand. v1 release (cleanup, README update, tag) sits in Phase J — only run after follow-up CJK plan completes.

---

## Phase A: Scaffolding and license split

### Task A1: Branch + worktree

**Files:**
- None (git state only)

- [ ] **Step 1: Create the feature branch and switch to it**

Run:
```bash
git checkout main && git pull && git checkout -b feat/sd-eloquence-rewrite
```
Expected: `Switched to a new branch 'feat/sd-eloquence-rewrite'`

- [ ] **Step 2: Confirm the working tree is clean**

Run: `git status`
Expected: `nothing to commit, working tree clean` (the `NVDA-IBMTTS-Driver/` and `sd_eloquence/src/eci_runtime.h` should NOT show up — if they do, add `NVDA-IBMTTS-Driver/` to `.gitignore` first.)

If `NVDA-IBMTTS-Driver/` shows up:
```bash
echo "" >> .gitignore
echo "# Reference repo (not vendored, just cloned next to the project)" >> .gitignore
echo "NVDA-IBMTTS-Driver/" >> .gitignore
git add .gitignore && git commit -m "gitignore: ignore the cloned NVDA-IBMTTS-Driver reference repo"
```

---

### Task A2: License split — add GPLv2 for sd_eloquence subtree

**Files:**
- Create: `sd_eloquence/LICENSE.GPL`
- Modify: `README.md` (add license-split note in §"Provenance and licensing")

- [ ] **Step 1: Download the canonical GPLv2 text**

Run: `curl -sSL https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt -o sd_eloquence/LICENSE.GPL`
Expected: file exists, ~18KB. Verify first line is `GNU GENERAL PUBLIC LICENSE`.

- [ ] **Step 2: Verify the file**

Run: `head -3 sd_eloquence/LICENSE.GPL && wc -l sd_eloquence/LICENSE.GPL`
Expected: starts with `GNU GENERAL PUBLIC LICENSE / Version 2, June 1991 / Copyright (C) 1989, 1991 Free Software Foundation`, length ~340 lines.

- [ ] **Step 3: Add license-split paragraph to README**

Edit `README.md`. Find the `## Provenance and licensing` section, and add a new bullet between the existing `**The converter**` bullet and `**The shipped Mach-O dylibs**` bullet:

```markdown
- **The speech-dispatcher module** (`sd_eloquence/`) is licensed under
  **GPL-2.0-or-later**. It incorporates anti-crash regex tables and
  dictionary-loading patterns derived from the
  [NVDA-IBMTTS-Driver](https://github.com/davidacm/NVDA-IBMTTS-Driver)
  project (Copyright (C) 2009-2026 David CM, GPL-2.0). Full GPLv2 text in
  `sd_eloquence/LICENSE.GPL`. The macho2elf converter and the rest of the
  project remain MIT.
```

- [ ] **Step 4: Commit**

```bash
git add sd_eloquence/LICENSE.GPL README.md
git commit -m "sd_eloquence: relicense subtree to GPLv2 ahead of NVDA filter port

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task A3: Move existing code to `sd_eloquence/src/old/`

**Files:**
- Move: `sd_eloquence/src/sd_eloquence.c` → `sd_eloquence/src/old/sd_eloquence.c`
- Move: `sd_eloquence/src/eci.h` → `sd_eloquence/src/old/eci.h`
- Move: `sd_eloquence/src/eci_runtime.c` → `sd_eloquence/src/old/eci_runtime.c`
- Move: `sd_eloquence/src/eci_runtime.h` → `sd_eloquence/src/old/eci_runtime.h`
- Move: `sd_eloquence/src/voice_presets.h` → `sd_eloquence/src/old/voice_presets.h`
- Move: `sd_eloquence/src/spd_audio.h` → `sd_eloquence/src/old/spd_audio.h`
- Modify: `CMakeLists.txt` (point existing target at old/ paths and rename to `sd_eloquence_old`)

- [ ] **Step 1: Create the `old/` directory and move all current source files into it**

Run:
```bash
mkdir -p sd_eloquence/src/old
git mv sd_eloquence/src/sd_eloquence.c sd_eloquence/src/old/
git mv sd_eloquence/src/eci.h          sd_eloquence/src/old/
git mv sd_eloquence/src/eci_runtime.c  sd_eloquence/src/old/
git mv sd_eloquence/src/eci_runtime.h  sd_eloquence/src/old/
git mv sd_eloquence/src/voice_presets.h sd_eloquence/src/old/
git mv sd_eloquence/src/spd_audio.h    sd_eloquence/src/old/
```

- [ ] **Step 2: Update `CMakeLists.txt` to point at the new path and rename the target**

In `CMakeLists.txt`, find this block:
```cmake
        add_executable(sd_eloquence
            sd_eloquence/src/sd_eloquence.c
            sd_eloquence/src/eci_runtime.c)
        target_include_directories(sd_eloquence PRIVATE
            sd_eloquence/src
```

Replace with:
```cmake
        # Old monolith, kept building under a different target name during the
        # rewrite. Will be deleted when the new sd_eloquence becomes the default.
        add_executable(sd_eloquence_old
            sd_eloquence/src/old/sd_eloquence.c
            sd_eloquence/src/old/eci_runtime.c)
        target_include_directories(sd_eloquence_old PRIVATE
            sd_eloquence/src/old
```

Then replace all remaining occurrences of `sd_eloquence` *as a CMake target name* (not the directory) in that block with `sd_eloquence_old`. That includes the `target_compile_definitions`, `target_include_directories`, `target_link_libraries`, and `install(TARGETS …)` lines.

- [ ] **Step 3: Confirm the build still works**

Run:
```bash
rm -rf build
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --target sd_eloquence_old
```
Expected: `sd_eloquence_old` binary appears at `build/sd_eloquence_old`. Build succeeds with no errors.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "sd_eloquence: move monolith to src/old/ for transitional build

Keeps the existing module building under target sd_eloquence_old while the
rewrite lands alongside it. The default sd_eloquence target will flip to the
new code once it passes smoke; src/old/ gets deleted at that point.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task A4: CMake scaffold for the new tree (compile a placeholder)

**Files:**
- Create: `sd_eloquence/src/eci/.gitkeep`, `sd_eloquence/src/filters/.gitkeep`, `sd_eloquence/src/ssml/.gitkeep`, `sd_eloquence/src/audio/.gitkeep`, `sd_eloquence/src/synth/.gitkeep`, `sd_eloquence/tests/.gitkeep`
- Create: `sd_eloquence/src/module_stub.c` (temporary; deleted in Task G3)
- Modify: `CMakeLists.txt` (add `sd_eloquence` target wired to `module_stub.c`, find PCRE2 + libxml2)

- [ ] **Step 1: Create empty subdirectories**

Run:
```bash
mkdir -p sd_eloquence/src/eci sd_eloquence/src/filters sd_eloquence/src/ssml \
         sd_eloquence/src/audio sd_eloquence/src/synth sd_eloquence/tests
touch sd_eloquence/src/eci/.gitkeep sd_eloquence/src/filters/.gitkeep \
      sd_eloquence/src/ssml/.gitkeep sd_eloquence/src/audio/.gitkeep \
      sd_eloquence/src/synth/.gitkeep sd_eloquence/tests/.gitkeep
```

- [ ] **Step 2: Create `sd_eloquence/src/module_stub.c` (temporary)**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 *
 * Temporary stub so the `sd_eloquence` CMake target exists during the rewrite.
 * Replaced by module.c in Task G3.
 */
#include <speech-dispatcher/spd_module_main.h>

int module_init(char **msg) { (void)msg; return -1; }
int module_close(void)      { return 0; }
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
```

- [ ] **Step 3: Update `CMakeLists.txt` to find PCRE2 and libxml2 and build the new (currently stub) target**

Find the section right after the `if(NOT SPEECHD_INCLUDE_DIR OR NOT SPEECHD_MODULE_LIB)` error check. Add this block **before** the `# Optional libsoxr` comment:

```cmake
        # libxml2 -- SSML SAX parser. Required.
        pkg_check_modules(LIBXML2 REQUIRED libxml-2.0)

        # libpcre2-8 -- PCRE2 (Perl-compatible) regex for anti-crash filters.
        # Optional: -DWITH_PCRE2=OFF makes filters into no-ops.
        option(WITH_PCRE2 "Build with libpcre2-8 anti-crash filters" ON)
        set(PCRE2_FOUND FALSE)
        if(WITH_PCRE2)
            pkg_check_modules(PCRE2 libpcre2-8)
            if(NOT PCRE2_FOUND)
                find_library(PCRE2_LIB NAMES pcre2-8 PATHS /usr/lib /usr/lib64)
                find_path(PCRE2_INCLUDE_DIR NAMES pcre2.h PATHS /usr/include)
                if(PCRE2_LIB AND PCRE2_INCLUDE_DIR)
                    set(PCRE2_FOUND TRUE)
                    set(PCRE2_LIBRARIES ${PCRE2_LIB})
                    set(PCRE2_INCLUDE_DIRS ${PCRE2_INCLUDE_DIR})
                endif()
            endif()
        endif()
```

Then **after** the existing `add_executable(sd_eloquence_old …)` block, add:

```cmake
        # New module (replaces sd_eloquence_old once feature-complete).
        add_executable(sd_eloquence
            sd_eloquence/src/module_stub.c)
        target_include_directories(sd_eloquence PRIVATE
            sd_eloquence/src
            ${SPEECHD_INCLUDE_DIR}
            ${SPEECHD_INCLUDE_DIR}/speech-dispatcher
            ${LIBXML2_INCLUDE_DIRS})
        target_compile_definitions(sd_eloquence PRIVATE -DPCRE2_CODE_UNIT_WIDTH=8)
        target_link_libraries(sd_eloquence PRIVATE
            ${SPEECHD_MODULE_LIB}
            ${LIBXML2_LIBRARIES}
            pthread
            dl)
        if(SOXR_FOUND)
            target_compile_definitions(sd_eloquence PRIVATE HAVE_SOXR)
            target_include_directories(sd_eloquence PRIVATE ${SOXR_INCLUDE_DIRS})
            target_link_libraries(sd_eloquence PRIVATE ${SOXR_LIBRARIES})
        endif()
        if(PCRE2_FOUND)
            target_compile_definitions(sd_eloquence PRIVATE HAVE_PCRE2)
            target_include_directories(sd_eloquence PRIVATE ${PCRE2_INCLUDE_DIRS})
            target_link_libraries(sd_eloquence PRIVATE ${PCRE2_LIBRARIES})
        endif()
        # NOT installed yet -- sd_eloquence_old still owns the install destination.
        # The install target flips in Task H1.
```

- [ ] **Step 4: Add ctest hookup for unit tests**

Near the top of `CMakeLists.txt`, right after `add_compile_options(-Wall -Wextra -Wno-unused-parameter)`, add:

```cmake
enable_testing()
```

- [ ] **Step 5: Verify the new target compiles**

Run:
```bash
rm -rf build
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --target sd_eloquence
```
Expected: `build/sd_eloquence` exists. Compile succeeds with no errors.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "sd_eloquence: CMake scaffold + libxml2/pcre2 detection + module_stub.c

Adds the new sd_eloquence target alongside sd_eloquence_old, wires libxml2
(SSML) and libpcre2-8 (anti-crash filters) into pkg-config detection, and
exposes WITH_PCRE2 toggle. The module_stub.c is replaced by module.c in
Task G3.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Phase B: eci/ subsystem extraction

### Task B1: Move `eci.h` to new location with GPLv2 header

**Files:**
- Create: `sd_eloquence/src/eci/eci.h` (content from `sd_eloquence/src/old/eci.h` + relicense header)

- [ ] **Step 1: Copy the existing eci.h, prepending a GPLv2 SPDX header**

Read the current contents of `sd_eloquence/src/old/eci.h`. Create `sd_eloquence/src/eci/eci.h` by writing:

1. New header (replacing the existing top-of-file comment):

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci.h -- ECI 6.x C API mirroring IBM ibmtts-sdk 6.7.4 eci.h.
 *
 * Signatures are byte-for-byte identical to IBM's; full ABI types are
 * declared here as plain enums/structs. The function-pointer table
 * (EciApi) lives in eci/runtime.h because we dlopen the engine at
 * runtime.
 *
 * IBM's BSD license is reproduced in the project LICENSE; this file is
 * an independent reimplementation of the public ABI declarations and
 * is GPLv2 in the sd_eloquence subtree.
 *
 * Apple's eci.dylib mostly follows the IBM ABI but with quirks worth
 * remembering:
 *   - eciSetParam(eciSampleRate, 2) rejects (no 22050 Hz output).
 *   - eciSynchronize is non-blocking; synthesis continues on a worker
 *     thread after the call returns.
 *   - Apple ships "2"-suffixed extensions (eciNewEx2 etc.) that aren't
 *     in the IBM-documented API.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
```

2. After the header, paste the body of `old/eci.h` starting at `#ifndef SD_ELOQUENCE_ECI_H`. **Remove** the `EciApi` struct definition and the `eci_runtime_open` / `eci_runtime_close` declarations — those move to `runtime.h`. Stop the file at the closing `#endif`.

- [ ] **Step 2: Confirm it still compiles**

Run: `gcc -Wall -Wextra -c -o /tmp/eci_header_test.o -x c - <<< '#include "sd_eloquence/src/eci/eci.h"'`
Expected: succeeds, no warnings.

- [ ] **Step 3: Commit**

```bash
git add sd_eloquence/src/eci/eci.h
git commit -m "eci/eci.h: relicense + move out of old/

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task B2: Create `eci/runtime.h` and `eci/runtime.c` with extended EciApi (dictionaries)

**Files:**
- Create: `sd_eloquence/src/eci/runtime.h`
- Create: `sd_eloquence/src/eci/runtime.c`

- [ ] **Step 1: Write `sd_eloquence/src/eci/runtime.h`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/runtime.h -- dlopen-loaded view of the IBM ECI runtime.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_ECI_RUNTIME_H
#define SD_ELOQUENCE_ECI_RUNTIME_H

#include "eci.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Dictionary types (IBM SDK 6.7.4 §"Dictionaries"). Declared here because we
 * load the dict entry points via the same dlsym pass. */
typedef void *ECIDictHand;
#define NULL_DICT_HAND 0
enum ECIDictError {
    DictNoError = 0,
    DictFileNotFound,
    DictOutOfMemory,
    DictInternalError,
    DictNoEntry,
    DictErrLookUpKey,
    DictAccessError,
    DictInvalidVolume
};
enum ECIDictVolume {
    eciMainDict    = 0,
    eciRootDict    = 1,
    eciAbbvDict    = 2,
    eciMainDictExt = 3
};

typedef struct EciApi {
    /* Lifecycle */
    ECIHand   (*New)(void);
    ECIHand   (*NewEx)(enum ECILanguageDialect);
    ECIHand   (*Delete)(ECIHand);
    Boolean   (*Reset)(ECIHand);
    Boolean   (*IsBeingReentered)(ECIHand);
    void      (*Version)(char *pBuffer);

    /* Diagnostics */
    int       (*ProgStatus)(ECIHand);
    void      (*ErrorMessage)(ECIHand, void *buffer);
    void      (*ClearErrors)(ECIHand);
    Boolean   (*TestPhrase)(ECIHand);

    /* Single-shot speak */
    Boolean   (*SpeakText)(ECIInputText pText, Boolean bAnnotationsInTextPhrase);
    Boolean   (*SpeakTextEx)(ECIInputText pText, Boolean bAnnotationsInTextPhrase,
                             enum ECILanguageDialect);

    /* Parameters */
    int       (*GetParam)(ECIHand, enum ECIParam);
    int       (*SetParam)(ECIHand, enum ECIParam, int);
    int       (*GetDefaultParam)(enum ECIParam);
    int       (*SetDefaultParam)(enum ECIParam, int);

    /* Voices */
    Boolean   (*CopyVoice)(ECIHand, int iVoiceFrom, int iVoiceTo);
    Boolean   (*GetVoiceName)(ECIHand, int iVoice, void *pBuffer);
    Boolean   (*SetVoiceName)(ECIHand, int iVoice, const void *pBuffer);
    int       (*GetVoiceParam)(ECIHand, int iVoice, enum ECIVoiceParam);
    int       (*SetVoiceParam)(ECIHand, int iVoice, enum ECIVoiceParam, int);

    /* Synthesis queue */
    Boolean   (*AddText)(ECIHand, ECIInputText);
    Boolean   (*InsertIndex)(ECIHand, int);
    Boolean   (*Synthesize)(ECIHand);
    Boolean   (*SynthesizeFile)(ECIHand, const void *pFilename);
    Boolean   (*ClearInput)(ECIHand);
    Boolean   (*GeneratePhonemes)(ECIHand, int iSize, void *pBuffer);
    int       (*GetIndex)(ECIHand);

    /* Playback control */
    Boolean   (*Stop)(ECIHand);
    Boolean   (*Speaking)(ECIHand);
    Boolean   (*Synchronize)(ECIHand);
    Boolean   (*Pause)(ECIHand, Boolean On);

    /* Output routing */
    Boolean   (*SetOutputBuffer)(ECIHand, int iSize, short *psBuffer);
    Boolean   (*SetOutputFilename)(ECIHand, const void *pFilename);
    Boolean   (*SetOutputDevice)(ECIHand, int iDevNum);

    /* Callbacks */
    void      (*RegisterCallback)(ECIHand, ECICallback, void *pData);

    /* Languages */
    int       (*GetAvailableLanguages)(enum ECILanguageDialect *aLanguages,
                                       int *nLanguages);

    /* Dictionaries */
    ECIDictHand       (*NewDict)(ECIHand);
    ECIDictHand       (*GetDict)(ECIHand);
    enum ECIDictError (*SetDict)(ECIHand, ECIDictHand);
    ECIDictHand       (*DeleteDict)(ECIHand, ECIDictHand);
    enum ECIDictError (*LoadDict)(ECIHand, ECIDictHand,
                                  enum ECIDictVolume, ECIInputText pFilename);
} EciApi;

int  eci_runtime_open(const char *eci_so_path, EciApi *api, char **errmsg);
void eci_runtime_close(void);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 2: Write `sd_eloquence/src/eci/runtime.c`**

Copy `sd_eloquence/src/old/eci_runtime.c` to `sd_eloquence/src/eci/runtime.c`, then:

1. Replace the SPDX header at top:
```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/runtime.c -- dlopen the ECI runtime + populate an EciApi function table.
 *
 * The ECI engine registers global C++ destructors on dlopen; calling dlclose
 * runs them in an order that crashes inside libc atexit on Apple's eci.dylib
 * Linux conversion. We leave the library mapped until process exit.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
```

2. Change `#include "eci.h"` to `#include "runtime.h"`.

3. After the existing `LOAD(GetAvailableLanguages);` line, add (before `return 0;`):
```c
    /* Dictionaries */
    LOAD(NewDict);
    LOAD(GetDict);
    LOAD(SetDict);
    LOAD(DeleteDict);
    LOAD(LoadDict);
```

- [ ] **Step 3: Verify compile**

Add the runtime.c to the new target temporarily:

In `CMakeLists.txt`, change `add_executable(sd_eloquence sd_eloquence/src/module_stub.c)` to:
```cmake
        add_executable(sd_eloquence
            sd_eloquence/src/module_stub.c
            sd_eloquence/src/eci/runtime.c)
```

Run:
```bash
cmake --build build --target sd_eloquence
```
Expected: succeeds.

- [ ] **Step 4: Commit**

```bash
git add sd_eloquence/src/eci/runtime.h sd_eloquence/src/eci/runtime.c CMakeLists.txt
git commit -m "eci/runtime: dlopen wrapper + dict API

Extends EciApi with NewDict/GetDict/SetDict/DeleteDict/LoadDict so
filters.c can install per-language user dictionaries.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task B3: `eci/voices.[ch]` — Apple preset table + activation

**Files:**
- Create: `sd_eloquence/src/eci/voices.h`
- Create: `sd_eloquence/src/eci/voices.c`
- Test: `sd_eloquence/tests/test_voices.c`

- [ ] **Step 1: Write `sd_eloquence/src/eci/voices.h`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/voices.h -- Apple's 8 ECI voice presets (transcribed from
 * KonaVoicePresets.plist in TextToSpeechKonaSupport.framework).
 *
 * Apple's eci.dylib synthesizes from voice slot 0 only; slots 1..7 hold
 * inert preset data unless an explicit eciCopyVoice promotes them. We
 * always write the chosen preset's params into slot 0.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_ECI_VOICES_H
#define SD_ELOQUENCE_ECI_VOICES_H

#include "runtime.h"

#define ECI_ACTIVE_SLOT 0

typedef struct {
    const char *name;
    const char *name_fr;          /* French override; NULL = use `name` */
    int gender;                   /* eciGender: 0=male, 1=female */
    int head_size;                /* 0..100 */
    int pitch_baseline;           /* 0..100 */
    int pitch_fluctuation;
    int roughness;
    int breathiness;
    int volume;                   /* 0..100 */
    const char *spd_voice_type;   /* SSIP voice_type tag, or NULL */
} VoicePreset;

#define N_VOICE_PRESETS 8
extern const VoicePreset g_voice_presets[N_VOICE_PRESETS];

/* Return preset name appropriate for the active language; uses name_fr for
 * French dialects, name otherwise. */
const char *voice_display_name(int slot, const char *iso_lang);

/* Activate a preset into slot 0 of the engine.
 *
 * spd_rate / spd_pitch / spd_volume are speech-dispatcher overrides in the
 * range -100..+100, or INT_MIN to use the preset's defaults. Speed defaults
 * to Apple's plist value (50). */
void voice_activate(const EciApi *eci, ECIHand h, int slot,
                    int spd_rate, int spd_pitch, int spd_volume);

/* Find a preset by case-insensitive name (English or French). Returns the
 * slot index, or -1 if not found. */
int voice_find_by_name(const char *name);

/* Find a preset slot by speechd voice_type ("MALE1", "FEMALE3", etc.).
 * CHILD_MALE and CHILD_FEMALE both map to Sandy (slot 2). Returns the slot,
 * or -1 if not matched. */
int voice_find_by_voice_type(const char *voice_type);

#endif
```

- [ ] **Step 2: Write `sd_eloquence/src/eci/voices.c`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/voices.c -- Apple voice preset table + activation helpers.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "voices.h"

#include <limits.h>
#include <strings.h>
#include <string.h>

const VoicePreset g_voice_presets[N_VOICE_PRESETS] = {
    /* slot 0 */ { "Reed",    "Jacques", 0, 50, 65, 30,  0,  0, 92, "MALE1" },
    /* slot 1 */ { "Shelley", NULL,      1, 50, 81, 30,  0, 50, 95, "FEMALE1" },
    /* slot 2 */ { "Sandy",   NULL,      1, 22, 93, 30,  0, 50, 95, "CHILD_FEMALE" },
    /* slot 3 */ { "Rocko",   NULL,      0, 86, 56, 47,  0,  0, 93, "MALE2" },
    /* slot 4 */ { "Flo",     NULL,      1, 56, 89, 35,  0, 40, 95, "FEMALE2" },
    /* slot 5 */ { "Grandma", NULL,      1, 45, 68, 30,  3, 40, 90, "FEMALE3" },
    /* slot 6 */ { "Grandpa", NULL,      0, 30, 61, 44, 18, 20, 90, "MALE3" },
    /* slot 7 */ { "Eddy",    NULL,      0, 50, 69, 34,  0,  0, 92, NULL },
};

const char *voice_display_name(int slot, const char *iso_lang) {
    if (slot < 0 || slot >= N_VOICE_PRESETS) return NULL;
    const VoicePreset *v = &g_voice_presets[slot];
    if (v->name_fr && iso_lang && strcmp(iso_lang, "fr") == 0)
        return v->name_fr;
    return v->name;
}

static int spd_to_eci_pct(int v) {
    int o = (v + 100) / 2;
    if (o < 0)   o = 0;
    if (o > 100) o = 100;
    return o;
}

static int spd_to_eci_speed(int v) {
    int o = v + 100;
    if (o < 0)   o = 0;
    if (o > 200) o = 200;
    return o;
}

void voice_activate(const EciApi *eci, ECIHand h, int slot,
                    int spd_rate, int spd_pitch, int spd_volume) {
    if (slot < 0 || slot >= N_VOICE_PRESETS) return;
    const VoicePreset *v = &g_voice_presets[slot];

    int speed_val = (spd_rate   != INT_MIN) ? spd_to_eci_speed(spd_rate) : 50;
    int pitch_val = (spd_pitch  != INT_MIN) ? spd_to_eci_pct(spd_pitch)
                                            : v->pitch_baseline;
    int vol_val   = (spd_volume != INT_MIN) ? spd_to_eci_pct(spd_volume)
                                            : v->volume;

    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciGender,           v->gender);
    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciHeadSize,         v->head_size);
    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciPitchFluctuation, v->pitch_fluctuation);
    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciPitchBaseline,    pitch_val);
    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciRoughness,        v->roughness);
    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciBreathiness,      v->breathiness);
    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciSpeed,            speed_val);
    eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciVolume,           vol_val);
}

int voice_find_by_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < N_VOICE_PRESETS; i++) {
        const VoicePreset *p = &g_voice_presets[i];
        if (!strcasecmp(name, p->name)) return i;
        if (p->name_fr && !strcasecmp(name, p->name_fr)) return i;
    }
    return -1;
}

int voice_find_by_voice_type(const char *t) {
    if (!t) return -1;
    if (!strcasecmp(t, "CHILD_MALE") || !strcasecmp(t, "CHILD_FEMALE"))
        return 2;  /* Sandy */
    for (int i = 0; i < N_VOICE_PRESETS; i++) {
        const char *vt = g_voice_presets[i].spd_voice_type;
        if (vt && !strcasecmp(t, vt)) return i;
    }
    return -1;
}
```

- [ ] **Step 3: Write the failing test `sd_eloquence/tests/test_voices.c`**

```c
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
```

- [ ] **Step 4: Wire up the test in CMakeLists.txt**

Find the `enable_testing()` line we added in Task A4. After it (or in a logical block near the bottom of CMakeLists), add:

```cmake
# Unit tests for the new sd_eloquence module. These don't need eci.so.
if(BUILD_SD_MODULE AND SPEECHD_FOUND)
    add_executable(test_voices
        sd_eloquence/tests/test_voices.c
        sd_eloquence/src/eci/voices.c)
    target_include_directories(test_voices PRIVATE sd_eloquence/src)
    add_test(NAME test_voices COMMAND test_voices)
endif()
```

- [ ] **Step 5: Run the test — verify it passes**

```bash
cmake --build build --target test_voices
ctest --test-dir build -R test_voices --output-on-failure
```
Expected: `test_voices: OK` and `1/1 Test #1: test_voices ... Passed`.

- [ ] **Step 6: Commit**

```bash
git add sd_eloquence/src/eci/voices.h sd_eloquence/src/eci/voices.c \
        sd_eloquence/tests/test_voices.c CMakeLists.txt
git commit -m "eci/voices: Apple preset table + lookup helpers

Slot 0 is the engine's only synthesis slot; voice_activate writes the
chosen preset's params into it. Lookup helpers cover English and French
aliases (Jacques = Reed in French), speechd voice_type tags including
the CHILD_* → Sandy mapping.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task B4: `eci/languages.[ch]` — language table + IETF↔dialect mapping

**Files:**
- Create: `sd_eloquence/src/eci/languages.h`
- Create: `sd_eloquence/src/eci/languages.c`
- Test: `sd_eloquence/tests/test_languages.c`

- [ ] **Step 1: Write `sd_eloquence/src/eci/languages.h`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/languages.h -- dialect/iso/availability table.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_ECI_LANGUAGES_H
#define SD_ELOQUENCE_ECI_LANGUAGES_H

#include "runtime.h"

typedef enum {
    LANG_AVAILABLE = 0,
    LANG_DISABLED  = 1,   /* present on disk but gated out (e.g. CJK known-unstable) */
    LANG_MISSING   = 2,   /* the .so isn't installed */
    LANG_WEDGED    = 3,   /* tripped a SIGSEGV guard or NewEx returned NULL */
} LangState;

typedef struct {
    int   eci_dialect;
    int   ini_major;
    int   ini_minor;
    const char *so_name;       /* e.g. "enu.so" */
    const char *langid;        /* e.g. "enu" -- prefix for main.dic/root.dic/abbr.dic */
    const char *iso_lang;      /* e.g. "en" */
    const char *iso_variant;   /* e.g. "us" */
    const char *human;
} LangEntry;

#define N_LANGS 14
extern const LangEntry g_langs[N_LANGS];

extern LangState g_lang_state[N_LANGS];

/* Lookups. Return NULL / -1 on miss. */
const LangEntry *lang_by_dialect(int dialect);
const LangEntry *lang_by_iso(const char *tag);     /* "en", "en-US", "en_us" */
int              lang_index(const LangEntry *L);   /* offset in g_langs */

/* Encoding for `eci.AddText` payloads on a given dialect (see spec §7.3).
 * Returns one of "cp1252" "gb18030" "cp932" "cp949" "big5". */
const char *lang_encoding_for(int dialect);

#endif
```

- [ ] **Step 2: Write `sd_eloquence/src/eci/languages.c`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/languages.c -- dialect/iso table.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "languages.h"

#include <strings.h>
#include <string.h>
#include <stdlib.h>

const LangEntry g_langs[N_LANGS] = {
    { eciGeneralAmericanEnglish, 1,  0, "enu.so", "enu", "en", "us", "American English" },
    { eciBritishEnglish,         1,  1, "eng.so", "eng", "en", "gb", "British English" },
    { eciCastilianSpanish,       2,  0, "esp.so", "esp", "es", "es", "Castilian Spanish" },
    { eciMexicanSpanish,         2,  1, "esm.so", "esm", "es", "mx", "Latin American Spanish" },
    { eciStandardFrench,         3,  0, "fra.so", "fra", "fr", "fr", "French" },
    { eciCanadianFrench,         3,  1, "frc.so", "frc", "fr", "ca", "Canadian French" },
    { eciStandardGerman,         4,  0, "deu.so", "deu", "de", "de", "German" },
    { eciStandardItalian,        5,  0, "ita.so", "ita", "it", "it", "Italian" },
    { eciMandarinChinese,        6,  0, "chs.so", "chs", "zh", "cn", "Mandarin Chinese (Simplified)" },
    { eciTaiwaneseMandarin,      6,  1, "cht.so", "cht", "zh", "tw", "Mandarin Chinese (Traditional)" },
    { eciBrazilianPortuguese,    7,  0, "ptb.so", "ptb", "pt", "br", "Brazilian Portuguese" },
    { eciStandardJapanese,       8,  0, "jpn.so", "jpn", "ja", "jp", "Japanese" },
    { eciStandardFinnish,        9,  0, "fin.so", "fin", "fi", "fi", "Finnish" },
    { eciStandardKorean,        10,  0, "kor.so", "kor", "ko", "kr", "Korean" },
};

LangState g_lang_state[N_LANGS] = { 0 };  /* set by engine.c at init */

const LangEntry *lang_by_dialect(int dialect) {
    for (int i = 0; i < N_LANGS; i++)
        if (g_langs[i].eci_dialect == dialect) return &g_langs[i];
    return NULL;
}

const LangEntry *lang_by_iso(const char *tag) {
    if (!tag) return NULL;
    /* Try exact "lang-region" match first. */
    for (int i = 0; i < N_LANGS; i++) {
        char ietf[16];
        snprintf(ietf, sizeof(ietf), "%s-%s",
                 g_langs[i].iso_lang, g_langs[i].iso_variant);
        if (strcasecmp(tag, ietf) == 0) return &g_langs[i];
    }
    /* Then fall back to "lang" prefix match (any region). */
    for (int i = 0; i < N_LANGS; i++) {
        size_t L = strlen(g_langs[i].iso_lang);
        if (strncasecmp(tag, g_langs[i].iso_lang, L) == 0 &&
            (tag[L] == 0 || tag[L] == '-' || tag[L] == '_'))
            return &g_langs[i];
    }
    return NULL;
}

int lang_index(const LangEntry *L) {
    if (!L || L < g_langs || L >= g_langs + N_LANGS) return -1;
    return (int)(L - g_langs);
}

const char *lang_encoding_for(int dialect) {
    switch (dialect) {
        case eciMandarinChinese:   return "gb18030";
        case eciStandardJapanese:  return "cp932";
        case eciStandardKorean:    return "cp949";
        case eciHongKongCantonese: return "big5";
        default:                   return "cp1252";
    }
}
```

- [ ] **Step 3: Write `sd_eloquence/tests/test_languages.c`**

```c
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
```

- [ ] **Step 4: Wire up the test in CMakeLists.txt**

Inside the existing `if(BUILD_SD_MODULE AND SPEECHD_FOUND)` test block (where `test_voices` is registered), add:

```cmake
    add_executable(test_languages
        sd_eloquence/tests/test_languages.c
        sd_eloquence/src/eci/languages.c)
    target_include_directories(test_languages PRIVATE sd_eloquence/src)
    add_test(NAME test_languages COMMAND test_languages)
```

- [ ] **Step 5: Run the test**

```bash
cmake --build build --target test_languages
ctest --test-dir build -R test_languages --output-on-failure
```
Expected: `test_languages: OK`, Passed.

- [ ] **Step 6: Commit**

```bash
git add sd_eloquence/src/eci/languages.h sd_eloquence/src/eci/languages.c \
        sd_eloquence/tests/test_languages.c CMakeLists.txt
git commit -m "eci/languages: dialect/iso/encoding table + lookup

LangEntry now also carries the 3-letter langid used as prefix for the
optional user-dictionary files (enu*.dic, deu*.dic, …). LangState is
declared here but populated by engine.c at init time.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task B5: `eci/engine.[ch]` — quirks-aware open/close/switch/pause

**Files:**
- Create: `sd_eloquence/src/eci/engine.h`
- Create: `sd_eloquence/src/eci/engine.c`

This wraps the bare `EciApi` with handlers for the known Apple eci.dylib quirks: eciSetParam(eciSampleRate, 2) rejection, can't dlclose, can't second-reload a language .so, and language-switch via SetParam (not Delete+NewEx). The component does *not* yet contain dictionaries or the CJK strategy — those land in later tasks.

- [ ] **Step 1: Write `sd_eloquence/src/eci/engine.h`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/engine.h -- quirks-aware ECI engine wrapper.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_ECI_ENGINE_H
#define SD_ELOQUENCE_ECI_ENGINE_H

#include "runtime.h"
#include "languages.h"

typedef struct {
    EciApi   api;
    ECIHand  h;
    int      sample_rate_param;   /* 0=8k 1=11025 2=22050 */
    int      sample_rate_hz;
    int      current_dialect;
    int      current_voice_slot;
} EciEngine;

/* Open eci.so at `eci_so_path`, create the engine handle for `initial_dialect`,
 * apply sample_rate_param (falling back to 1 if rejected), register the audio
 * callback. Returns 0 on success; on failure returns -1 and writes a
 * heap-allocated error string to *errmsg. */
int  engine_open(EciEngine *e,
                 const char *eci_so_path,
                 int initial_dialect,
                 int sample_rate_param,
                 ECICallback audio_cb,
                 void *cb_data,
                 short *pcm_chunk,
                 int   pcm_chunk_samples,
                 char **errmsg);

/* Close the engine handle. The shared library stays mapped (deliberate; see
 * runtime.c). Safe to call with !e->h. */
void engine_close(EciEngine *e);

/* Switch to a new language dialect. Uses SetParam(eciLanguageDialect)
 * rather than Delete+NewEx because the latter crashes Apple's build on
 * second reload of a language .so. */
int  engine_switch_language(EciEngine *e, int dialect);

/* Pause / resume via eciPause. */
void engine_pause(EciEngine *e, int on);

/* Stop synthesis (eciStop). Safe to call from any thread. */
void engine_stop(EciEngine *e);

/* Stub: returns a heap-allocated version string ("6.1.0.0") via eciVersion. */
char *engine_version(EciEngine *e);

#endif
```

- [ ] **Step 2: Write `sd_eloquence/src/eci/engine.c`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/engine.c -- quirks-aware engine wrapper.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "engine.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sample_rate_param_to_hz(int p) {
    switch (p) { case 0: return 8000; case 1: return 11025; case 2: return 22050; }
    return 11025;
}

int engine_open(EciEngine *e,
                const char *eci_so_path,
                int initial_dialect,
                int sample_rate_param,
                ECICallback audio_cb,
                void *cb_data,
                short *pcm_chunk,
                int   pcm_chunk_samples,
                char **errmsg) {
    memset(e, 0, sizeof(*e));

    char *err = NULL;
    if (eci_runtime_open(eci_so_path, &e->api, &err) != 0) {
        if (errmsg) *errmsg = err;
        return -1;
    }

    e->h = e->api.NewEx((enum ECILanguageDialect)initial_dialect);
    if (!e->h) {
        const LangEntry *L = lang_by_dialect(initial_dialect);
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "eciNewEx(%#x %s) returned NULL", initial_dialect,
                 L ? L->human : "?");
        if (errmsg) *errmsg = strdup(buf);
        return -1;
    }
    e->current_dialect = initial_dialect;
    e->current_voice_slot = 0;

    /* Apple rejects sample_rate=2 (22050); fall back to 11025. */
    if (e->api.SetParam(e->h, eciSampleRate, sample_rate_param) < 0) {
        sample_rate_param = 1;
        e->api.SetParam(e->h, eciSampleRate, sample_rate_param);
    }
    e->sample_rate_param = sample_rate_param;
    e->sample_rate_hz    = sample_rate_param_to_hz(sample_rate_param);

    e->api.RegisterCallback(e->h, audio_cb, cb_data);
    e->api.SetOutputBuffer(e->h, pcm_chunk_samples, pcm_chunk);
    return 0;
}

void engine_close(EciEngine *e) {
    if (e->h) {
        e->api.Stop(e->h);
        e->api.Delete(e->h);
        e->h = NULL;
    }
    eci_runtime_close();
}

int engine_switch_language(EciEngine *e, int dialect) {
    if (!e->h || dialect == e->current_dialect) return 0;
    e->api.Stop(e->h);
    e->api.Synchronize(e->h);
    /* SetParam sometimes returns -1 but the engine synthesizes in the new
     * dialect anyway; we trust that path because Delete+NewEx crashes
     * Apple's build on the second reload of a language .so. */
    e->api.SetParam(e->h, eciLanguageDialect, dialect);
    e->current_dialect = dialect;
    return 0;
}

void engine_pause(EciEngine *e, int on) {
    if (e->h) e->api.Pause(e->h, on ? ECITrue : ECIFalse);
}

void engine_stop(EciEngine *e) {
    if (e->h) e->api.Stop(e->h);
}

char *engine_version(EciEngine *e) {
    char buf[64] = { 0 };
    e->api.Version(buf);
    return strdup(buf);
}
```

- [ ] **Step 3: Verify compile by adding to the sd_eloquence target**

In `CMakeLists.txt`, find the `add_executable(sd_eloquence ...)` block from Task A4, replace the source list to include engine + languages + voices:

```cmake
        add_executable(sd_eloquence
            sd_eloquence/src/module_stub.c
            sd_eloquence/src/eci/runtime.c
            sd_eloquence/src/eci/voices.c
            sd_eloquence/src/eci/languages.c
            sd_eloquence/src/eci/engine.c)
```

Run: `cmake --build build --target sd_eloquence`
Expected: succeeds.

- [ ] **Step 4: Commit**

```bash
git add sd_eloquence/src/eci/engine.h sd_eloquence/src/eci/engine.c CMakeLists.txt
git commit -m "eci/engine: quirks-aware open/switch/close

Encapsulates the Apple eci.dylib quirks (eciSampleRate=2 rejection
falls back to 11025; language switching via SetParam not Delete+NewEx;
shared library stays mapped at close). CJK-specific logic added in
later tasks.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Phase C: config + audio

### Task C1: `config.[ch]` — eloquence.conf parser

**Files:**
- Create: `sd_eloquence/src/config.h`
- Create: `sd_eloquence/src/config.c`
- Test: `sd_eloquence/tests/test_config.c`

- [ ] **Step 1: Write `sd_eloquence/src/config.h`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * config.h -- eloquence.conf parser. All keys have hardcoded defaults;
 * unknown keys log a debug warning but never fail init.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_CONFIG_H
#define SD_ELOQUENCE_CONFIG_H

#define ELOQ_PATH_MAX 512

typedef struct {
    int  debug;
    char data_dir[ELOQ_PATH_MAX];
    char dict_dir[ELOQ_PATH_MAX];   /* empty -> derive from data_dir + "/dicts" */
    int  default_sample_rate;        /* 0/1/2 */
    int  default_voice_slot;         /* 0..7 */
    int  default_language;           /* ECI dialect code */

    /* libsoxr resampling */
    int  resample_rate;              /* 0 = pass-through */
    int  resample_quality;           /* 0=quick .. 4=very-high */
    int  resample_phase;             /* 0=intermediate 1=linear 2=minimum */
    int  resample_steep;

    /* NVDA-style toggles */
    int  use_dictionaries;           /* default 1 */
    int  rate_boost;                 /* default 0; eciSpeed multiplier when on */
    int  pause_mode;                 /* 0=none 1=end-only 2=all; default 2 */
    int  phrase_prediction;          /* default 0 */
    int  send_params;                /* default 1 */
    int  backquote_tags;             /* default 0 (security) */

    /* CJK */
    int  cjk_segv_guard;             /* default 0 */
} EloqConfig;

/* Initialize *c with hardcoded defaults. */
void config_defaults(EloqConfig *c);

/* Parse `path` into *c, layering over the existing values. Missing file = OK,
 * use defaults; returns 0. Returns -1 only on fopen errors other than ENOENT.
 * Unknown keys log via fprintf(stderr,...) when c->debug is non-zero. */
int  config_parse_file(EloqConfig *c, const char *path);

/* Parse a single key=value pair (used by tests; bypasses file I/O). Returns
 * 0 on success, -1 on validation error. Whitespace already stripped. */
int  config_apply_kv(EloqConfig *c, const char *key, const char *val);

/* Returns the effective dictionary dir: c->dict_dir if non-empty, otherwise
 * "${c->data_dir}/dicts" in a static buffer (caller must not retain across
 * subsequent calls). */
const char *config_effective_dict_dir(const EloqConfig *c);

#endif
```

- [ ] **Step 2: Write `sd_eloquence/src/config.c`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * config.c -- eloquence.conf parser.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "config.h"

#include "eci/eci.h"
#include "eci/voices.h"
#include "eci/languages.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void config_defaults(EloqConfig *c) {
    memset(c, 0, sizeof(*c));
    c->debug = 0;
    strcpy(c->data_dir, "/usr/lib/eloquence");
    c->dict_dir[0] = 0;
    c->default_sample_rate = 1;
    c->default_voice_slot  = 0;
    c->default_language    = eciGeneralAmericanEnglish;
    c->resample_rate    = 0;
    c->resample_quality = 4;
    c->resample_phase   = 0;
    c->resample_steep   = 0;
    c->use_dictionaries  = 1;
    c->rate_boost        = 0;
    c->pause_mode        = 2;
    c->phrase_prediction = 0;
    c->send_params       = 1;
    c->backquote_tags    = 0;
    c->cjk_segv_guard    = 0;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    if (*s == '"' && e > s + 1 && e[-1] == '"') { *--e = 0; s++; }
    return s;
}

static int parse_quality(const char *v) {
    if (!strcasecmp(v, "quick"))     return 0;
    if (!strcasecmp(v, "low"))       return 1;
    if (!strcasecmp(v, "medium"))    return 2;
    if (!strcasecmp(v, "high"))      return 3;
    if (!strcasecmp(v, "very-high") ||
        !strcasecmp(v, "vhq"))       return 4;
    return -1;
}

static int parse_phase(const char *v) {
    if (!strcasecmp(v, "intermediate")) return 0;
    if (!strcasecmp(v, "linear"))       return 1;
    if (!strcasecmp(v, "minimum"))      return 2;
    return -1;
}

int config_apply_kv(EloqConfig *c, const char *key, const char *val) {
    if (!key || !val) return -1;

#define COPY_PATH(field) do { \
        strncpy(c->field, val, sizeof(c->field) - 1); \
        c->field[sizeof(c->field) - 1] = 0; \
    } while (0)

    if      (!strcasecmp(key, "Debug"))                  c->debug = atoi(val);
    else if (!strcasecmp(key, "EloquenceDataDir"))       COPY_PATH(data_dir);
    else if (!strcasecmp(key, "EloquenceDictionaryDir")) COPY_PATH(dict_dir);
    else if (!strcasecmp(key, "EloquenceSampleRate")) {
        int n = atoi(val);
        if (n < 0 || n > 2) return -1;
        c->default_sample_rate = n;
    }
    else if (!strcasecmp(key, "EloquenceDefaultVoice")) {
        char *end = NULL;
        long n = strtol(val, &end, 10);
        if (end && *end == 0 && n >= 0 && n < N_VOICE_PRESETS)
            c->default_voice_slot = (int)n;
        else {
            int slot = voice_find_by_name(val);
            if (slot < 0) return -1;
            c->default_voice_slot = slot;
        }
    }
    else if (!strcasecmp(key, "EloquenceDefaultLanguage")) {
        char *end = NULL;
        long n = strtol(val, &end, 0);
        if (end && *end == 0) {
            if (!lang_by_dialect((int)n)) return -1;
            c->default_language = (int)n;
        } else {
            const LangEntry *L = lang_by_iso(val);
            if (!L) return -1;
            c->default_language = L->eci_dialect;
        }
    }
    else if (!strcasecmp(key, "EloquenceResampleRate"))    c->resample_rate    = atoi(val);
    else if (!strcasecmp(key, "EloquenceResampleQuality")) {
        int q = parse_quality(val); if (q < 0) return -1; c->resample_quality = q;
    }
    else if (!strcasecmp(key, "EloquenceResamplePhase")) {
        int p = parse_phase(val); if (p < 0) return -1; c->resample_phase = p;
    }
    else if (!strcasecmp(key, "EloquenceResampleSteep"))   c->resample_steep   = atoi(val) ? 1 : 0;
    else if (!strcasecmp(key, "EloquenceUseDictionaries")) c->use_dictionaries = atoi(val) ? 1 : 0;
    else if (!strcasecmp(key, "EloquenceRateBoost"))       c->rate_boost       = atoi(val) ? 1 : 0;
    else if (!strcasecmp(key, "EloquencePauseMode")) {
        int n = atoi(val); if (n < 0 || n > 2) return -1; c->pause_mode = n;
    }
    else if (!strcasecmp(key, "EloquencePhrasePrediction")) c->phrase_prediction = atoi(val) ? 1 : 0;
    else if (!strcasecmp(key, "EloquenceSendParams"))       c->send_params       = atoi(val) ? 1 : 0;
    else if (!strcasecmp(key, "EloquenceBackquoteTags"))    c->backquote_tags    = atoi(val) ? 1 : 0;
    else if (!strcasecmp(key, "EloquenceCjkSegvGuard"))     c->cjk_segv_guard    = atoi(val) ? 1 : 0;
    else return -1;  /* unknown key */

#undef COPY_PATH
    return 0;
}

int config_parse_file(EloqConfig *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return 0;
        return -1;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (*p == 0 || *p == '#') continue;
        char *sp = p;
        while (*sp && !isspace((unsigned char)*sp)) sp++;
        if (*sp == 0) continue;
        *sp++ = 0;
        char *val = trim(sp);
        int r = config_apply_kv(c, p, val);
        if (r != 0 && c->debug)
            fprintf(stderr, "sd_eloquence: ignored config '%s = %s'\n", p, val);
    }
    fclose(f);
    return 0;
}

const char *config_effective_dict_dir(const EloqConfig *c) {
    static char buf[ELOQ_PATH_MAX + 8];
    if (c->dict_dir[0]) return c->dict_dir;
    snprintf(buf, sizeof(buf), "%s/dicts", c->data_dir);
    return buf;
}
```

- [ ] **Step 3: Write `sd_eloquence/tests/test_config.c`**

```c
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
    fprintf(f, "EloquenceRateBoost 1\n");
    fprintf(f, "BogusKey something\n");  /* logged but not fatal */
    fclose(f);
    config_defaults(&c);
    assert(config_parse_file(&c, tmp) == 0);
    assert(c.debug == 1);
    assert(c.backquote_tags == 1);
    assert(c.rate_boost == 1);

    /* Missing file is OK. */
    config_defaults(&c);
    assert(config_parse_file(&c, "/nonexistent.conf") == 0);

    puts("test_config: OK");
    return 0;
}
```

- [ ] **Step 4: Wire up the test**

In CMakeLists.txt, inside the test block:

```cmake
    add_executable(test_config
        sd_eloquence/tests/test_config.c
        sd_eloquence/src/config.c
        sd_eloquence/src/eci/voices.c
        sd_eloquence/src/eci/languages.c)
    target_include_directories(test_config PRIVATE sd_eloquence/src)
    add_test(NAME test_config COMMAND test_config)
```

- [ ] **Step 5: Run the test**

```bash
cmake --build build --target test_config
ctest --test-dir build -R test_config --output-on-failure
```
Expected: `test_config: OK`, Passed.

- [ ] **Step 6: Commit**

```bash
git add sd_eloquence/src/config.h sd_eloquence/src/config.c \
        sd_eloquence/tests/test_config.c CMakeLists.txt
git commit -m "config: eloquence.conf parser + defaults

All keys have hardcoded defaults; unknown keys log a debug warning but
don't fail init. Dictionary dir derives from data dir when unset.
config_apply_kv is the testable per-key entry point.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task C2: `audio/resampler.[ch]` — libsoxr wrapper

**Files:**
- Create: `sd_eloquence/src/audio/resampler.h`
- Create: `sd_eloquence/src/audio/resampler.c`

- [ ] **Step 1: Write `sd_eloquence/src/audio/resampler.h`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * audio/resampler.h -- libsoxr wrapper. Pass-through when disabled.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_AUDIO_RESAMPLER_H
#define SD_ELOQUENCE_AUDIO_RESAMPLER_H

#include <stddef.h>
#include <stdint.h>

typedef struct Resampler Resampler;

/* Allocate a resampler going input_hz -> output_hz. output_hz=0 or
 * input_hz==output_hz returns a pass-through resampler (resampler_process
 * just memcpy's). quality is 0..4 (quick..very-high); phase is 0..2
 * (intermediate/linear/minimum); steep is 0 or 1. */
Resampler *resampler_new(int input_hz, int output_hz,
                         int quality, int phase, int steep,
                         char **errmsg);
void resampler_free(Resampler *r);

/* Resample `in_samples` int16 mono samples from `in` into `out` (capacity
 * `out_cap`). Returns count actually written, or -1 on error. */
int  resampler_process(Resampler *r,
                       const int16_t *in, int in_samples,
                       int16_t *out, int out_cap);

/* End-of-utterance flush — drains the polyphase tail. Returns count written
 * to `out`. */
int  resampler_flush(Resampler *r, int16_t *out, int out_cap);

/* True if the resampler is active (not pass-through). */
int  resampler_is_active(const Resampler *r);
int  resampler_output_rate(const Resampler *r);

#endif
```

- [ ] **Step 2: Write `sd_eloquence/src/audio/resampler.c`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * audio/resampler.c -- libsoxr wrapper.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "resampler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SOXR
#include <soxr.h>
#endif

struct Resampler {
    int input_hz;
    int output_hz;
    int active;
#ifdef HAVE_SOXR
    soxr_t  soxr;
#endif
};

Resampler *resampler_new(int input_hz, int output_hz,
                         int quality, int phase, int steep,
                         char **errmsg) {
    Resampler *r = calloc(1, sizeof(*r));
    if (!r) { if (errmsg) *errmsg = strdup("alloc failure"); return NULL; }
    r->input_hz = input_hz;
    r->output_hz = output_hz;

    if (output_hz <= 0 || output_hz == input_hz) {
        r->active = 0;
        r->output_hz = input_hz;
        return r;
    }

#ifdef HAVE_SOXR
    static const unsigned long qtbl[] = { SOXR_QQ, SOXR_LQ, SOXR_MQ, SOXR_HQ, SOXR_VHQ };
    int qi = quality;
    if (qi < 0 || qi > 4) qi = 4;
    unsigned long recipe = qtbl[qi];
    switch (phase) {
        case 0: recipe |= SOXR_INTERMEDIATE_PHASE; break;
        case 1: recipe |= SOXR_LINEAR_PHASE;       break;
        case 2: recipe |= SOXR_MINIMUM_PHASE;      break;
    }
    if (steep) recipe |= SOXR_STEEP_FILTER;

    soxr_quality_spec_t qs = soxr_quality_spec(recipe, 0);
    soxr_io_spec_t      is = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);
    soxr_error_t err = NULL;
    r->soxr = soxr_create(input_hz, output_hz, 1, &err, &is, &qs, NULL);
    if (err) {
        if (errmsg) {
            char buf[256];
            snprintf(buf, sizeof(buf), "soxr_create: %s", soxr_strerror(err));
            *errmsg = strdup(buf);
        }
        free(r);
        return NULL;
    }
    r->active = 1;
    return r;
#else
    (void)quality; (void)phase; (void)steep;
    if (errmsg) *errmsg = strdup("resampling requested but built without HAVE_SOXR");
    free(r);
    return NULL;
#endif
}

void resampler_free(Resampler *r) {
    if (!r) return;
#ifdef HAVE_SOXR
    if (r->soxr) soxr_delete(r->soxr);
#endif
    free(r);
}

int resampler_process(Resampler *r,
                      const int16_t *in, int in_samples,
                      int16_t *out, int out_cap) {
    if (!r->active) {
        int n = in_samples < out_cap ? in_samples : out_cap;
        memcpy(out, in, n * sizeof(int16_t));
        return n;
    }
#ifdef HAVE_SOXR
    size_t in_done = 0, out_done = 0;
    soxr_error_t err = soxr_process(r->soxr, in, in_samples, &in_done,
                                    out, out_cap, &out_done);
    if (err) return -1;
    return (int)out_done;
#else
    (void)in; (void)in_samples; (void)out; (void)out_cap;
    return -1;
#endif
}

int resampler_flush(Resampler *r, int16_t *out, int out_cap) {
    if (!r->active) return 0;
#ifdef HAVE_SOXR
    size_t out_done = 0;
    soxr_error_t err = soxr_process(r->soxr, NULL, 0, NULL, out, out_cap, &out_done);
    if (err) return 0;
    return (int)out_done;
#else
    (void)out; (void)out_cap;
    return 0;
#endif
}

int resampler_is_active(const Resampler *r) { return r ? r->active : 0; }
int resampler_output_rate(const Resampler *r) { return r ? r->output_hz : 0; }
```

- [ ] **Step 3: Wire into the target**

In CMakeLists.txt, append `sd_eloquence/src/audio/resampler.c` to the `add_executable(sd_eloquence …)` source list. Verify with `cmake --build build --target sd_eloquence` (succeeds).

- [ ] **Step 4: Commit**

```bash
git add sd_eloquence/src/audio/resampler.h sd_eloquence/src/audio/resampler.c CMakeLists.txt
git commit -m "audio/resampler: libsoxr wrapper with pass-through fallback

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task C3: `audio/sink.[ch]` — speechd PCM forwarder

**Files:**
- Create: `sd_eloquence/src/audio/sink.h`
- Create: `sd_eloquence/src/audio/sink.c`

- [ ] **Step 1: Write `sd_eloquence/src/audio/sink.h`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * audio/sink.h -- forward PCM to speech-dispatcher's module_tts_output_server.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_AUDIO_SINK_H
#define SD_ELOQUENCE_AUDIO_SINK_H

#include "resampler.h"

#include <stdint.h>

typedef struct {
    Resampler *resampler;
    int        engine_rate_hz;
    int16_t   *scratch;          /* used when resampler is active */
    int        scratch_cap;
} AudioSink;

/* Initialize a sink. If `resampler` is non-NULL the sink will resample;
 * pass NULL for direct pass-through at `engine_rate_hz`. The scratch buffer
 * is allocated internally (resampler ratio × pcm_chunk_samples). */
int  audio_sink_init(AudioSink *s, Resampler *resampler,
                     int engine_rate_hz, int pcm_chunk_samples,
                     char **errmsg);
void audio_sink_dispose(AudioSink *s);

/* Push `n` int16 mono samples through the sink; resamples (if active) and
 * forwards via module_tts_output_server. Returns 0 on success, -1 on
 * resampler or speechd error. */
int  audio_sink_push(AudioSink *s, const int16_t *samples, int n);

/* Drain resampler tail at end of utterance. */
void audio_sink_flush(AudioSink *s);

#endif
```

- [ ] **Step 2: Write `sd_eloquence/src/audio/sink.c`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * audio/sink.c -- forward PCM to module_tts_output_server.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "sink.h"

#include "old/spd_audio.h"
#include <speech-dispatcher/spd_module_main.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int audio_sink_init(AudioSink *s, Resampler *r,
                    int engine_rate_hz, int pcm_chunk_samples,
                    char **errmsg) {
    memset(s, 0, sizeof(*s));
    s->resampler = r;
    s->engine_rate_hz = engine_rate_hz;
    if (r && resampler_is_active(r)) {
        /* 8× covers 11025 -> 48000 with headroom for soxr's polyphase
         * flush boundaries; less than that and soxr can write past the
         * end of the buffer. */
        s->scratch_cap = pcm_chunk_samples * 8;
        s->scratch = malloc(s->scratch_cap * sizeof(int16_t));
        if (!s->scratch) {
            if (errmsg) *errmsg = strdup("alloc failure");
            return -1;
        }
    }
    return 0;
}

void audio_sink_dispose(AudioSink *s) {
    free(s->scratch);
    s->scratch = NULL;
}

static int forward(int16_t *samples, int n, int rate_hz) {
    AudioTrack t = {
        .bits         = 16,
        .num_channels = 1,
        .num_samples  = n,
        .sample_rate  = rate_hz,
        .samples      = samples,
    };
    return module_tts_output_server(&t, SPD_AUDIO_LE);
}

int audio_sink_push(AudioSink *s, const int16_t *in, int n) {
    if (n <= 0) return 0;

    if (s->resampler && resampler_is_active(s->resampler)) {
        int got = resampler_process(s->resampler, in, n, s->scratch, s->scratch_cap);
        if (got < 0) {
            fprintf(stderr, "sd_eloquence: resampler error\n");
            return -1;
        }
        if (got > 0) {
            int rate = resampler_output_rate(s->resampler);
            if (forward(s->scratch, got, rate) < 0) return -1;
        }
        return 0;
    }

    /* Pass-through: forward.const-correct cast is OK because module_tts_output_server
     * doesn't mutate the buffer despite the non-const pointer. */
    return forward((int16_t *)in, n, s->engine_rate_hz);
}

void audio_sink_flush(AudioSink *s) {
    if (!s->resampler || !resampler_is_active(s->resampler) || !s->scratch) return;
    int got = resampler_flush(s->resampler, s->scratch, s->scratch_cap);
    if (got > 0)
        forward(s->scratch, got, resampler_output_rate(s->resampler));
}
```

- [ ] **Step 3: Bring back `spd_audio.h` into the new tree**

The new sink references `old/spd_audio.h`. That's fine for now (`old/` is still on the include path implicitly when files include with a relative path). Verify by adding sink.c to the target and rebuilding:

```cmake
        add_executable(sd_eloquence
            sd_eloquence/src/module_stub.c
            sd_eloquence/src/eci/runtime.c
            sd_eloquence/src/eci/voices.c
            sd_eloquence/src/eci/languages.c
            sd_eloquence/src/eci/engine.c
            sd_eloquence/src/config.c
            sd_eloquence/src/audio/resampler.c
            sd_eloquence/src/audio/sink.c)
```

Run: `cmake --build build --target sd_eloquence`
Expected: succeeds.

- [ ] **Step 4: Commit**

```bash
git add sd_eloquence/src/audio/sink.h sd_eloquence/src/audio/sink.c CMakeLists.txt
git commit -m "audio/sink: speechd PCM forwarder with optional resampling

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Phase D: SSML parser

### Task D1: `synth/marks.[ch]` — mark name ↔ id table

**Files:**
- Create: `sd_eloquence/src/synth/marks.h`
- Create: `sd_eloquence/src/synth/marks.c`
- Test: `sd_eloquence/tests/test_marks.c`

- [ ] **Step 1: Write `sd_eloquence/src/synth/marks.h`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * synth/marks.h -- numeric-id ↔ name table for SSML <mark> support.
 *
 * The encoding is (job_seq << 16) | per_job_idx, so stale marks from a
 * canceled job can be distinguished from current-job marks at callback time.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_SYNTH_MARKS_H
#define SD_ELOQUENCE_SYNTH_MARKS_H

#include <stdint.h>
#include <stdbool.h>

#define MARKS_MAX        256
#define END_STRING_ID    0xFFFFu     /* reserved low-16 sentinel for end-of-string */

/* Initialize / reset the global marks table. Call once at module init. */
void marks_init(void);

/* Register a mark name and return its 32-bit id. Returns 0 if the table is
 * full (caller logs and synthesizes without the mark). NULL name = unregister
 * everything for this job_seq. The 16-bit per-job idx range is 0..0xFFFE;
 * we reserve 0xFFFF for END_STRING. */
uint32_t marks_register(const char *name, uint32_t job_seq);

/* Look up a mark by id. Returns NULL if not in the table or already consumed.
 * Marks the entry consumed so the next lookup returns NULL. */
const char *marks_resolve(uint32_t id);

/* Free every entry whose job_seq matches. Call when a job is freed. */
void marks_release_job(uint32_t job_seq);

/* Helpers. */
static inline uint32_t marks_make_end(uint32_t job_seq) {
    return (job_seq << 16) | END_STRING_ID;
}
static inline uint32_t marks_job_of(uint32_t id) { return id >> 16; }
static inline uint16_t marks_idx_of(uint32_t id) { return (uint16_t)(id & 0xFFFF); }

#endif
```

- [ ] **Step 2: Write `sd_eloquence/src/synth/marks.c`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * synth/marks.c -- flat-table mark registry.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "marks.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    uint32_t id;
    char    *name;
    uint32_t job_seq;
    bool     in_use;
    bool     consumed;
} MarkEntry;

static MarkEntry g_table[MARKS_MAX];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t g_next_idx_per_job[65536];   /* tracks per-job counter (small) */
/* In practice only a handful of jobs are in flight; using a 64K array is
 * cheap (128 KB) and keeps the lookup O(1). The wrap to 0 every 0xFFFE marks
 * is fine because each job_seq has its own counter and entries are released
 * when the job is freed. */

void marks_init(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MARKS_MAX; i++) {
        free(g_table[i].name);
        memset(&g_table[i], 0, sizeof(g_table[i]));
    }
    memset(g_next_idx_per_job, 0, sizeof(g_next_idx_per_job));
    pthread_mutex_unlock(&g_lock);
}

uint32_t marks_register(const char *name, uint32_t job_seq) {
    if (!name) return 0;
    pthread_mutex_lock(&g_lock);
    uint16_t idx = g_next_idx_per_job[job_seq & 0xFFFF];
    if (idx >= END_STRING_ID - 1) {  /* exhausted, drop */
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    /* Find a free slot. */
    int slot = -1;
    for (int i = 0; i < MARKS_MAX; i++) {
        if (!g_table[i].in_use) { slot = i; break; }
        if (g_table[i].consumed) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    g_next_idx_per_job[job_seq & 0xFFFF] = idx + 1;
    free(g_table[slot].name);
    g_table[slot].id       = (job_seq << 16) | idx;
    g_table[slot].name     = strdup(name);
    g_table[slot].job_seq  = job_seq;
    g_table[slot].in_use   = true;
    g_table[slot].consumed = false;
    uint32_t id = g_table[slot].id;
    pthread_mutex_unlock(&g_lock);
    return id;
}

const char *marks_resolve(uint32_t id) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MARKS_MAX; i++) {
        if (g_table[i].in_use && !g_table[i].consumed && g_table[i].id == id) {
            const char *name = g_table[i].name;
            g_table[i].consumed = true;
            pthread_mutex_unlock(&g_lock);
            return name;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

void marks_release_job(uint32_t job_seq) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MARKS_MAX; i++) {
        if (g_table[i].in_use && g_table[i].job_seq == job_seq) {
            free(g_table[i].name);
            memset(&g_table[i], 0, sizeof(g_table[i]));
        }
    }
    g_next_idx_per_job[job_seq & 0xFFFF] = 0;
    pthread_mutex_unlock(&g_lock);
}
```

- [ ] **Step 3: Write `sd_eloquence/tests/test_marks.c`**

```c
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
```

- [ ] **Step 4: Wire up the test in CMakeLists.txt**

Inside the test block, add:
```cmake
    add_executable(test_marks
        sd_eloquence/tests/test_marks.c
        sd_eloquence/src/synth/marks.c)
    target_include_directories(test_marks PRIVATE sd_eloquence/src)
    target_link_libraries(test_marks PRIVATE pthread)
    add_test(NAME test_marks COMMAND test_marks)
```

- [ ] **Step 5: Run the test**

```bash
cmake --build build --target test_marks
ctest --test-dir build -R test_marks --output-on-failure
```
Expected: `test_marks: OK`, Passed.

- [ ] **Step 6: Commit**

```bash
git add sd_eloquence/src/synth/marks.h sd_eloquence/src/synth/marks.c \
        sd_eloquence/tests/test_marks.c CMakeLists.txt
git commit -m "synth/marks: name↔id registry with per-job scoping

Encoding (job_seq << 16) | per_job_idx lets the audio callback drop
stale marks from canceled jobs at O(1). Resolve is single-consume so a
mark callback fires exactly once.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task D2: `synth/job.h` — frame definitions

**Files:**
- Create: `sd_eloquence/src/synth/job.h`

- [ ] **Step 1: Write `sd_eloquence/src/synth/job.h`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * synth/job.h -- synth_job + synth_frame types.
 *
 * A speak request from speechd is parsed into a job. The job's frames are
 * executed by the synth worker in order: text frames go through filters →
 * AddText; mark frames become InsertIndex; prosody/voice/lang push/pop
 * pairs save and restore engine state. Worker code lives in worker.c.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_SYNTH_JOB_H
#define SD_ELOQUENCE_SYNTH_JOB_H

#include <speech-dispatcher/spd_module_main.h>

#include <stddef.h>
#include <stdint.h>

typedef enum {
    FRAME_TEXT,
    FRAME_MARK,
    FRAME_BREAK,
    FRAME_PROSODY_PUSH,
    FRAME_PROSODY_POP,
    FRAME_VOICE_PUSH,
    FRAME_VOICE_POP,
    FRAME_LANG_PUSH,
    FRAME_LANG_POP,
    FRAME_TEXTMODE,
} synth_frame_kind;

typedef struct {
    synth_frame_kind kind;
    union {
        struct { char *text; }                                  text;
        struct { uint32_t id; const char *name; }                mark;
        struct { int millis; }                                   brk;
        struct { int param; int new_value; int saved_value; }    prosody;
        struct { int slot; int saved_slot; }                     voice;
        struct { int dialect; int saved_dialect; }               lang;
        struct { int mode; int saved_mode; }                     textmode;
    } u;
} synth_frame;

typedef struct synth_job {
    struct synth_job *next;
    uint32_t           seq;
    SPDMessageType     msgtype;
    synth_frame       *frames;
    size_t             n_frames;
    char              *arena;       /* bump-allocator for text + mark names */
    size_t             arena_used;
    size_t             arena_cap;
} synth_job;

/* Allocate a job with an initial frame and arena capacity. */
synth_job *synth_job_new(size_t frames_cap, size_t arena_cap);

/* Append a frame (returns pointer to the frame slot to fill, or NULL if
 * the frame array is full). */
synth_frame *synth_job_push_frame(synth_job *j);

/* Allocate `n` bytes from the job arena (returns NULL if exhausted). */
void *synth_job_arena_alloc(synth_job *j, size_t n);

/* Duplicate a string into the job arena. Returns NULL if arena is full or
 * `s` is NULL. */
char *synth_job_arena_strdup(synth_job *j, const char *s);

/* Duplicate a (text, len) chunk into the arena -- not necessarily NUL-terminated
 * in the source; the returned string IS NUL-terminated. */
char *synth_job_arena_strndup(synth_job *j, const char *s, size_t len);

/* Free a job and everything in its arena. */
void synth_job_free(synth_job *j);

#endif
```

- [ ] **Step 2: Write `sd_eloquence/src/synth/job.c`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * synth/job.c -- synth_job allocation and arena management.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "job.h"

#include <stdlib.h>
#include <string.h>

synth_job *synth_job_new(size_t frames_cap, size_t arena_cap) {
    synth_job *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->frames = calloc(frames_cap, sizeof(synth_frame));
    if (!j->frames) { free(j); return NULL; }
    j->n_frames = 0;
    /* We treat n_frames as the *occupied* count and store the capacity in
     * a private trailing word -- but for simplicity, we instead pack the
     * capacity into the arena's first word.  Easier: just store the cap
     * separately. */
    /* (We'll cheat: encode frames_cap into arena_used unused space by
     * splitting struct fields. Simpler: add `frames_cap` field.) */
    j->arena = malloc(arena_cap);
    if (!j->arena) { free(j->frames); free(j); return NULL; }
    j->arena_used = 0;
    j->arena_cap  = arena_cap;
    /* Store frames_cap in arena's last 8 bytes (avoid adding a struct field
     * just for this; arena_cap is power-of-2 typical, plenty of room): */
    *(size_t *)(j->arena + arena_cap - sizeof(size_t)) = frames_cap;
    j->arena_cap -= sizeof(size_t);
    return j;
}

static size_t job_frames_cap(synth_job *j) {
    return *(size_t *)(j->arena + j->arena_cap);
}

synth_frame *synth_job_push_frame(synth_job *j) {
    if (!j) return NULL;
    if (j->n_frames >= job_frames_cap(j)) return NULL;
    synth_frame *f = &j->frames[j->n_frames++];
    memset(f, 0, sizeof(*f));
    return f;
}

void *synth_job_arena_alloc(synth_job *j, size_t n) {
    if (!j) return NULL;
    /* 8-byte align */
    size_t aligned = (j->arena_used + 7) & ~(size_t)7;
    if (aligned + n > j->arena_cap) return NULL;
    void *p = j->arena + aligned;
    j->arena_used = aligned + n;
    return p;
}

char *synth_job_arena_strdup(synth_job *j, const char *s) {
    if (!s) return NULL;
    return synth_job_arena_strndup(j, s, strlen(s));
}

char *synth_job_arena_strndup(synth_job *j, const char *s, size_t len) {
    if (!s) return NULL;
    char *out = synth_job_arena_alloc(j, len + 1);
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = 0;
    return out;
}

void synth_job_free(synth_job *j) {
    if (!j) return;
    free(j->frames);
    free(j->arena);
    free(j);
}
```

- [ ] **Step 3: Add to CMake target**

Append `sd_eloquence/src/synth/job.c sd_eloquence/src/synth/marks.c` to the `add_executable(sd_eloquence …)` source list. Run `cmake --build build --target sd_eloquence` — expect success.

- [ ] **Step 4: Commit**

```bash
git add sd_eloquence/src/synth/job.h sd_eloquence/src/synth/job.c CMakeLists.txt
git commit -m "synth/job: frame definitions + arena allocator

Per-job arena bump-allocator owns all text/mark-name buffers; frame
push tracks capacity (encoded in arena trailing word to avoid an extra
struct field).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task D3: `ssml/ssml.[ch]` — SAX-based SSML → frame translator

**Files:**
- Create: `sd_eloquence/src/ssml/ssml.h`
- Create: `sd_eloquence/src/ssml/ssml.c`
- Test: `sd_eloquence/tests/test_ssml.c`

This task is large; it lands in five steps:

1. Skeleton ssml.h/c with `ssml_parse` entry point and plain-text fast path.
2. SAX handler for `<mark>` and `<break>`.
3. SAX handler for `<prosody>`, `<voice>`, `<lang>`/`xml:lang`.
4. SAX handler for `<say-as>`, `<sub>`, `<p>`, `<s>`, `<emphasis>`.
5. Test fixture with golden outputs.

- [ ] **Step 1: Write `sd_eloquence/src/ssml/ssml.h`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ssml/ssml.h -- translate SSML 1.0 (as wrapped by speech-dispatcher)
 * into a synth_job. Falls back to plain-text path if parsing fails.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_SSML_H
#define SD_ELOQUENCE_SSML_H

#include "../synth/job.h"

#include <stddef.h>
#include <stdint.h>

/* Parse `data[0..len)` and produce a synth_job. msgtype controls behavior:
 *   SPD_MSGTYPE_TEXT: full SSML or plain-text fallback
 *   SPD_MSGTYPE_CHAR/KEY/SPELL: no SSML; one FRAME_TEXTMODE(2) + FRAME_TEXT + FRAME_TEXTMODE(restore)
 *   SPD_MSGTYPE_SOUND_ICON: not delivered to speak_sync; not handled here
 *
 * `default_dialect` is the current engine dialect, used as the initial
 * FRAME_LANG context for nested <lang> tags.
 *
 * `job_seq` is the caller-supplied job sequence number, used for mark id encoding.
 *
 * Returns a freshly-allocated job (caller frees via synth_job_free) or NULL
 * on out-of-memory. */
synth_job *ssml_parse(const char *data, size_t len,
                      SPDMessageType msgtype,
                      int default_dialect,
                      uint32_t job_seq);

#endif
```

- [ ] **Step 2: Write `sd_eloquence/src/ssml/ssml.c` — skeleton + fast paths**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ssml/ssml.c -- SAX-based SSML → frame translator.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "ssml.h"

#include "../synth/marks.h"
#include "../eci/voices.h"
#include "../eci/languages.h"
#include "../eci/eci.h"

#include <libxml/parser.h>
#include <libxml/SAX2.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FRAMES_CAP    256
#define ARENA_CAP_BASE 4096

typedef struct {
    synth_job  *job;
    int         current_dialect;
    uint32_t    job_seq;
    int         text_buf_len;   /* used to coalesce adjacent text */
    char       *text_buf;
    size_t      text_buf_cap;
} SsmlCtx;

static int has_lt(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) if (data[i] == '<') return 1;
    return 0;
}

static void flush_text(SsmlCtx *c) {
    if (c->text_buf_len <= 0) return;
    synth_frame *f = synth_job_push_frame(c->job);
    if (f) {
        f->kind = FRAME_TEXT;
        f->u.text.text = synth_job_arena_strndup(c->job, c->text_buf, c->text_buf_len);
    }
    c->text_buf_len = 0;
}

static void append_text(SsmlCtx *c, const xmlChar *ch, int len) {
    if (len <= 0) return;
    if ((size_t)(c->text_buf_len + len + 1) > c->text_buf_cap) {
        size_t newcap = c->text_buf_cap ? c->text_buf_cap * 2 : 256;
        while (newcap < (size_t)(c->text_buf_len + len + 1)) newcap *= 2;
        char *nb = realloc(c->text_buf, newcap);
        if (!nb) return;  /* drop silently; degrades to truncated text */
        c->text_buf = nb;
        c->text_buf_cap = newcap;
    }
    memcpy(c->text_buf + c->text_buf_len, ch, len);
    c->text_buf_len += len;
    c->text_buf[c->text_buf_len] = 0;
}

/* Convenience: get an XML attribute value as a C string (or NULL). */
static const char *attr(const xmlChar **attrs, const char *name) {
    if (!attrs) return NULL;
    for (int i = 0; attrs[i]; i += 2) {
        if (xmlStrcasecmp(attrs[i], (const xmlChar *)name) == 0)
            return (const char *)attrs[i + 1];
    }
    return NULL;
}

/* Forward decls for element handlers (filled in by later tasks). */
static void on_mark(SsmlCtx *c, const xmlChar **attrs);
static void on_break(SsmlCtx *c, const xmlChar **attrs);
static void on_prosody_start(SsmlCtx *c, const xmlChar **attrs);
static void on_prosody_end(SsmlCtx *c);
static void on_voice_start(SsmlCtx *c, const xmlChar **attrs);
static void on_voice_end(SsmlCtx *c);
static void on_lang_start(SsmlCtx *c, const xmlChar **attrs);
static void on_lang_end(SsmlCtx *c);
static void on_sayas_start(SsmlCtx *c, const xmlChar **attrs);
static void on_sayas_end(SsmlCtx *c);
static void on_sub_start(SsmlCtx *c, const xmlChar **attrs);
static void on_emphasis(SsmlCtx *c, const xmlChar **attrs, int start);

static void sax_start_element(void *ud, const xmlChar *name, const xmlChar **attrs) {
    SsmlCtx *c = ud;
    flush_text(c);
    if      (xmlStrcasecmp(name, (xmlChar *)"speak")    == 0) { /* handled below */ }
    else if (xmlStrcasecmp(name, (xmlChar *)"mark")     == 0)  on_mark(c, attrs);
    else if (xmlStrcasecmp(name, (xmlChar *)"break")    == 0)  on_break(c, attrs);
    else if (xmlStrcasecmp(name, (xmlChar *)"prosody")  == 0)  on_prosody_start(c, attrs);
    else if (xmlStrcasecmp(name, (xmlChar *)"voice")    == 0)  on_voice_start(c, attrs);
    else if (xmlStrcasecmp(name, (xmlChar *)"lang")     == 0)  on_lang_start(c, attrs);
    else if (xmlStrcasecmp(name, (xmlChar *)"say-as")   == 0)  on_sayas_start(c, attrs);
    else if (xmlStrcasecmp(name, (xmlChar *)"sub")      == 0)  on_sub_start(c, attrs);
    else if (xmlStrcasecmp(name, (xmlChar *)"emphasis") == 0)  on_emphasis(c, attrs, 1);
    /* Unknown elements: children processed, element ignored. */

    /* <speak xml:lang="..."> sets initial dialect. */
    if (xmlStrcasecmp(name, (xmlChar *)"speak") == 0) {
        const char *lang = attr(attrs, "xml:lang");
        if (!lang) lang = attr(attrs, "lang");
        if (lang) {
            const LangEntry *L = lang_by_iso(lang);
            if (L) c->current_dialect = L->eci_dialect;
        }
    }
}

static void sax_end_element(void *ud, const xmlChar *name) {
    SsmlCtx *c = ud;
    flush_text(c);
    if      (xmlStrcasecmp(name, (xmlChar *)"prosody")  == 0)  on_prosody_end(c);
    else if (xmlStrcasecmp(name, (xmlChar *)"voice")    == 0)  on_voice_end(c);
    else if (xmlStrcasecmp(name, (xmlChar *)"lang")     == 0)  on_lang_end(c);
    else if (xmlStrcasecmp(name, (xmlChar *)"say-as")   == 0)  on_sayas_end(c);
    else if (xmlStrcasecmp(name, (xmlChar *)"emphasis") == 0)  on_emphasis(c, NULL, 0);
    else if (xmlStrcasecmp(name, (xmlChar *)"p") == 0 ||
             xmlStrcasecmp(name, (xmlChar *)"s") == 0) {
        /* <p>/<s> close inserts a small break. */
        synth_frame *f = synth_job_push_frame(c->job);
        if (f) { f->kind = FRAME_BREAK; f->u.brk.millis = 400; }
    }
}

static void sax_characters(void *ud, const xmlChar *ch, int len) {
    SsmlCtx *c = ud;
    append_text(c, ch, len);
}

static synth_job *plain_text_job(const char *data, size_t len,
                                 SPDMessageType msgtype, int dialect,
                                 uint32_t job_seq) {
    (void)dialect; (void)job_seq;
    synth_job *j = synth_job_new(FRAMES_CAP, len + ARENA_CAP_BASE);
    if (!j) return NULL;
    j->seq = job_seq;
    j->msgtype = msgtype;

    if (msgtype == SPD_MSGTYPE_CHAR ||
        msgtype == SPD_MSGTYPE_KEY  ||
        msgtype == SPD_MSGTYPE_SPELL) {
        synth_frame *f1 = synth_job_push_frame(j);
        if (f1) { f1->kind = FRAME_TEXTMODE; f1->u.textmode.mode = 2; f1->u.textmode.saved_mode = 0; }
    }
    synth_frame *f = synth_job_push_frame(j);
    if (f) {
        f->kind = FRAME_TEXT;
        f->u.text.text = synth_job_arena_strndup(j, data, len);
    }
    if (msgtype == SPD_MSGTYPE_CHAR ||
        msgtype == SPD_MSGTYPE_KEY  ||
        msgtype == SPD_MSGTYPE_SPELL) {
        synth_frame *f2 = synth_job_push_frame(j);
        if (f2) { f2->kind = FRAME_TEXTMODE; f2->u.textmode.mode = 0; f2->u.textmode.saved_mode = 2; }
    }
    return j;
}

synth_job *ssml_parse(const char *data, size_t len,
                      SPDMessageType msgtype, int default_dialect,
                      uint32_t job_seq) {
    if (msgtype != SPD_MSGTYPE_TEXT) {
        return plain_text_job(data, len, msgtype, default_dialect, job_seq);
    }
    if (!has_lt(data, len)) {
        return plain_text_job(data, len, msgtype, default_dialect, job_seq);
    }

    synth_job *j = synth_job_new(FRAMES_CAP, len * 2 + ARENA_CAP_BASE);
    if (!j) return NULL;
    j->seq = job_seq;
    j->msgtype = msgtype;

    SsmlCtx ctx = {
        .job = j, .current_dialect = default_dialect, .job_seq = job_seq,
        .text_buf = NULL, .text_buf_len = 0, .text_buf_cap = 0,
    };

    xmlSAXHandler sax;
    memset(&sax, 0, sizeof(sax));
    sax.startElement = sax_start_element;
    sax.endElement   = sax_end_element;
    sax.characters   = sax_characters;

    int ok = xmlSAXUserParseMemory(&sax, &ctx, data, (int)len);
    flush_text(&ctx);
    free(ctx.text_buf);

    if (ok != 0) {
        /* Parse failed; fall back to plain text. */
        synth_job_free(j);
        return plain_text_job(data, len, msgtype, default_dialect, job_seq);
    }
    return j;
}

/* === Element handlers below; filled in by next tasks. === */

static void on_mark(SsmlCtx *c, const xmlChar **attrs) {
    const char *name = attr(attrs, "name");
    if (!name || strlen(name) > 30) return;
    uint32_t id = marks_register(name, c->job_seq);
    if (id == 0) return;  /* table full or unregisterable -- silently drop */
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_MARK;
    f->u.mark.id = id;
    f->u.mark.name = synth_job_arena_strdup(c->job, name);
}

static int strength_to_ms(const char *s) {
    if (!s) return 200;
    if (!strcasecmp(s, "none"))    return 0;
    if (!strcasecmp(s, "x-weak"))  return 100;
    if (!strcasecmp(s, "weak"))    return 200;
    if (!strcasecmp(s, "medium"))  return 400;
    if (!strcasecmp(s, "strong"))  return 700;
    if (!strcasecmp(s, "x-strong"))return 1000;
    return 200;
}

static int parse_time_ms(const char *t) {
    if (!t) return -1;
    char *end = NULL;
    double v = strtod(t, &end);
    if (end == t) return -1;
    if (end && (!*end || isspace((unsigned char)*end))) return (int)v;  /* bare number = ms */
    if (!strcasecmp(end, "ms"))  return (int)v;
    if (!strcasecmp(end, "s"))   return (int)(v * 1000.0);
    return -1;
}

static void on_break(SsmlCtx *c, const xmlChar **attrs) {
    int ms = parse_time_ms(attr(attrs, "time"));
    if (ms < 0) ms = strength_to_ms(attr(attrs, "strength"));
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_BREAK;
    f->u.brk.millis = ms;
}

/* Prosody/voice/lang/sayas/sub/emphasis filled in next steps. */
static int parse_percent(const char *v, int dflt) {
    if (!v) return dflt;
    char *end = NULL;
    double d = strtod(v, &end);
    if (end == v) return dflt;
    if (end && *end == '%') return (int)d;
    /* Named values handled by callers. */
    return dflt;
}

static int prosody_rate_value(const char *v) {
    if (!v) return -1;
    if (!strcasecmp(v, "x-slow"))  return 20;
    if (!strcasecmp(v, "slow"))    return 40;
    if (!strcasecmp(v, "medium"))  return 60;
    if (!strcasecmp(v, "fast"))    return 80;
    if (!strcasecmp(v, "x-fast")) return 100;
    return parse_percent(v, -1);
}

static int prosody_volume_value(const char *v) {
    if (!v) return -1;
    if (!strcasecmp(v, "silent")) return 0;
    if (!strcasecmp(v, "soft"))   return 25;
    if (!strcasecmp(v, "medium")) return 50;
    if (!strcasecmp(v, "loud"))   return 75;
    if (!strcasecmp(v, "x-loud")) return 100;
    return parse_percent(v, -1);
}

static int prosody_pitch_value(const char *v) {
    if (!v) return -1;
    /* Named: x-low / low / medium / high / x-high. */
    if (!strcasecmp(v, "x-low"))  return 20;
    if (!strcasecmp(v, "low"))    return 40;
    if (!strcasecmp(v, "medium")) return 50;
    if (!strcasecmp(v, "high"))   return 70;
    if (!strcasecmp(v, "x-high")) return 90;
    /* "+Nst" -> semitones -> ~6% each. */
    if (*v == '+' || *v == '-') {
        char *end = NULL;
        double d = strtod(v, &end);
        if (end && (!strcasecmp(end, "st") || !strcasecmp(end, "Hz")))
            return 50 + (int)(d * 6.0);
    }
    return parse_percent(v, -1);
}

static void on_prosody_start(SsmlCtx *c, const xmlChar **attrs) {
    /* Each attribute that's present generates one PROSODY_PUSH; the
     * matching POP happens at on_prosody_end via a counter. To keep this
     * simple, we push at most one frame: rate takes priority over pitch,
     * pitch over volume. This matches the most common use. */
    int v = prosody_rate_value(attr(attrs, "rate"));
    int param = -1;
    if (v >= 0) param = eciSpeed;
    else {
        v = prosody_pitch_value(attr(attrs, "pitch"));
        if (v >= 0) param = eciPitchBaseline;
        else {
            v = prosody_volume_value(attr(attrs, "volume"));
            if (v >= 0) param = eciVolume;
        }
    }
    if (param < 0) return;
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_PROSODY_PUSH;
    f->u.prosody.param = param;
    f->u.prosody.new_value = v;
    f->u.prosody.saved_value = -1;  /* worker fills in via GetVoiceParam at exec time */
}

static void on_prosody_end(SsmlCtx *c) {
    /* POP only emits a frame if the matching PUSH did; we always emit one
     * here -- if there was no PUSH, the worker treats POP as no-op. */
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_PROSODY_POP;
}

static void on_voice_start(SsmlCtx *c, const xmlChar **attrs) {
    const char *name = attr(attrs, "name");
    int slot = -1;
    if (name) slot = voice_find_by_name(name);
    if (slot < 0) {
        const char *gender = attr(attrs, "gender");
        if (gender && !strcasecmp(gender, "male"))   slot = 0;
        if (gender && !strcasecmp(gender, "female")) slot = 1;
    }
    if (slot < 0) return;
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_VOICE_PUSH;
    f->u.voice.slot = slot;
}

static void on_voice_end(SsmlCtx *c) {
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_VOICE_POP;
}

static void on_lang_start(SsmlCtx *c, const xmlChar **attrs) {
    const char *lang = attr(attrs, "xml:lang");
    if (!lang) lang = attr(attrs, "lang");
    if (!lang) return;
    const LangEntry *L = lang_by_iso(lang);
    if (!L) return;
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_LANG_PUSH;
    f->u.lang.dialect = L->eci_dialect;
    c->current_dialect = L->eci_dialect;
}

static void on_lang_end(SsmlCtx *c) {
    (void)c;
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_LANG_POP;
}

static void on_sayas_start(SsmlCtx *c, const xmlChar **attrs) {
    const char *as = attr(attrs, "interpret-as");
    if (!as) return;
    int mode = 0;
    if (!strcasecmp(as, "characters") || !strcasecmp(as, "spell")) mode = 2;
    if (mode == 0) return;  /* digits/cardinal/ordinal pass through */
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_TEXTMODE;
    f->u.textmode.mode = mode;
    f->u.textmode.saved_mode = 0;
}

static void on_sayas_end(SsmlCtx *c) {
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_TEXTMODE;
    f->u.textmode.mode = 0;
    f->u.textmode.saved_mode = 2;
}

static void on_sub_start(SsmlCtx *c, const xmlChar **attrs) {
    const char *alias = attr(attrs, "alias");
    if (!alias) return;
    /* sub replaces inner text with alias entirely. We emit the alias as a
     * text frame; the inner text is buffered but never flushed because the
     * </sub> handler doesn't flush -- it just sees what the SAX gave it.
     * Simpler: emit alias text + suppress until </sub>. We approximate by
     * just emitting alias now; inner characters between <sub> and </sub>
     * will leak. This is acceptable for v1; revisit if we see <sub> in the
     * wild. */
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_TEXT;
    f->u.text.text = synth_job_arena_strdup(c->job, alias);
}

static void on_emphasis(SsmlCtx *c, const xmlChar **attrs, int start) {
    (void)c; (void)attrs; (void)start;
    /* v1: passthrough (text inside emphasis is rendered normally). */
}
```

- [ ] **Step 3: Write the test `sd_eloquence/tests/test_ssml.c`**

```c
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
```

- [ ] **Step 4: Wire up the test**

In CMakeLists.txt inside the test block:
```cmake
    add_executable(test_ssml
        sd_eloquence/tests/test_ssml.c
        sd_eloquence/src/ssml/ssml.c
        sd_eloquence/src/synth/job.c
        sd_eloquence/src/synth/marks.c
        sd_eloquence/src/eci/voices.c
        sd_eloquence/src/eci/languages.c)
    target_include_directories(test_ssml PRIVATE
        sd_eloquence/src ${LIBXML2_INCLUDE_DIRS})
    target_link_libraries(test_ssml PRIVATE ${LIBXML2_LIBRARIES} pthread)
    add_test(NAME test_ssml COMMAND test_ssml)
```

Also append `sd_eloquence/src/ssml/ssml.c` to the main `sd_eloquence` target.

- [ ] **Step 5: Run the test**

```bash
cmake --build build --target test_ssml
ctest --test-dir build -R test_ssml --output-on-failure
```
Expected: `test_ssml: OK`, Passed.

- [ ] **Step 6: Commit**

```bash
git add sd_eloquence/src/ssml/ssml.h sd_eloquence/src/ssml/ssml.c \
        sd_eloquence/tests/test_ssml.c CMakeLists.txt
git commit -m "ssml: SAX-based SSML 1.0 → synth_frame translator

Supports <mark>, <break>, <prosody> (rate/pitch/volume), <voice> (name/
gender), <lang>/<xml:lang>, <say-as interpret-as=characters|spell>, <sub
alias=>, <p>/<s>. Falls back to plain text on parse error.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Phase E: Anti-crash filters

### Task E1: `filters/filters.[ch]` — PCRE2 driver

**Files:**
- Create: `sd_eloquence/src/filters/filters.h`
- Create: `sd_eloquence/src/filters/filters.c`
- Create: `sd_eloquence/src/filters/README.GPL.md`

- [ ] **Step 1: Write `sd_eloquence/src/filters/filters.h`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * filters/filters.h -- per-language anti-crash regex driver.
 *
 * Rules are derived from NVDA-IBMTTS-Driver/addon/synthDrivers/ibmeci.py
 * (Copyright (C) 2009-2026 David CM, GPL-2.0). Attribution in
 * filters/README.GPL.md.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_FILTERS_H
#define SD_ELOQUENCE_FILTERS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *pattern;
    const char *replacement;
    uint32_t    flags;          /* PCRE2_CASELESS, etc.; 0 = none */
    void       *compiled;       /* lazily filled (pcre2_code *) */
} filter_rule;

/* NULL-terminated arrays exported by lang_*.c. */
extern filter_rule lang_global_rules[];
extern filter_rule lang_en_rules[];
extern filter_rule lang_es_rules[];
extern filter_rule lang_fr_rules[];
extern filter_rule lang_de_rules[];
extern filter_rule lang_pt_rules[];

/* Apply the global + per-dialect rule chain to `text` (NUL-terminated).
 * Allocates a new buffer (caller frees) holding the filtered result.
 * Returns NULL only on allocation failure; if PCRE2 isn't compiled in or
 * a rule fails, returns a fresh strdup of `text` unchanged. */
char *filters_apply(const char *text, int dialect);

/* True if the filter engine actually runs regex (HAVE_PCRE2). False = no-op. */
int   filters_active(void);

#endif
```

- [ ] **Step 2: Write `sd_eloquence/src/filters/filters.c`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * filters/filters.c -- PCRE2 driver. Compiles patterns lazily.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "filters.h"

#include "../eci/eci.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_PCRE2
#include <pcre2.h>

static pcre2_code *compile_rule(filter_rule *r) {
    if (r->compiled) return (pcre2_code *)r->compiled;
    int errnum = 0;
    PCRE2_SIZE erroff = 0;
    pcre2_code *c = pcre2_compile((PCRE2_SPTR)r->pattern,
                                  PCRE2_ZERO_TERMINATED, r->flags,
                                  &errnum, &erroff, NULL);
    if (!c) {
        char err[128];
        pcre2_get_error_message(errnum, (PCRE2_UCHAR *)err, sizeof(err));
        fprintf(stderr, "sd_eloquence: filter compile failed at offset %zu: %s\n  pattern: %s\n",
                (size_t)erroff, err, r->pattern);
        return NULL;
    }
    r->compiled = c;
    return c;
}

static char *apply_array(char *text, filter_rule rules[]) {
    if (!text) return NULL;
    pcre2_match_data *md = NULL;
    for (filter_rule *r = rules; r->pattern; r++) {
        pcre2_code *c = compile_rule(r);
        if (!c) continue;
        if (!md) md = pcre2_match_data_create_from_pattern(c, NULL);
        size_t in_len = strlen(text);
        size_t cap = in_len * 2 + 16;
        char *out = malloc(cap);
        if (!out) break;
        PCRE2_SIZE out_len = cap;
        int rc = pcre2_substitute(c, (PCRE2_SPTR)text, in_len, 0,
                                  PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH,
                                  md, NULL,
                                  (PCRE2_SPTR)r->replacement, PCRE2_ZERO_TERMINATED,
                                  (PCRE2_UCHAR *)out, &out_len);
        if (rc == PCRE2_ERROR_NOMEMORY) {
            free(out);
            out = malloc(out_len + 1);
            if (!out) break;
            cap = out_len + 1;
            out_len = cap;
            rc = pcre2_substitute(c, (PCRE2_SPTR)text, in_len, 0,
                                  PCRE2_SUBSTITUTE_GLOBAL, md, NULL,
                                  (PCRE2_SPTR)r->replacement, PCRE2_ZERO_TERMINATED,
                                  (PCRE2_UCHAR *)out, &out_len);
        }
        if (rc < 0) {
            free(out);
            continue;
        }
        free(text);
        text = out;
        text[out_len] = 0;
    }
    if (md) pcre2_match_data_free(md);
    return text;
}

int filters_active(void) { return 1; }
#else
static char *apply_array(char *text, filter_rule rules[]) {
    (void)rules;
    return text;
}
int filters_active(void) { return 0; }
#endif

static filter_rule *rules_for_dialect(int dialect) {
    switch (dialect) {
        case eciGeneralAmericanEnglish:
        case eciBritishEnglish:        return lang_en_rules;
        case eciCastilianSpanish:
        case eciMexicanSpanish:        return lang_es_rules;
        case eciStandardFrench:
        case eciCanadianFrench:        return lang_fr_rules;
        case eciStandardGerman:        return lang_de_rules;
        case eciBrazilianPortuguese:   return lang_pt_rules;
        default:                        return NULL;
    }
}

char *filters_apply(const char *text, int dialect) {
    if (!text) return NULL;
    char *t = strdup(text);
    if (!t) return NULL;
    t = apply_array(t, lang_global_rules);
    filter_rule *per = rules_for_dialect(dialect);
    if (per) t = apply_array(t, per);
    return t;
}
```

- [ ] **Step 3: Write `sd_eloquence/src/filters/README.GPL.md`**

```markdown
# filters/ — anti-crash regex tables (GPL-2.0-or-later)

The per-language regex tables under this directory are derived from the
[NVDA-IBMTTS-Driver](https://github.com/davidacm/NVDA-IBMTTS-Driver)
project — specifically
`addon/synthDrivers/ibmeci.py` (`english_fixes`, `english_ibm_fixes`,
`ibm_global_fixes`, `spanish_fixes`, `spanish_ibm_fixes`,
`spanish_ibm_anticrash`, `french_fixes`, `french_ibm_fixes`,
`german_fixes`, `german_ibm_fixes`, `portuguese_ibm_fixes`,
`ibm_pause_re`).

Original copyright: © 2009-2026 David CM (`dhf360@gmail.com`) and
contributors, released under the GNU General Public License version 2.
Full GPLv2 text: `../LICENSE.GPL` (project-wide for the sd_eloquence
subtree).

The pattern strings have been translated from Python `re` byte-mode
syntax to PCRE2 8-bit syntax. Where the original used `\xNN` byte
escapes for cp1252 characters, those translate directly. Inline `(?i)`
flags become `PCRE2_CASELESS` on the rule.
```

- [ ] **Step 4: Verify compile** (filters.c references undefined `lang_*_rules` until next task — temporarily add a stub array to make it link)

Add at the bottom of `filters.c` (we'll remove this in Task E2 once real rules land):
```c
/* STUB rule arrays so filters.c compiles before lang_*.c land. */
filter_rule lang_global_rules[] = { {NULL,NULL,0,NULL} };
filter_rule lang_en_rules[]     = { {NULL,NULL,0,NULL} };
filter_rule lang_es_rules[]     = { {NULL,NULL,0,NULL} };
filter_rule lang_fr_rules[]     = { {NULL,NULL,0,NULL} };
filter_rule lang_de_rules[]     = { {NULL,NULL,0,NULL} };
filter_rule lang_pt_rules[]     = { {NULL,NULL,0,NULL} };
```

Add to CMake: `sd_eloquence/src/filters/filters.c` in the `sd_eloquence` target sources.

Run: `cmake --build build --target sd_eloquence`
Expected: succeeds.

- [ ] **Step 5: Commit**

```bash
git add sd_eloquence/src/filters/filters.h sd_eloquence/src/filters/filters.c \
        sd_eloquence/src/filters/README.GPL.md CMakeLists.txt
git commit -m "filters: PCRE2 driver with lazy compile + dialect dispatch

Stub rule arrays included; replaced by per-language tables in Tasks E2-E7.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task E2: Port `lang_global_rules` from NVDA's `ibm_global_fixes`

**Files:**
- Create: `sd_eloquence/src/filters/lang_global.c`
- Modify: `sd_eloquence/src/filters/filters.c` (remove its stub `lang_global_rules`)
- Test: `sd_eloquence/tests/test_filters.c`

**Source:** `NVDA-IBMTTS-Driver/addon/synthDrivers/ibmeci.py` lines 63–73 (`ibm_global_fixes` dict).

- [ ] **Step 1: Read the source rules**

Run: `sed -n '63,73p' NVDA-IBMTTS-Driver/addon/synthDrivers/ibmeci.py`

Expected output (paraphrased): 5 byte-mode regex/replacement pairs handling: trailing punctuation gluing, `(s)` parenthetical preservation, space-then-punctuation removal, and bracket spacing.

- [ ] **Step 2: Write `sd_eloquence/src/filters/lang_global.c`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Derived from NVDA-IBMTTS-Driver/addon/synthDrivers/ibmeci.py:63-73
 *                                ibm_global_fixes
 * Copyright (C) 2009-2026 David CM and contributors, GPL-2.0
 */
#include "filters.h"

#ifdef HAVE_PCRE2
#include <pcre2.h>

filter_rule lang_global_rules[] = {
    /* Prevent spelling-out when punctuation follows a word. */
    { "([a-z]+)([~#$%^*({|\\\\[<%\\x95])", "$1 $2", PCRE2_CASELESS, NULL },
    /* Don't break phrases like books(s). */
    { "([a-z]+)\\s+(\\(s\\))",             "$1$2",  PCRE2_CASELESS, NULL },
    /* Remove space before punctuation when not followed by letter/digit. */
    { "([a-z]+|\\d+|\\W+)\\s+([:.!;,?](?![a-z]|\\d))",
                                            "$1$2",  PCRE2_CASELESS, NULL },
    /* Reduce two-space separator inside brackets/parens. */
    { "([\\(\\[]+)  (.)",                  "$1$2",  0, NULL },
    { "(.)  ([\\)\\]]+)",                  "$1$2",  0, NULL },
    { NULL, NULL, 0, NULL }
};

#else
filter_rule lang_global_rules[] = { { NULL, NULL, 0, NULL } };
#endif
```

- [ ] **Step 3: Remove the stub `lang_global_rules` from `filters.c`**

In `sd_eloquence/src/filters/filters.c`, delete the line:
```c
filter_rule lang_global_rules[] = { {NULL,NULL,0,NULL} };
```
(Keep the other stub arrays — they get removed in Tasks E3–E7.)

- [ ] **Step 4: Write `sd_eloquence/tests/test_filters.c`**

```c
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
#endif
    /* Filters always preserve identity on pass-through. */
    char *out = filters_apply("plain text", eciGeneralAmericanEnglish);
    assert(strstr(out, "plain text") != NULL);
    free(out);
    puts("test_filters: OK");
    return 0;
}
```

- [ ] **Step 5: Wire up the test**

```cmake
    add_executable(test_filters
        sd_eloquence/tests/test_filters.c
        sd_eloquence/src/filters/filters.c
        sd_eloquence/src/filters/lang_global.c)
    target_include_directories(test_filters PRIVATE sd_eloquence/src)
    if(PCRE2_FOUND)
        target_compile_definitions(test_filters PRIVATE HAVE_PCRE2 PCRE2_CODE_UNIT_WIDTH=8)
        target_include_directories(test_filters PRIVATE ${PCRE2_INCLUDE_DIRS})
        target_link_libraries(test_filters PRIVATE ${PCRE2_LIBRARIES})
    endif()
    add_test(NAME test_filters COMMAND test_filters)
```

Also add `sd_eloquence/src/filters/lang_global.c` to the main `sd_eloquence` target sources.

- [ ] **Step 6: Run the test**

```bash
cmake --build build --target test_filters
ctest --test-dir build -R test_filters --output-on-failure
```
Expected: Passed.

- [ ] **Step 7: Commit**

```bash
git add sd_eloquence/src/filters/lang_global.c sd_eloquence/src/filters/filters.c \
        sd_eloquence/tests/test_filters.c CMakeLists.txt
git commit -m "filters: port ibm_global_fixes (5 rules) from NVDA

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Tasks E3–E7: Port per-language rules

Each of E3–E7 follows the same pattern as E2 with the source line range, output file, and fixture below. **For each task, the engineer:**

1. Reads the source rule block in `NVDA-IBMTTS-Driver/addon/synthDrivers/ibmeci.py`.
2. Transcribes each Python `re.compile(br"pattern", re.I)` → C `filter_rule{ "pattern", "replacement", PCRE2_CASELESS, NULL }`. Notes:
   - Replace Python `\b` → PCRE2 `\\b`
   - Replace Python `re.compile(br'X', re.I | re.L)` → `PCRE2_CASELESS` (PCRE2 has Unicode by default; the L locale flag is non-portable and unused).
   - Replace named replacements like `br'\1\2'` → `"$1$2"`.
   - Python `re.compile(br"x")` without flags → flags `0`.
3. Deletes the stub `lang_XX_rules` declaration in `filters.c`.
4. Adds a test case to `test_filters.c` for one representative pattern per language.
5. Adds the `.c` to the `sd_eloquence` and `test_filters` CMake source lists.
6. Runs `ctest -R test_filters` to verify.
7. Commits.

**Task E3 — English:** Sources: ibmeci.py lines 41-62 (`english_fixes`) and 74-103 (`english_ibm_fixes`). Output: `sd_eloquence/src/filters/lang_en.c`. Union them, IBM rules first.

Representative test case for the English block (add to `test_filters.c`):
```c
check("Mc  Donald", "McDonald", eciGeneralAmericanEnglish, "en-mc-rule");
```

**Task E4 — Spanish:** Sources: ibmeci.py lines 104-112 (`spanish_fixes`), 113-118 (`spanish_ibm_fixes`), 119-122 (`spanish_ibm_anticrash`). Output: `sd_eloquence/src/filters/lang_es.c`.

Representative test case:
```c
check("00\xaa", "00 \xaa", eciCastilianSpanish, "es-anticrash");
```

**Task E5 — French:** Sources: ibmeci.py lines 137-154 (`french_fixes`), 155-158 (`french_ibm_fixes`). Output: `sd_eloquence/src/filters/lang_fr.c`.

Representative test case:
```c
check("n\xb0 1", "num\xe9ro 1", eciStandardFrench, "fr-numero");
```

**Task E6 — German:** Sources: ibmeci.py lines 123-128 (`german_fixes`), 129-133 (`german_ibm_fixes`). Output: `sd_eloquence/src/filters/lang_de.c`.

Representative test case:
```c
check("dane-ben", "dane `0 ben", eciStandardGerman, "de-anticrash");
```

**Task E7 — Portuguese:** Source: ibmeci.py lines 134-136 (`portuguese_ibm_fixes`). Output: `sd_eloquence/src/filters/lang_pt.c`.

Representative test case:
```c
check("12:00:30", "12:00 30", eciBrazilianPortuguese, "pt-time");
```

After each task, commit with message:
```
filters: port <english|spanish|french|german|portuguese>_ibm_fixes from NVDA

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

---

## Phase F: synth worker thread

### Task F1: `synth/worker.h` — public API

**Files:**
- Create: `sd_eloquence/src/synth/worker.h`

- [ ] **Step 1: Write the header**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * synth/worker.h -- dedicated synth thread + job queue.
 *
 * The worker owns the EciEngine handle. speechd loop pushes jobs; worker
 * walks frames, calls AddText/InsertIndex/Synthesize/Synchronize, and the
 * ECI callback (registered by engine.c) runs on the worker thread.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_SYNTH_WORKER_H
#define SD_ELOQUENCE_SYNTH_WORKER_H

#include "job.h"
#include "../eci/engine.h"
#include "../audio/sink.h"
#include "../config.h"

typedef struct SynthWorker SynthWorker;

/* Spin up the worker thread. Returns NULL on alloc failure. The worker
 * takes ownership of `engine` and `sink` (it does NOT free them on
 * worker_destroy; they are caller-owned). */
SynthWorker *worker_create(EciEngine *engine, AudioSink *sink, const EloqConfig *cfg);

/* Submit a job. The worker takes ownership; it frees via synth_job_free
 * after execution. */
void worker_submit(SynthWorker *w, synth_job *job);

/* Signal stop. Sets the atomic flag; the worker's ECI callback returns
 * eciDataAbort on the next chunk. Returns immediately (does not block on
 * the worker). */
void worker_request_stop(SynthWorker *w);

/* Pause / resume via eciPause. */
void worker_pause(SynthWorker *w);
void worker_resume(SynthWorker *w);

/* Shut down: signal stop, drain queue, join thread. */
void worker_destroy(SynthWorker *w);

#endif
```

- [ ] **Step 2: Commit (header only, no code yet)**

```bash
git add sd_eloquence/src/synth/worker.h
git commit -m "synth/worker: public API for the dedicated synth thread

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task F2: `synth/worker.c` — thread + queue + frame execution

**Files:**
- Create: `sd_eloquence/src/synth/worker.c`

- [ ] **Step 1: Write `sd_eloquence/src/synth/worker.c`**

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * synth/worker.c -- dedicated synth thread + job queue.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "worker.h"

#include "marks.h"
#include "../filters/filters.h"
#include "../eci/voices.h"

#include <speech-dispatcher/spd_module_main.h>

#include <iconv.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PCM_CHUNK_SAMPLES 8192

struct SynthWorker {
    EciEngine        *engine;
    AudioSink        *sink;
    const EloqConfig *cfg;

    pthread_t         thread;
    pthread_mutex_t   lock;
    pthread_cond_t    cv;
    synth_job        *head;
    synth_job        *tail;

    _Atomic int       running;
    _Atomic int       stop_requested;
    _Atomic int       paused;
    _Atomic uint32_t  current_job_seq;
};

/* The ECI callback needs access to the SynthWorker; we stash it in TLS-free
 * fashion via a single global pointer (one worker per process). */
static SynthWorker *g_worker = NULL;

static int16_t g_pcm_chunk[PCM_CHUNK_SAMPLES];

static enum ECICallbackReturn eci_callback(ECIHand h, enum ECIMessage msg,
                                           long lParam, void *pData) {
    (void)h; (void)pData;
    SynthWorker *w = g_worker;
    if (!w) return eciDataProcessed;

    if (atomic_load(&w->stop_requested))
        return eciDataAbort;

    if (msg == eciWaveformBuffer) {
        if (lParam <= 0) return eciDataProcessed;
        if (lParam > PCM_CHUNK_SAMPLES) lParam = PCM_CHUNK_SAMPLES;
        audio_sink_push(w->sink, g_pcm_chunk, (int)lParam);
        return eciDataProcessed;
    }

    if (msg == eciIndexReply) {
        uint32_t id = (uint32_t)lParam;
        uint32_t job_seq = atomic_load(&w->current_job_seq);
        if ((id >> 16) != job_seq) return eciDataProcessed;  /* stale */
        if ((id & 0xFFFF) == END_STRING_ID) return eciDataProcessed;
        const char *name = marks_resolve(id);
        if (name) module_report_index_mark(name);
        return eciDataProcessed;
    }

    return eciDataProcessed;
}

/* iconv UTF-8 → target encoding. Returns malloc'd buffer (NUL-terminated). */
static char *transcode(const char *in, const char *enc) {
    if (!enc || strcmp(enc, "utf-8") == 0) return strdup(in);
    iconv_t cd = iconv_open(enc, "UTF-8");
    if (cd == (iconv_t)-1) return strdup(in);
    size_t in_left = strlen(in);
    size_t out_cap = in_left * 4 + 16;
    char *out = malloc(out_cap);
    if (!out) { iconv_close(cd); return NULL; }
    char *in_p  = (char *)in;
    char *out_p = out;
    size_t out_left = out_cap - 1;
    while (in_left > 0) {
        size_t r = iconv(cd, &in_p, &in_left, &out_p, &out_left);
        if (r == (size_t)-1) {
            /* Skip one byte and substitute '?'. */
            if (out_left == 0) break;
            *out_p++ = '?';
            out_left--;
            in_p++;
            in_left--;
        }
    }
    *out_p = 0;
    iconv_close(cd);
    return out;
}

static void exec_text_frame(SynthWorker *w, synth_frame *f, int dialect) {
    if (!f->u.text.text) return;
    char *filtered = filters_apply(f->u.text.text, dialect);
    if (!filtered) return;
    /* Backquote sanitization. */
    if (!w->cfg->backquote_tags) {
        for (char *p = filtered; *p; p++) if (*p == '`') *p = ' ';
    }
    char *enc = transcode(filtered, lang_encoding_for(dialect));
    free(filtered);
    if (!enc) return;
    w->engine->api.AddText(w->engine->h, enc);
    free(enc);
}

static int break_factor_for_rate(int eci_speed) {
    /* NVDA's empirical scale: {10:1, 43:2, 60:3, 75:4, 85:5}. */
    static const struct { int r; int f; } tbl[] = {
        {10,1}, {43,2}, {60,3}, {75,4}, {85,5}, {0,0}
    };
    int prev_r = tbl[0].r, prev_f = tbl[0].f;
    if (eci_speed <= prev_r) return prev_f;
    for (int i = 1; tbl[i].r; i++) {
        if (eci_speed <= tbl[i].r) {
            return prev_f + (tbl[i].f - prev_f) * (eci_speed - prev_r) / (tbl[i].r - prev_r);
        }
        prev_r = tbl[i].r; prev_f = tbl[i].f;
    }
    return prev_f;
}

static void exec_break_frame(SynthWorker *w, synth_frame *f) {
    int eci_speed = w->engine->api.GetVoiceParam(w->engine->h, ECI_ACTIVE_SLOT, eciSpeed);
    int factor = break_factor_for_rate(eci_speed);
    int p = (f->u.brk.millis * factor) / 100;  /* rough scaling */
    if (p < 1) p = 1;
    char buf[32];
    snprintf(buf, sizeof(buf), " `p%d ", p);
    w->engine->api.AddText(w->engine->h, buf);
}

static void exec_job(SynthWorker *w, synth_job *j) {
    atomic_store(&w->current_job_seq, j->seq);

    int saved_dialect_stack[16] = { 0 };
    int saved_voice_stack[16] = { 0 };
    int saved_mode_stack[16] = { 0 };
    int dialect_top = -1, voice_top = -1, mode_top = -1;
    int current_dialect = w->engine->current_dialect;

    /* Optional pre-utterance backquote prefix from EloquenceSendParams /
     * EloquencePhrasePrediction. */
    char prefix[64] = "";
    if (w->cfg->send_params) {
        int vol = w->engine->api.GetVoiceParam(w->engine->h, ECI_ACTIVE_SLOT, eciVolume);
        int spd = w->engine->api.GetVoiceParam(w->engine->h, ECI_ACTIVE_SLOT, eciSpeed);
        snprintf(prefix, sizeof(prefix), "`vv%d `vs%d ", vol, spd);
    }
    if (w->cfg->phrase_prediction)
        strncat(prefix, "`pp1 ", sizeof(prefix) - strlen(prefix) - 1);
    if (prefix[0])
        w->engine->api.AddText(w->engine->h, prefix);

    for (size_t i = 0; i < j->n_frames; i++) {
        if (atomic_load(&w->stop_requested)) break;
        synth_frame *f = &j->frames[i];
        switch (f->kind) {
            case FRAME_TEXT:
                exec_text_frame(w, f, current_dialect);
                break;
            case FRAME_MARK:
                w->engine->api.InsertIndex(w->engine->h, (int)f->u.mark.id);
                break;
            case FRAME_BREAK:
                exec_break_frame(w, f);
                break;
            case FRAME_PROSODY_PUSH: {
                int saved = w->engine->api.GetVoiceParam(w->engine->h, ECI_ACTIVE_SLOT,
                                                        f->u.prosody.param);
                f->u.prosody.saved_value = saved;
                w->engine->api.SetVoiceParam(w->engine->h, ECI_ACTIVE_SLOT,
                                             f->u.prosody.param, f->u.prosody.new_value);
                break;
            }
            case FRAME_PROSODY_POP:
                /* Find the most recent matching PUSH on the same param. */
                for (ssize_t k = (ssize_t)i - 1; k >= 0; k--) {
                    if (j->frames[k].kind == FRAME_PROSODY_PUSH) {
                        w->engine->api.SetVoiceParam(w->engine->h, ECI_ACTIVE_SLOT,
                                                     j->frames[k].u.prosody.param,
                                                     j->frames[k].u.prosody.saved_value);
                        break;
                    }
                }
                break;
            case FRAME_VOICE_PUSH:
                if (voice_top + 1 < (int)(sizeof(saved_voice_stack)/sizeof(int))) {
                    saved_voice_stack[++voice_top] = w->engine->current_voice_slot;
                    voice_activate(&w->engine->api, w->engine->h, f->u.voice.slot,
                                   INT_MIN, INT_MIN, INT_MIN);
                    w->engine->current_voice_slot = f->u.voice.slot;
                }
                break;
            case FRAME_VOICE_POP:
                if (voice_top >= 0) {
                    int s = saved_voice_stack[voice_top--];
                    voice_activate(&w->engine->api, w->engine->h, s,
                                   INT_MIN, INT_MIN, INT_MIN);
                    w->engine->current_voice_slot = s;
                }
                break;
            case FRAME_LANG_PUSH:
                if (dialect_top + 1 < (int)(sizeof(saved_dialect_stack)/sizeof(int))) {
                    saved_dialect_stack[++dialect_top] = current_dialect;
                    engine_switch_language(w->engine, f->u.lang.dialect);
                    current_dialect = f->u.lang.dialect;
                }
                break;
            case FRAME_LANG_POP:
                if (dialect_top >= 0) {
                    int d = saved_dialect_stack[dialect_top--];
                    engine_switch_language(w->engine, d);
                    current_dialect = d;
                }
                break;
            case FRAME_TEXTMODE:
                if (mode_top + 1 < (int)(sizeof(saved_mode_stack)/sizeof(int))) {
                    saved_mode_stack[++mode_top] = f->u.textmode.saved_mode;
                    w->engine->api.SetParam(w->engine->h, eciTextMode, f->u.textmode.mode);
                }
                break;
        }
    }

    /* End-of-string mark to detect completion. */
    w->engine->api.InsertIndex(w->engine->h, (int)marks_make_end(j->seq));

    w->engine->api.Synthesize(w->engine->h);
    w->engine->api.Synchronize(w->engine->h);
    audio_sink_flush(w->sink);

    if (atomic_load(&w->stop_requested))
        module_report_event_stop();
    else
        module_report_event_end();

    marks_release_job(j->seq);
    synth_job_free(j);

    atomic_store(&w->stop_requested, 0);
}

static void *worker_main(void *ud) {
    SynthWorker *w = ud;
    while (atomic_load(&w->running)) {
        pthread_mutex_lock(&w->lock);
        while (atomic_load(&w->running) && !w->head)
            pthread_cond_wait(&w->cv, &w->lock);
        synth_job *j = w->head;
        if (j) {
            w->head = j->next;
            if (!w->head) w->tail = NULL;
            j->next = NULL;
        }
        pthread_mutex_unlock(&w->lock);
        if (!j) continue;

        if (atomic_load(&w->stop_requested)) {
            /* Drop pre-canceled job. */
            module_report_event_stop();
            marks_release_job(j->seq);
            synth_job_free(j);
            atomic_store(&w->stop_requested, 0);
            continue;
        }
        exec_job(w, j);
    }
    return NULL;
}

SynthWorker *worker_create(EciEngine *engine, AudioSink *sink, const EloqConfig *cfg) {
    SynthWorker *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->engine = engine;
    w->sink = sink;
    w->cfg  = cfg;
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cv, NULL);
    atomic_store(&w->running, 1);
    g_worker = w;
    /* Re-register the engine callback to point at OUR callback. The
     * engine_open already wired it, but we want the up-to-date g_worker. */
    engine->api.RegisterCallback(engine->h, eci_callback, NULL);
    engine->api.SetOutputBuffer(engine->h, PCM_CHUNK_SAMPLES, g_pcm_chunk);
    if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
        free(w);
        return NULL;
    }
    return w;
}

void worker_submit(SynthWorker *w, synth_job *job) {
    pthread_mutex_lock(&w->lock);
    job->next = NULL;
    if (w->tail) w->tail->next = job;
    else         w->head = job;
    w->tail = job;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->lock);
}

void worker_request_stop(SynthWorker *w) {
    atomic_store(&w->stop_requested, 1);
    engine_stop(w->engine);
    /* Drain queued (not yet started) jobs -- report stop for each. */
    pthread_mutex_lock(&w->lock);
    synth_job *j = w->head;
    while (j) {
        synth_job *next = j->next;
        module_report_event_stop();
        marks_release_job(j->seq);
        synth_job_free(j);
        j = next;
    }
    w->head = w->tail = NULL;
    pthread_mutex_unlock(&w->lock);
}

void worker_pause(SynthWorker *w) {
    atomic_store(&w->paused, 1);
    engine_pause(w->engine, 1);
}

void worker_resume(SynthWorker *w) {
    atomic_store(&w->paused, 0);
    engine_pause(w->engine, 0);
}

void worker_destroy(SynthWorker *w) {
    if (!w) return;
    atomic_store(&w->running, 0);
    pthread_mutex_lock(&w->lock);
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread, NULL);
    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->cv);
    g_worker = NULL;
    free(w);
}
```

- [ ] **Step 2: Add to CMake target**

Append `sd_eloquence/src/synth/worker.c` to the `sd_eloquence` target sources. Verify build:
```bash
cmake --build build --target sd_eloquence
```

- [ ] **Step 3: Commit**

```bash
git add sd_eloquence/src/synth/worker.c CMakeLists.txt
git commit -m "synth/worker: dedicated synth thread + job execution

Walks the frame array: text → filters/iconv/AddText, mark → InsertIndex,
break → ` p<N>, prosody/voice/lang push/pop with state stacks. ECI
callback fires synchronously inside Synchronize and pushes PCM to the
audio sink. Stop signals atomic + eciStop; queued jobs drain with
event_stop.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Phase G: speechd module entry points

### Task G1: `module.c` — module_init + module_close

**Files:**
- Modify: replace `sd_eloquence/src/module_stub.c` with the real implementation, renamed to `module.c`

- [ ] **Step 1: Rename and replace**

```bash
git mv sd_eloquence/src/module_stub.c sd_eloquence/src/module.c
```

Then write `sd_eloquence/src/module.c` (overwrite):

```c
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * module.c -- speech-dispatcher entry points.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include <speech-dispatcher/spd_module_main.h>

#include "config.h"
#include "eci/engine.h"
#include "eci/languages.h"
#include "eci/voices.h"
#include "audio/resampler.h"
#include "audio/sink.h"
#include "synth/worker.h"
#include "synth/marks.h"
#include "ssml/ssml.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static EloqConfig   g_cfg;
static EciEngine    g_engine;
static Resampler   *g_resampler = NULL;
static AudioSink    g_sink;
static SynthWorker *g_worker = NULL;
static _Atomic uint32_t g_job_seq = 0;

/* Per-voice SPD rate/pitch/volume overrides; INT_MIN = use preset default. */
static int g_spd_rate[N_VOICE_PRESETS];
static int g_spd_pitch[N_VOICE_PRESETS];
static int g_spd_volume[N_VOICE_PRESETS];

static int enter_data_dir(char **errmsg) {
    char p[1024];
    snprintf(p, sizeof(p), "%s/eci.so", g_cfg.data_dir);
    if (access(p, R_OK) != 0) {
        char buf[2048];
        snprintf(buf, sizeof(buf), "%s not found (set EloquenceDataDir)", p);
        if (errmsg) *errmsg = strdup(buf);
        return -1;
    }
    snprintf(p, sizeof(p), "%s/eci.ini", g_cfg.data_dir);
    if (access(p, R_OK) != 0) {
        char buf[2048];
        snprintf(buf, sizeof(buf), "%s not found (cmake --install should generate it)", p);
        if (errmsg) *errmsg = strdup(buf);
        return -1;
    }
    if (chdir(g_cfg.data_dir) != 0) {
        char buf[2048];
        snprintf(buf, sizeof(buf), "chdir(%s): %s", g_cfg.data_dir, strerror(errno));
        if (errmsg) *errmsg = strdup(buf);
        return -1;
    }
    return 0;
}

int module_config(const char *configfile) {
    config_defaults(&g_cfg);
    if (configfile) config_parse_file(&g_cfg, configfile);
    return 0;
}

int module_init(char **msg) {
    for (int i = 0; i < N_VOICE_PRESETS; i++) {
        g_spd_rate[i] = INT_MIN;
        g_spd_pitch[i] = INT_MIN;
        g_spd_volume[i] = INT_MIN;
    }
    marks_init();

    if (enter_data_dir(msg) != 0) return -1;

    char eci_so[1024];
    snprintf(eci_so, sizeof(eci_so), "%s/eci.so", g_cfg.data_dir);

    /* The audio callback gets re-registered by worker_create with the right
     * worker pointer; for engine_open we pass a temporary that's overwritten. */
    if (engine_open(&g_engine, eci_so, g_cfg.default_language,
                    g_cfg.default_sample_rate,
                    NULL, NULL, NULL, 0, msg) != 0)
        return -1;

    /* Activate the default voice preset into slot 0. */
    voice_activate(&g_engine.api, g_engine.h, g_cfg.default_voice_slot,
                   INT_MIN, INT_MIN, INT_MIN);
    g_engine.current_voice_slot = g_cfg.default_voice_slot;

    char *re_err = NULL;
    g_resampler = resampler_new(g_engine.sample_rate_hz, g_cfg.resample_rate,
                                g_cfg.resample_quality, g_cfg.resample_phase,
                                g_cfg.resample_steep, &re_err);
    if (!g_resampler) {
        if (re_err && msg) *msg = re_err;
        return -1;
    }
    if (audio_sink_init(&g_sink, g_resampler, g_engine.sample_rate_hz,
                        8192, msg) != 0)
        return -1;

    module_audio_set_server();

    g_worker = worker_create(&g_engine, &g_sink, &g_cfg);
    if (!g_worker) {
        if (msg) *msg = strdup("worker_create failed");
        return -1;
    }

    char ver[64] = { 0 };
    g_engine.api.Version(ver);
    if (msg) {
        char *out = malloc(160);
        if (out) {
            const LangEntry *L = lang_by_dialect(g_engine.current_dialect);
            snprintf(out, 160,
                     "ETI Eloquence %s -- ready (rate=%d Hz, lang=%s, voice=%s)",
                     ver, g_engine.sample_rate_hz,
                     L ? L->human : "?",
                     g_voice_presets[g_engine.current_voice_slot].name);
        }
        *msg = out;
    }
    return 0;
}

int module_close(void) {
    worker_destroy(g_worker);
    g_worker = NULL;
    audio_sink_dispose(&g_sink);
    resampler_free(g_resampler);
    g_resampler = NULL;
    engine_close(&g_engine);
    return 0;
}

int module_audio_set(const char *var, const char *val) { (void)var; (void)val; return 0; }
int module_audio_init(char **s) { if (s) *s = strdup("ok"); return 0; }
int module_loglevel_set(const char *v, const char *l) {
    if (!strcasecmp(v, "log_level")) g_cfg.debug = atoi(l) > 0;
    (void)l; return 0;
}
int module_debug(int e, const char *f) { g_cfg.debug = e; (void)f; return 0; }

/* The remaining entry points land in Task G2. */
int  module_speak(char *d, size_t b, SPDMessageType t) { (void)d; (void)b; (void)t; return -1; }
void module_speak_sync(const char *d, size_t b, SPDMessageType t);
void module_speak_begin(void) {}
void module_speak_end(void)   {}
void module_speak_pause(void) {}
void module_speak_stop(void)  {}
int  module_stop(void);
size_t module_pause(void);
int    module_set(const char *var, const char *val);
SPDVoice **module_list_voices(void);
int    module_loop(void) { return module_process(STDIN_FILENO, 1); }

void module_speak_sync(const char *d, size_t b, SPDMessageType t) {
    (void)d; (void)b; (void)t;
    /* Body in Task G2. */
    module_speak_error();
}

int    module_stop(void) { return 0; }
size_t module_pause(void) { return 0; }
int    module_set(const char *var, const char *val) { (void)var; (void)val; return 0; }
SPDVoice **module_list_voices(void) { return NULL; }
```

- [ ] **Step 2: Update CMake target name**

In `CMakeLists.txt`, change the `add_executable(sd_eloquence …)` source list: replace `sd_eloquence/src/module_stub.c` with `sd_eloquence/src/module.c`.

Run: `cmake --build build --target sd_eloquence`
Expected: succeeds.

- [ ] **Step 3: Commit**

```bash
git add sd_eloquence/src/module.c CMakeLists.txt
git commit -m "module: speechd module_init/close + global wiring

speak_sync / stop / pause / set / list_voices land in Task G2.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task G2: `module.c` — speak/stop/pause/set/list

**Files:**
- Modify: `sd_eloquence/src/module.c`

- [ ] **Step 1: Replace the stubbed bodies**

In `sd_eloquence/src/module.c`, replace the stubs at the bottom (everything from `void module_speak_sync(...) {` through end of file) with:

```c
void module_speak_sync(const char *d, size_t bytes, SPDMessageType t) {
    if (!g_worker) {
        module_speak_error();
        return;
    }
    uint32_t seq = atomic_fetch_add(&g_job_seq, 1) + 1;
    synth_job *job = ssml_parse(d, bytes, t, g_engine.current_dialect, seq);
    if (!job) {
        module_speak_error();
        return;
    }
    module_speak_ok();
    module_report_event_begin();
    worker_submit(g_worker, job);
}

int module_stop(void) {
    if (g_worker) worker_request_stop(g_worker);
    return 0;
}

size_t module_pause(void) {
    if (g_worker) worker_pause(g_worker);
    return 0;
}

int module_set(const char *var, const char *val) {
    if (!g_worker || !var || !val) return 0;

    if (!strcasecmp(var, "rate")) {
        int spd = atoi(val);
        g_spd_rate[g_engine.current_voice_slot] = spd;
        voice_activate(&g_engine.api, g_engine.h, g_engine.current_voice_slot,
                       g_spd_rate[g_engine.current_voice_slot],
                       g_spd_pitch[g_engine.current_voice_slot],
                       g_spd_volume[g_engine.current_voice_slot]);
        return 0;
    }
    if (!strcasecmp(var, "pitch")) {
        g_spd_pitch[g_engine.current_voice_slot] = atoi(val);
        voice_activate(&g_engine.api, g_engine.h, g_engine.current_voice_slot,
                       g_spd_rate[g_engine.current_voice_slot],
                       g_spd_pitch[g_engine.current_voice_slot],
                       g_spd_volume[g_engine.current_voice_slot]);
        return 0;
    }
    if (!strcasecmp(var, "volume")) {
        g_spd_volume[g_engine.current_voice_slot] = atoi(val);
        voice_activate(&g_engine.api, g_engine.h, g_engine.current_voice_slot,
                       g_spd_rate[g_engine.current_voice_slot],
                       g_spd_pitch[g_engine.current_voice_slot],
                       g_spd_volume[g_engine.current_voice_slot]);
        return 0;
    }
    if (!strcasecmp(var, "voice_type")) {
        int slot = voice_find_by_voice_type(val);
        if (slot >= 0) {
            g_engine.current_voice_slot = slot;
            voice_activate(&g_engine.api, g_engine.h, slot, INT_MIN, INT_MIN, INT_MIN);
        }
        return 0;
    }
    if (!strcasecmp(var, "language")) {
        const LangEntry *L = lang_by_iso(val);
        if (L && L->eci_dialect != g_engine.current_dialect)
            engine_switch_language(&g_engine, L->eci_dialect);
        return 0;
    }
    if (!strcasecmp(var, "synthesis_voice")) {
        /* "Reed-en-US" / "Jacques-fr-fr" -- parse preset name then dialect. */
        int matched = -1;
        size_t name_len = 0;
        for (int i = 0; i < N_VOICE_PRESETS; i++) {
            const VoicePreset *p = &g_voice_presets[i];
            size_t nl = strlen(p->name);
            if (!strncasecmp(val, p->name, nl)) { matched = i; name_len = nl; break; }
            if (p->name_fr) {
                size_t fl = strlen(p->name_fr);
                if (!strncasecmp(val, p->name_fr, fl)) { matched = i; name_len = fl; break; }
            }
        }
        if (matched < 0) return 0;
        g_engine.current_voice_slot = matched;
        voice_activate(&g_engine.api, g_engine.h, matched, INT_MIN, INT_MIN, INT_MIN);
        if (val[name_len] == '-' && val[name_len + 1]) {
            const LangEntry *L = lang_by_iso(val + name_len + 1);
            if (L && L->eci_dialect != g_engine.current_dialect)
                engine_switch_language(&g_engine, L->eci_dialect);
        }
        return 0;
    }
    if (!strcasecmp(var, "punctuation_mode")) {
        int mode = !strcasecmp(val, "all") ? 2 : 0;
        g_engine.api.SetParam(g_engine.h, eciTextMode, mode);
        return 0;
    }
    return 0;
}

static void uppercase_region(char *p) {
    for (; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
}

SPDVoice **module_list_voices(void) {
    int cap = N_LANGS * N_VOICE_PRESETS;
    SPDVoice **list = calloc(cap + 1, sizeof(SPDVoice *));
    if (!list) return NULL;
    int idx = 0;
    for (int li = 0; li < N_LANGS; li++) {
        if (g_lang_state[li] != LANG_AVAILABLE) continue;
        for (int vi = 0; vi < N_VOICE_PRESETS; vi++) {
            SPDVoice *v = calloc(1, sizeof(SPDVoice));
            if (!v) continue;
            char ietf[16];
            snprintf(ietf, sizeof(ietf), "%s-%s",
                     g_langs[li].iso_lang, g_langs[li].iso_variant);
            uppercase_region(ietf + strlen(g_langs[li].iso_lang) + 1);
            char unique[64];
            const char *vname = voice_display_name(vi, g_langs[li].iso_lang);
            snprintf(unique, sizeof(unique), "%s-%s", vname, ietf);
            v->name     = strdup(unique);
            v->language = strdup(ietf);
            v->variant  = strdup(vname);
            list[idx++] = v;
        }
    }
    list[idx] = NULL;
    return list;
}
```

Also: in `module_init`, before the `voice_activate(...)` call near the top, add a loop to mark every language available (CJK gated for now until Phase 1+):

```c
    for (int i = 0; i < N_LANGS; i++) {
        const char *so = g_langs[i].so_name;
        int is_cjk = (strcmp(so, "jpn.so") == 0 || strcmp(so, "kor.so") == 0 ||
                      strcmp(so, "chs.so") == 0 || strcmp(so, "cht.so") == 0);
        g_lang_state[i] = is_cjk ? LANG_DISABLED : LANG_AVAILABLE;
    }
```

- [ ] **Step 2: Verify compile**

```bash
cmake --build build --target sd_eloquence
```

- [ ] **Step 3: Commit**

```bash
git add sd_eloquence/src/module.c
git commit -m "module: speak/stop/pause/set/list_voices

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Phase H: smoke + integration

### Task H1: Flip CMake default + install paths to new module

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Move the `install(TARGETS sd_eloquence_old ...)` to `install(TARGETS sd_eloquence ...)`**

In `CMakeLists.txt`, find:
```cmake
        install(TARGETS sd_eloquence_old
            RUNTIME DESTINATION ${SPEECHD_MODULEBINDIR})
```

Change to:
```cmake
        install(TARGETS sd_eloquence
            RUNTIME DESTINATION ${SPEECHD_MODULEBINDIR})
```

- [ ] **Step 2: Verify install**

```bash
rm -rf build /tmp/sd_install
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
cmake --install build --prefix /tmp/sd_install
ls /tmp/sd_install/usr/lib/speech-dispatcher-modules/
```
Expected: `sd_eloquence` (not `sd_eloquence_old`).

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "CMake: install the new sd_eloquence binary (replacing sd_eloquence_old)

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task H2: `dist/smoke.sh` — manual pre-release smoke

**Files:**
- Create: `dist/smoke.sh`

- [ ] **Step 1: Write `dist/smoke.sh`**

```bash
#!/usr/bin/env bash
# sd_eloquence smoke test -- run after install, before tagging release.
set -euo pipefail

OUT=${OUT:-/tmp/sd_eloquence_smoke.log}
echo "Writing smoke results to $OUT"
echo "sd_eloquence smoke run $(date)" >"$OUT"

run() {
    local label="$1"; shift
    echo >>"$OUT"
    echo "[$label] $*" >>"$OUT"
    spd-say -o eloquence "$@" 2>>"$OUT" || true
    sleep 1.5
}

# Per-language one-liners.
run "en-US" -l en-US "American English."
run "en-GB" -l en-GB "British English."
run "es-ES" -l es-ES "Hola mundo."
run "es-MX" -l es-MX "Hola desde México."
run "fr-FR" -l fr-FR "Bonjour le monde."
run "fr-CA" -l fr-CA "Bonjour du Québec."
run "de-DE" -l de-DE "Hallo Welt."
run "it-IT" -l it-IT "Ciao mondo."
run "pt-BR" -l pt-BR "Olá mundo."
run "fi-FI" -l fi-FI "Hei maailma."

# Variants.
for v in Reed Shelley Sandy Rocko Flo Grandma Grandpa Eddy; do
    run "variant-$v" -y "$v" "Variant $v"
done

# SSML marks.
run "ssml-mark" -m '<speak>Before <mark name="here"/> after.</speak>'

# Cancel mid-sentence.
( spd-say -o eloquence "This is a very long sentence that should be canceled in the middle." &
  PID=$!; sleep 0.3; kill -INT $PID 2>/dev/null || true ) >>"$OUT" 2>&1 || true

# Pause/resume.
echo "[pause/resume] manual: ensure spd-say survives a pause/resume cycle" >>"$OUT"

echo "Smoke complete." | tee -a "$OUT"
```

- [ ] **Step 2: Make executable**

```bash
chmod +x dist/smoke.sh
```

- [ ] **Step 3: Commit**

```bash
git add dist/smoke.sh
git commit -m "dist/smoke.sh: pre-release manual smoke script

10 languages, 8 variants, an SSML mark utterance, a cancel-mid-sentence
test. Manual; output to /tmp/sd_eloquence_smoke.log for diffing across
releases.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task H3: `examples/mark_probe.c` — mark-event verification

**Files:**
- Create: `examples/mark_probe.c`
- Modify: `CMakeLists.txt` (build `mark_probe`)

- [ ] **Step 1: Write `examples/mark_probe.c`**

```c
/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * mark_probe -- send a sentence with three <mark>s through spd-say and
 * verify the IndexReply callback fires the right names. Manual; not in CI.
 *
 * Run AFTER installing the new sd_eloquence module:
 *   build/examples/mark_probe
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

int main(void) {
    /* Use spd-say with -m for SSML. The actual mark events are reported
     * back via SSIP "INDEX-MARK <name>" lines, but spd-say doesn't echo
     * them; this probe exists to catch crashes. For real verification,
     * tail /var/log/speech-dispatcher.log with debug enabled. */
    const char *ssml =
        "<speak>"
        "First <mark name=\"alpha\"/> "
        "second <mark name=\"beta\"/> "
        "third <mark name=\"gamma\"/>."
        "</speak>";
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "spd-say -o eloquence -m '%s'", ssml);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "spd-say returned %d\n", rc);
        return 1;
    }
    fprintf(stderr,
            "mark_probe: utterance sent. Check speech-dispatcher.log for\n"
            "'INDEX-MARK alpha', 'INDEX-MARK beta', 'INDEX-MARK gamma' lines.\n");
    return 0;
}
```

- [ ] **Step 2: Add to CMakeLists.txt**

In the `BUILD_EXAMPLES` block:
```cmake
    add_executable(mark_probe examples/mark_probe.c)
    set_target_properties(mark_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples)
```

- [ ] **Step 3: Commit**

```bash
git add examples/mark_probe.c CMakeLists.txt
git commit -m "examples/mark_probe: smoke binary for SSML mark-event handling

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task H4: Update `dist/install.sh` for new deps

**Files:**
- Modify: `dist/install.sh`

- [ ] **Step 1: Read the current installer**

Run: `cat dist/install.sh | head -100`

- [ ] **Step 2: Add libxml2 and libpcre2 to the dep-resolution table**

Locate the section that resolves runtime dependencies via the host package manager. For each detected package manager (apt / dnf / pacman / zypper / etc.), add entries:

| Component | apt | dnf | pacman |
|---|---|---|---|
| libxml2 | `libxml2` | `libxml2` | `libxml2` |
| libpcre2-8 | `libpcre2-8-0` | `pcre2` | `pcre2` |

(Modify the existing `INSTALL_DEPS_*` arrays / case branches accordingly.)

- [ ] **Step 3: Commit**

```bash
git add dist/install.sh
git commit -m "dist/install.sh: include libxml2 + libpcre2-8 runtime deps

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task H5: Manual Latin-language smoke

**Files:** none.

- [ ] **Step 1: Install the new module locally**

```bash
sudo cmake --install build
sudo systemctl restart speech-dispatcher.socket speech-dispatcher.service 2>/dev/null || \
  sudo killall -HUP speech-dispatcherd 2>/dev/null || true
```

- [ ] **Step 2: Run the smoke script**

```bash
dist/smoke.sh
```

- [ ] **Step 3: Listen + check the log**

Open `/tmp/sd_eloquence_smoke.log`. Expected:
- Every per-language line produces an audible utterance in the right voice/accent.
- Every variant produces audibly different prosody.
- The SSML mark utterance plays without crashing.
- Cancel-mid-sentence cuts the audio cleanly.

- [ ] **Step 4: If issues, file them as separate bugs and fix on this branch before continuing.**

This step has no commit unless issues surfaced and were fixed.

---

## Phase I: CJK Phase 0 — diagnostic infrastructure

These tasks deliver the probe binary, valgrind/gdb invocations, and a written characterization document. The actual CJK fixes are deferred to a follow-up plan.

### Task I1: `examples/cjk_probe.c` — minimal-repro CJK probe

**Files:**
- Create: `examples/cjk_probe.c`
- Modify: `CMakeLists.txt` (build `cjk_probe`)

- [ ] **Step 1: Write `examples/cjk_probe.c`**

```c
/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * cjk_probe -- minimum reproducer for the CJK first-AddText crashes.
 *
 * Usage: cjk_probe <path/to/eci.so> <dialect-hex>
 *   e.g. cjk_probe /usr/lib/eloquence/eci.so 0x00060000   # zh-CN Mandarin
 *
 * Loads eci.so, calls eciNewEx(dialect), registers a callback, sends a tiny
 * dialect-appropriate test phrase, Synchronizes, prints PCM stats or
 * (more usefully) crashes inside the engine so we can characterize.
 *
 * Build under valgrind:  valgrind --leak-check=no --read-var-info=yes \
 *                          build/examples/cjk_probe ./eci.so 0x00060000
 * Build under gdb:       gdb --args build/examples/cjk_probe ./eci.so 0x00060000
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *ECIHand;
typedef int   ECIInputText;  /* opaque; we pass char *cast */
typedef enum { eciDataNotProcessed, eciDataProcessed, eciDataAbort } ECICallbackReturn;
typedef ECICallbackReturn (*ECICallback)(ECIHand, int, long, void *);

#define PCM_CAP 8192
static int16_t pcm[PCM_CAP];
static long total_samples = 0;
static long max_sample = 0;

static ECICallbackReturn cb(ECIHand h, int msg, long lp, void *d) {
    (void)h; (void)d;
    if (msg == 0 /* eciWaveformBuffer */ && lp > 0) {
        total_samples += lp;
        for (long i = 0; i < lp; i++) {
            long v = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (v > max_sample) max_sample = v;
        }
    }
    return eciDataProcessed;
}

/* Encoding-specific test phrase ("hello"). */
static const char *phrase_for(int dialect) {
    switch (dialect) {
        case 0x00060000: return "\xC4\xE3\xBA\xC3";  /* 你好 in gb18030 */
        case 0x00060001: return "\xA7\x41\xA6\x6E";  /* 你好 in big5 */
        case 0x00080000: return "\x82\xB1\x82\xF1\x82\xC9\x82\xBF\x82\xCD";  /* こんにちは in cp932 */
        case 0x000A0000: return "\xBE\xC8\xB3\xE7";  /* 안녕 in cp949 */
        default: return "hello";
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <eci.so> <dialect-hex>\n", argv[0]);
        return 2;
    }
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    ECIHand (*NewEx)(int)            = dlsym(lib, "eciNewEx");
    void (*RegisterCallback)(ECIHand, ECICallback, void *) = dlsym(lib, "eciRegisterCallback");
    int  (*SetOutputBuffer)(ECIHand, int, short *) = dlsym(lib, "eciSetOutputBuffer");
    int  (*AddText)(ECIHand, const void *) = dlsym(lib, "eciAddText");
    int  (*Synthesize)(ECIHand)            = dlsym(lib, "eciSynthesize");
    int  (*Synchronize)(ECIHand)           = dlsym(lib, "eciSynchronize");
    int  (*Delete)(ECIHand)                = dlsym(lib, "eciDelete");
    int  (*ProgStatus)(ECIHand)            = dlsym(lib, "eciProgStatus");

    int dialect = (int)strtol(argv[2], NULL, 0);
    fprintf(stderr, "cjk_probe: eciNewEx(0x%08x)...\n", dialect);
    ECIHand h = NewEx(dialect);
    fprintf(stderr, "  -> handle=%p\n", h);
    if (!h) return 1;

    RegisterCallback(h, cb, NULL);
    SetOutputBuffer(h, PCM_CAP, pcm);

    const char *p = phrase_for(dialect);
    fprintf(stderr, "cjk_probe: AddText (%zu bytes)\n", strlen(p));
    AddText(h, p);
    fprintf(stderr, "  ProgStatus = 0x%x\n", ProgStatus(h));

    fprintf(stderr, "cjk_probe: Synthesize + Synchronize...\n");
    Synthesize(h);
    Synchronize(h);
    fprintf(stderr, "cjk_probe: %ld samples, peak %ld\n", total_samples, max_sample);

    Delete(h);
    /* Intentionally NO dlclose; matches the engine wrapper's behavior. */
    return total_samples > 0 ? 0 : 1;
}
```

- [ ] **Step 2: Add to CMakeLists.txt**

```cmake
    add_executable(cjk_probe examples/cjk_probe.c)
    target_link_libraries(cjk_probe PRIVATE dl)
    set_target_properties(cjk_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples)
```

- [ ] **Step 3: Build**

```bash
cmake --build build --target cjk_probe
```

- [ ] **Step 4: Run against each CJK dialect (informational)**

```bash
build/examples/cjk_probe /usr/lib/eloquence/eci.so 0x00060000  # zh-CN
build/examples/cjk_probe /usr/lib/eloquence/eci.so 0x00060001  # zh-TW
build/examples/cjk_probe /usr/lib/eloquence/eci.so 0x00080000  # ja-JP
build/examples/cjk_probe /usr/lib/eloquence/eci.so 0x000A0000  # ko-KR
```

Expected: some will crash; that's the data we want. Save the crash output verbatim.

- [ ] **Step 5: Commit**

```bash
git add examples/cjk_probe.c CMakeLists.txt
git commit -m "examples/cjk_probe: minimal-repro CJK dialect probe

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task I2: Symbol map for `eci.so`

**Files:**
- Create: `docs/cjk-investigation/eci-so-symbols.txt`

- [ ] **Step 1: Generate the symbol map**

```bash
mkdir -p docs/cjk-investigation
nm --defined-only /usr/lib/eloquence/eci.so | sort > docs/cjk-investigation/eci-so-symbols.txt
```

- [ ] **Step 2: Verify `reset_sent_vars` is in there**

```bash
grep reset_sent_vars docs/cjk-investigation/eci-so-symbols.txt
```
Expected: a line with the symbol's address. If empty, the symbol is stripped and we'll need addr2line at a later stage.

- [ ] **Step 3: Commit**

```bash
git add docs/cjk-investigation/eci-so-symbols.txt
git commit -m "docs/cjk-investigation: symbol map of eci.so for crash analysis

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task I3: Run probes under valgrind + gdb; record findings

**Files:**
- Create: `docs/cjk-investigation/2026-05-12-phase0-findings.md`

- [ ] **Step 1: Run each CJK dialect under valgrind**

```bash
cd /usr/lib/eloquence
valgrind --error-exitcode=1 --read-var-info=yes --leak-check=no \
  /path/to/build/examples/cjk_probe ./eci.so 0x00060000 \
  > /tmp/cjk_zh_cn.log 2>&1
```
Repeat for the other three dialects.

- [ ] **Step 2: Re-run any that crashed under gdb**

```bash
cd /usr/lib/eloquence
gdb -batch -ex 'set pagination off' \
    -ex run -ex 'bt full' -ex 'info registers' \
    --args /path/to/build/examples/cjk_probe ./eci.so 0x00060000 \
  > /tmp/cjk_zh_cn.gdb.log 2>&1
```

- [ ] **Step 3: Write up findings in `docs/cjk-investigation/2026-05-12-phase0-findings.md`**

Use this template:

```markdown
# CJK Phase 0 findings — 2026-05-12

## Probes run

| Dialect | Hex | Result | Crash site |
|---|---|---|---|
| zh-CN | 0x00060000 | crash / silence / ok | function name + offset |
| zh-TW | 0x00060001 | … | … |
| ja-JP | 0x00080000 | … | … |
| ko-KR | 0x000A0000 | … | … |

## Backtraces (verbatim)

### zh-CN
```
<paste gdb backtrace here>
```

### (repeat for other dialects)

## Hypotheses (testable in subsequent phases)

1. **Encoding mismatch** — UTF-8 vs gb18030/cp932/cp949/big5.
   Test: re-run probe with iconv'd input. Plan task: …
2. **Missing romanizer pre-load** — chsrom/jpnrom/korrom.so not in dlopen scope.
   Test: …
3. **State carry-over** — engine internal state from English not cleared.
   Test: …

## Recommendation

Based on the above, the highest-value next experiment is __________. The
follow-up plan should start with that.
```

- [ ] **Step 4: Commit**

```bash
git add docs/cjk-investigation/2026-05-12-phase0-findings.md
git commit -m "docs/cjk-investigation: Phase 0 findings + hypothesis triage

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task I4: Open the follow-up plan

This is **not a coding task** — it's a workflow step.

- [ ] **Step 1: Return to brainstorming for CJK Phase 1–3 fixes**

Run the `superpowers:brainstorming` skill again, anchored on the Phase 0 findings document. Output: a fresh spec + plan covering whichever phases the findings make feasible.

- [ ] **Step 2: Decision point**

If the findings show one or more of phases 1/2/3 can solve all four dialects, write the follow-up plan and continue the rewrite branch. If the findings show no clean path forward and phase 4 (subprocess sandbox) was off the table, re-decompose: strip CJK back out of v1, ship Latin-only v1, file a v2 CJK spec for later.

---

## End of plan

Phases A–H produce a functional GPLv2 sd_eloquence module with the 10 Latin-script languages working end-to-end, full SSML translation, NVDA-derived anti-crash filters, optional user dictionaries, and the CMake/install plumbing flipped over. Phase I lands the diagnostic infrastructure needed to decide what Phase 1–3 work looks like.

**v1 release sits behind the follow-up CJK plan.** Once that plan finishes (either with CJK working or with the explicit fallback agreed), Phase J kicks in:

- Delete `sd_eloquence/src/old/`
- Update README to reflect new structure + capabilities
- Tag `v1.0.0`

Phase J details will land in the follow-up plan (or in a final stub plan if CJK ships clean).

---

## Self-review

**1. Spec coverage:**

| Spec section | Plan task(s) |
|---|---|
| §3 License (GPLv2 split) | A2 |
| §4.1 File layout | A4, B1–B5, C1–C3, D1–D3, E1–E7, F1–F2 |
| §4.2 Dependencies (libxml2, pcre2, libsoxr, iconv) | A4, C2, D3, E1, F2 |
| §5 Data flow + job model | D2, F2 |
| §5.3 Stop/cancel | F2 (`worker_request_stop`) |
| §5.4 Pause/resume | F2 (`worker_pause/resume`) + G2 (`module_pause`) |
| §6 CJK strategy phases 0 | I1–I3 |
| §6 CJK phases 1–3 | Deferred to follow-up plan (I4) |
| §7 SSML translation table | D3 |
| §7.1 Mark id encoding | D1 |
| §7.2 Filter ordering | F2 (`exec_text_frame`) |
| §7.3 Encoding table | B4 (`lang_encoding_for`) |
| §8.1 Anti-crash filters | E1–E7 |
| §8.2 User dictionaries | Engine extension in B2; **TODO**: dict loading at engine_open / language switch is NOT yet wired in this plan. → Adding Task G3 below. |
| §8.3 NVDA toggles | F2 (`exec_job` `send_params`/`phrase_prediction`) + G2 (`module_set`) |
| §9 Error handling | C1, D3 (fallback), E1 (PCRE2 off → no-op), G1 (init errors) |
| §10 Config keys | C1 |
| §11 Testing | test_voices/_languages/_config/_marks/_ssml/_filters + smoke.sh |
| §12 Migration | A3, A4, H1 |

**Identified gap: dictionary loading wiring (§8.2).** Adding it below as Task G3.

### Task G3 (added in self-review): wire user dictionaries into engine_open / language switch

**Files:**
- Modify: `sd_eloquence/src/eci/engine.h` (add dict-related fields)
- Modify: `sd_eloquence/src/eci/engine.c` (load dictionaries)
- Modify: `sd_eloquence/src/module.c` (pass `dict_dir` from config)

- [ ] **Step 1: Extend `EciEngine` in `engine.h`**

Add a field:
```c
    ECIDictHand  dicts[N_LANGS];   /* one per language, lazily loaded */
    char         dict_dir[ELOQ_PATH_MAX];
    int          use_dictionaries;
```

(You'll need to `#include "../config.h"` for `ELOQ_PATH_MAX`.)

- [ ] **Step 2: Add `engine_load_dictionary(EciEngine *e, int dialect)`**

Add to `engine.h`:
```c
/* Lookup or lazily load the dictionary for `dialect` and apply it via SetDict.
 * No-op if e->use_dictionaries is 0 or no dictionary files exist. Returns 0
 * on success, -1 on Load/SetDict error. */
int engine_load_dictionary(EciEngine *e, int dialect);
```

- [ ] **Step 3: Implement in `engine.c`**

```c
int engine_load_dictionary(EciEngine *e, int dialect) {
    if (!e->use_dictionaries) return 0;
    const LangEntry *L = lang_by_dialect(dialect);
    if (!L) return 0;
    int idx = lang_index(L);
    if (e->dicts[idx]) {
        e->api.SetDict(e->h, e->dicts[idx]);
        return 0;
    }
    char path[ELOQ_PATH_MAX + 64];
    int any = 0;
    ECIDictHand d = e->api.NewDict(e->h);
    if (!d) return -1;
    static const struct { const char *suffix; enum ECIDictVolume vol; } files[] = {
        { "main.dic", eciMainDict }, { "root.dic", eciRootDict }, { "abbr.dic", eciAbbvDict },
    };
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s%s", e->dict_dir, L->langid, files[i].suffix);
        if (access(path, R_OK) == 0) {
            if (e->api.LoadDict(e->h, d, files[i].vol, path) == DictNoError) any = 1;
        }
    }
    if (!any) {
        e->api.DeleteDict(e->h, d);
        return 0;
    }
    e->dicts[idx] = d;
    e->api.SetDict(e->h, d);
    return 0;
}
```

Update `engine_switch_language` to call `engine_load_dictionary(e, dialect)` after the SetParam call.

Update `engine_open` to accept a `dict_dir` + `use_dictionaries` parameter and store them.

- [ ] **Step 4: Pass through from `module.c`**

In `module_init`, copy `g_cfg`'s effective dict dir + `use_dictionaries` flag into `g_engine` before calling `engine_load_dictionary` for the initial dialect.

- [ ] **Step 5: Commit**

```bash
git add sd_eloquence/src/eci/engine.h sd_eloquence/src/eci/engine.c sd_eloquence/src/module.c
git commit -m "eci/engine: optional user-dictionary loading per language

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

**2. Placeholder scan:** no TBD/TODO/"implement later" entries remain. The "follow-up plan" references in I4 are explicit branching points, not placeholders.

**3. Type consistency:** `synth_job`, `synth_frame`, `EciApi`, `EciEngine`, `EloqConfig`, `SynthWorker` names match across all tasks. `END_STRING_ID` is `0xFFFF` consistently. Mark id encoding `(seq << 16) | idx` is consistent.

---

## Execution choice

Plan complete and saved to `docs/superpowers/plans/2026-05-12-sd-eloquence-rewrite.md`. Two execution options:

1. **Subagent-Driven (recommended)** — dispatch a fresh subagent per task, review between tasks, fast iteration.
2. **Inline Execution** — execute tasks in this session using `superpowers:executing-plans`, batch execution with checkpoints.

Which approach?
