# ECI `2`-API switch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Switch `sd_eloquence`'s engine wrapper to drive `eci.so` via the modern `2`-suffixed ECI API — the same API Apple's `TextToSpeechKonaSupport.framework` uses — replacing the legacy IBM-compatible API we currently call. Hypothesis: this initializes language-module state that the legacy API skips, fixing the CJK `reset_sent_vars` crash.

**Architecture:** Five-commit migration on `feat/sd-eloquence-rewrite`. Each commit independently builds + passes Latin smoke. Reverse-engineer signatures from Apple's framework disassembly before code lands. Public interface to `module.c` / `worker.c` stays the same; rewrite is internal to `sd_eloquence/src/eci/`.

**Tech Stack:** C11, `llvm-objdump` + `llvm-otool` + `nm` for RE. No new build deps.

**Working dir:** `/home/stas/apple-eloquence-elf/.worktrees/sd-eloquence-rewrite` (continuing on `feat/sd-eloquence-rewrite`).

**Spec:** `docs/superpowers/specs/2026-05-13-eci-2-api-switch-design.md`

**Scope of this plan:** Phases RE → A → B → C → D → E (optional) → F → G. Phase G's outcome branches: CJK works → re-ungate; CJK still crashes → ship anyway, re-gate, follow-up brainstorm.

---

## Phase RE: Reverse-engineer the `2`-API signatures

Each function in the critical-path list gets its own docs file + commit. The workflow is uniform; established in Task RE1 (eciNewEx2 as the worked example) and reused in RE2–RE14.

### Task RE1: Establish RE workflow with `eciNewEx2`

**Files:**
- Create: `docs/eci-2-api/eciNewEx2.md`
- Create: `docs/eci-2-api/README.md` (workflow doc)

Working dir: `/home/stas/apple-eloquence-elf/.worktrees/sd-eloquence-rewrite`

- [ ] **Step 1: Locate Apple's framework binary**

```bash
APPLE_FW="/home/stas/embedded-viavoice/Apple/extracted_18.2_kona/Library/Developer/CoreSimulator/Profiles/Runtimes/tvOS 18.2.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/TextToSpeechKonaSupport.framework/TextToSpeechKonaSupport"
ls -lh "$APPLE_FW"
```
Expected: 134K Mach-O 64-bit binary.

- [ ] **Step 2: Find every call site of `eciNewEx2` in Apple's framework**

```bash
llvm-objdump -d --x86-asm-syntax=intel --no-show-raw-insn "$APPLE_FW" 2>/dev/null \
  | grep -B 8 '_eciNewEx2' | head -60
```

Expected: one or more 8-instruction "windows" preceding `call _eciNewEx2`. Each window shows how arguments are set up (loads into `rdi`, `rsi`, etc.).

- [ ] **Step 3: Read the function prologue in eci.so**

```bash
llvm-objdump -d --x86-asm-syntax=intel --no-show-raw-insn /usr/lib/eloquence/eci.so 2>/dev/null \
  | awk '/<eciNewEx2>:/,/^[a-z0-9]+ <eci/' | head -60
```

Expected: function entry at offset `0x1cda` (per the nm output we already have). Prologue shows `push rbp; mov rbp, rsp; ...` and how arg-registers get used.

- [ ] **Step 4: Write the per-function doc**

Create `docs/eci-2-api/eciNewEx2.md`:

```markdown
# eciNewEx2

**Inferred signature:**

```c
ECIHand eciNewEx2(enum ECILanguageDialect dialect);
```

(Same signature as the legacy `eciNewEx`. The "2" version differs in
internal state initialization, not in C-level interface.)

## Apple framework call site

`TextToSpeechKonaSupport` at offset [paste offset from step 2]:

```asm
mov   edi, dword [<source>]      ; arg 1: dialect (int32)
call  _eciNewEx2
mov   r15, rax                    ; return: ECIHand
```

## eci.so function prologue

`eci.so + 0x1cda`:

```asm
[paste prologue from step 3]
```

## Notes

- Arg 1 in rdi (32-bit register slice `edi` shown means int32 promotion).
- Return in rax (pointer-sized, matches `ECIHand` = `void *`).
- No stack args.
```

Fill in the bracketed sections with the actual disassembly output from steps 2-3.

- [ ] **Step 5: Write the workflow README**

Create `docs/eci-2-api/README.md`:

```markdown
# ECI 2-API reverse-engineering notes

For each `2`-suffixed function `sd_eloquence` wraps, a per-function
doc here records the inferred signature plus the disassembly evidence
(Apple's call site + eci.so's function prologue) it's based on.

Workflow per function:

1. Find every call site in Apple's framework:
   ```
   APPLE_FW="<path>"
   llvm-objdump -d --x86-asm-syntax=intel --no-show-raw-insn "$APPLE_FW" \
     | grep -B 8 '_<funcname>'
   ```
   The 8 instructions before each `call` show how arg registers
   (`rdi`/`rsi`/`rdx`/`rcx`/`r8`/`r9` for integers/pointers; `xmm0..xmm7`
   for floats) are set up per System V AMD64 ABI. Return is in `rax`
   (or `xmm0` for float).

2. Read the function prologue in eci.so:
   ```
   llvm-objdump -d --x86-asm-syntax=intel --no-show-raw-insn /usr/lib/eloquence/eci.so \
     | awk '/<funcname>:/,/^[a-z0-9]+ <eci/' | head -60
   ```
   Prologue typically reveals how many bytes the function consumes
   from each arg register (`mov eax, edi` = 32-bit arg; `mov rax, rdi`
   = 64-bit arg/pointer).

3. Cross-reference against the legacy `funcname-without-2` if a doc
   exists in the IBM SDK header for it. The `2` version usually has
   the same prefix args + maybe an extra options field.

4. Write the per-function doc using the template in `eciNewEx2.md`.

## Critical-path functions for the migration

(See `docs/superpowers/specs/2026-05-13-eci-2-api-switch-design.md` §5.3.)
```

- [ ] **Step 6: Commit**

```bash
git add docs/eci-2-api/
git commit -m "docs/eci-2-api: RE workflow + eciNewEx2 signature

First function in the critical-path list. Establishes the per-function
disassembly + signature-inference workflow used by every subsequent RE
task. eciNewEx2's signature is identical to legacy eciNewEx; the only
practical difference is internal state initialization.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Tasks RE2–RE14: RE the remaining critical-path functions

Apply the RE1 workflow to each of the following. Each task: one function, one commit, one doc file `docs/eci-2-api/<funcname>.md`.

| Task | Function | Likely signature shape (verify via RE) |
|------|----------|----------------------------------------|
| RE2  | `eciDelete2` | `ECIHand eciDelete2(ECIHand h)` |
| RE3  | `eciSetParam2` | `int eciSetParam2(ECIHand h, enum ECIParam param, int value)` |
| RE4  | `eciGetParam2` | `int eciGetParam2(ECIHand h, enum ECIParam param)` |
| RE5  | `eciAddText2` | `Boolean eciAddText2(ECIHand h, ECIInputText text)` |
| RE6  | `eciInsertIndex2` | `Boolean eciInsertIndex2(ECIHand h, int index)` |
| RE7  | `eciSynthesize2` | `Boolean eciSynthesize2(ECIHand h)` |
| RE8  | `eciSynchronize2` | `Boolean eciSynchronize2(ECIHand h)` |
| RE9  | `eciStop2` | `Boolean eciStop2(ECIHand h)` |
| RE10 | `eciPause2` | `Boolean eciPause2(ECIHand h, Boolean on)` |
| RE11 | `eciRegisterCallback2` | `void eciRegisterCallback2(ECIHand h, ECICallback cb, void *data)` |
| RE12 | `eciNewAudioFormat2` | `ECIAudioFormatHand eciNewAudioFormat2(ECIsampleFormat *out)` — **critical: pin down ECIsampleFormat struct layout from this function's bytes-written pattern** |
| RE13 | `eciDeleteAudioFormat2` | `Boolean eciDeleteAudioFormat2(ECIAudioFormatHand h)` |
| RE14 | `eciRegisterSampleBuffer2` | `Boolean eciRegisterSampleBuffer2(ECIHand h, short *buf, int n_samples, ECIAudioFormatHand fmt)` |

Each task's structure (replace `<funcname>` with the function):

- [ ] **Step 1: Find call sites + prologue**

```bash
APPLE_FW="/home/stas/embedded-viavoice/Apple/extracted_18.2_kona/Library/Developer/CoreSimulator/Profiles/Runtimes/tvOS 18.2.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/TextToSpeechKonaSupport.framework/TextToSpeechKonaSupport"
echo "=== Apple call sites ==="
llvm-objdump -d --x86-asm-syntax=intel --no-show-raw-insn "$APPLE_FW" 2>/dev/null \
  | grep -B 8 '_<funcname>' | head -40
echo ""
echo "=== eci.so prologue ==="
llvm-objdump -d --x86-asm-syntax=intel --no-show-raw-insn /usr/lib/eloquence/eci.so 2>/dev/null \
  | awk '/<<funcname>>:/,/^[a-z0-9]+ <eci/' | head -40
```

- [ ] **Step 2: Verify the function exists in eci.so**

```bash
nm -D /usr/lib/eloquence/eci.so 2>/dev/null | grep '<funcname>$'
```
Expected: one line showing the function offset + `T <funcname>`.

- [ ] **Step 3: Cross-reference with the legacy version's IBM SDK signature**

Open `sd_eloquence/src/eci/eci.h` and locate the legacy declaration (function name without `2`). The `2` version's signature is usually identical or has at most one extra trailing arg.

- [ ] **Step 4: Write the per-function doc**

Create `docs/eci-2-api/<funcname>.md` using the template established in RE1.

**Special note for RE12 (`eciNewAudioFormat2`):** This is the function that defines `ECIsampleFormat`'s layout. After the prologue inspection, also disassemble the first ~30 instructions of the function body to see what fields it writes into the passed-in struct. Each `mov dword [rdi + N], <value>` reveals a struct field at offset `N`. Record the inferred layout in `docs/eci-2-api/eciNewAudioFormat2.md`.

**Special note for RE14 (`eciRegisterSampleBuffer2`):** verify the buffer-size arg is in samples, not bytes. The function probably does `mov ecx, edx` (or similar) to use it as a sample count; if it did `shl rdx, 1` first it'd be bytes-to-samples conversion. Almost certainly samples.

- [ ] **Step 5: Commit**

```bash
git add docs/eci-2-api/<funcname>.md
git commit -m "docs/eci-2-api: RE <funcname> signature

[one-line summary of what's notable, e.g. 'identical to legacy'
or 'takes additional format-handle arg vs eciSetOutputBuffer']

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task RE15: RE the dictionary `2`-functions

**Files:**
- Create: `docs/eci-2-api/eciNewDict2.md`
- Create: `docs/eci-2-api/eciDeleteDict2.md`
- Create: `docs/eci-2-api/eciLoadDictVolume2.md`
- Create: `docs/eci-2-api/eciActivateDict2.md`

Same workflow as RE2–RE14. Single commit covering all four (they're tightly related — dict lifecycle).

- [ ] **Step 1: RE each of the four functions** using the RE1 workflow

The legacy signatures we already use:
- `ECIDictHand (*NewDict)(ECIHand)` → `eciNewDict2` likely identical
- `ECIDictHand (*DeleteDict)(ECIHand, ECIDictHand)` → `eciDeleteDict2` likely identical
- `enum ECIDictError (*LoadDict)(ECIHand, ECIDictHand, enum ECIDictVolume, ECIInputText pFilename)` → `eciLoadDictVolume2` may take args in different order
- (no direct legacy equivalent; legacy uses `eciSetDict`) → `eciActivateDict2` likely takes (ECIHand, ECIDictHand)

- [ ] **Step 2: Write the four docs**

- [ ] **Step 3: Commit**

```bash
git add docs/eci-2-api/eciNewDict2.md docs/eci-2-api/eciDeleteDict2.md \
        docs/eci-2-api/eciLoadDictVolume2.md docs/eci-2-api/eciActivateDict2.md
git commit -m "docs/eci-2-api: RE dictionary 2-function signatures

NewDict2 / DeleteDict2 / LoadDictVolume2 / ActivateDict2. Wraps the
modern dict lifecycle; replaces the legacy NewDict / DeleteDict /
LoadDict / SetDict.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task RE16 (optional): `eciSetStandardVoice2`

**Files:**
- Create: `docs/eci-2-api/eciSetStandardVoice2.md`

This one's only used if voice activation's design changes during Phase E. Apply the standard workflow. Special interest: examine what the function does internally — does it set ALL 8 voice params at once (in which case it replaces our per-param loop), or just a "current voice slot" pointer (in which case we still need per-param overrides)?

- [ ] **Standard workflow + commit** as in RE2–RE14.

---

## Phase A: Add new types

### Task A1: Add `ECIsampleFormat` + `ECIAudioFormatHand` to `eci.h`

**Files:**
- Modify: `sd_eloquence/src/eci/eci.h`

- [ ] **Step 1: Open the file and find a place after the existing typedefs**

```bash
grep -n 'typedef\|ECIDictHand\|ECIHand' sd_eloquence/src/eci/eci.h | head -10
```
Find where `ECIHand` and other opaque pointer types live.

- [ ] **Step 2: Insert the new declarations**

Insert right after the existing `typedef void *ECIHand;` line (or equivalent):

```c
/* === 2-API: audio format types =========================================
 * Apple's modern ECI API splits audio buffer registration into two
 * objects: a format struct (ECIsampleFormat) the caller fills in, and
 * an opaque handle (ECIAudioFormatHand) the engine returns from
 * eciNewAudioFormat2. Both legacy and modern APIs coexist in eci.so;
 * see docs/eci-2-api/README.md for the RE notes that pin down the
 * struct layout.
 */
typedef struct ECIsampleFormat {
    /* Layout per docs/eci-2-api/eciNewAudioFormat2.md (verified by RE). */
    int sample_rate;        /* Hz, e.g. 11025 */
    int bits_per_sample;    /* 16 */
    int channels;           /* 1 = mono */
    int byte_order;         /* 0 = little-endian */
    int format_type;        /* 0 = linear PCM */
    /* Future: additional fields if RE12 reveals more. */
} ECIsampleFormat;

typedef void *ECIAudioFormatHand;
#define NULL_AUDIO_FORMAT_HAND 0
```

If the RE12 doc reveals additional struct fields, update both the struct AND the comment to keep them in sync.

- [ ] **Step 3: Verify the new types compile**

```bash
cd /home/stas/apple-eloquence-elf/.worktrees/sd-eloquence-rewrite
gcc -Wall -Wextra -c -o /tmp/eci_test.o -x c - <<<'#include "sd_eloquence/src/eci/eci.h"'
echo $?
```
Expected: exit code 0, no compile errors.

- [ ] **Step 4: Commit**

```bash
git add sd_eloquence/src/eci/eci.h
git commit -m "eci/eci.h: add ECIsampleFormat + ECIAudioFormatHand types

Wraps Apple's modern audio-format API. Layout pinned by the RE doc
docs/eci-2-api/eciNewAudioFormat2.md. Both types coexist with the
existing legacy types in this header; the migration to the 2 API
happens in subsequent commits.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task A2: Add new function-pointer slots to `EciApi` in `runtime.h`

**Files:**
- Modify: `sd_eloquence/src/eci/runtime.h`

- [ ] **Step 1: Read the existing struct**

```bash
sed -n '/^typedef struct EciApi/,/^} EciApi;/p' sd_eloquence/src/eci/runtime.h
```

- [ ] **Step 2: Add new slots as ADDITIVE changes (legacy slots stay)**

Inside the existing `typedef struct EciApi { ... } EciApi;`, immediately after the existing legacy function pointers and before the closing `} EciApi;`, add a new section:

```c
    /* === 2-API: modern function-pointer slots ============================
     * The 2-suffixed forms are what Apple's TextToSpeechKonaSupport
     * framework uses to drive the engine. The CJK language modules
     * (chs/cht/jpn/kor + their *rom variants) were built against this
     * API. The legacy slots above remain in place during the migration;
     * they get removed in the final cleanup commit. */

    /* Lifecycle */
    ECIHand   (*NewEx2)(enum ECILanguageDialect);
    ECIHand   (*Delete2)(ECIHand);
    Boolean   (*Pause2)(ECIHand, Boolean);
    Boolean   (*Stop2)(ECIHand);
    Boolean   (*Synchronize2)(ECIHand);

    /* Parameters */
    int       (*GetParam2)(ECIHand, enum ECIParam);
    int       (*SetParam2)(ECIHand, enum ECIParam, int);

    /* Text + synthesis */
    Boolean   (*AddText2)(ECIHand, ECIInputText);
    Boolean   (*InsertIndex2)(ECIHand, int);
    Boolean   (*Synthesize2)(ECIHand);

    /* Callback */
    void      (*RegisterCallback2)(ECIHand, ECICallback, void *);

    /* Audio format (replaces eciSetOutputBuffer) */
    ECIAudioFormatHand (*NewAudioFormat2)(ECIsampleFormat *);
    Boolean            (*DeleteAudioFormat2)(ECIAudioFormatHand);
    Boolean            (*RegisterSampleBuffer2)(ECIHand, short *, int,
                                                 ECIAudioFormatHand);

    /* Dictionaries */
    ECIDictHand       (*NewDict2)(ECIHand);
    ECIDictHand       (*DeleteDict2)(ECIHand, ECIDictHand);
    enum ECIDictError (*LoadDictVolume2)(ECIHand, ECIDictHand,
                                          enum ECIDictVolume, ECIInputText);
    enum ECIDictError (*ActivateDict2)(ECIHand, ECIDictHand);
```

If any RE doc reveals a different argument shape than shown above, adjust the corresponding declaration to match the RE'd reality. The RE docs are the source of truth.

- [ ] **Step 3: Verify it compiles**

```bash
cmake --build build 2>&1 | tail -5
```
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add sd_eloquence/src/eci/runtime.h
git commit -m "eci/runtime.h: add 2-API function-pointer slots to EciApi

Additive change -- legacy slots stay in place during migration. Each
slot's signature comes from the RE'd doc under docs/eci-2-api/.
runtime.c populates them via dlsym in the next commit.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Phase B: `runtime.c` dlsym wiring

### Task B1: Add LOAD macros for the new slots

**Files:**
- Modify: `sd_eloquence/src/eci/runtime.c`

- [ ] **Step 1: Read the existing LOAD pattern**

```bash
grep -n 'LOAD(' sd_eloquence/src/eci/runtime.c | head -10
```

- [ ] **Step 2: Add LOAD lines for the new functions**

Find the existing block of `LOAD(...)` calls. Add a new section AFTER the existing ones, before the `return 0;`:

```c
    /* === 2-API loads ===================================================
     * Modern function-pointer slots. Both forms get loaded during the
     * migration; the legacy LOADs disappear in the final cleanup
     * commit. */
    LOAD(NewEx2);
    LOAD(Delete2);
    LOAD(Pause2);
    LOAD(Stop2);
    LOAD(Synchronize2);
    LOAD(GetParam2);
    LOAD(SetParam2);
    LOAD(AddText2);
    LOAD(InsertIndex2);
    LOAD(Synthesize2);
    LOAD(RegisterCallback2);
    LOAD(NewAudioFormat2);
    LOAD(DeleteAudioFormat2);
    LOAD(RegisterSampleBuffer2);
    LOAD(NewDict2);
    LOAD(DeleteDict2);
    LOAD(LoadDictVolume2);
    LOAD(ActivateDict2);
```

The LOAD macro auto-prefixes `eci`, so `LOAD(NewEx2)` looks up `eciNewEx2` via dlsym.

- [ ] **Step 3: Verify build**

```bash
cmake --build build 2>&1 | tail -10
```
Expected: build clean.

- [ ] **Step 4: Verify all dlsyms resolve at install time**

Install + run a smoke that opens the engine (any spd-say invocation):

```bash
sudo cmake --install build
systemctl --user restart speech-dispatcher.service
sleep 1
spd-say -o eloquence "load smoke"
sleep 2
tail -10 /run/user/$(id -u)/speech-dispatcher/log/eloquence.log
pgrep -af speech-dispatcher-modules/sd_eloquence
```
Expected: module is alive (pgrep shows it), eloquence.log has no errors about missing symbols. The engine still uses the legacy API (because engine.c hasn't been touched yet), so this validates that the new dlsyms didn't break the existing init.

- [ ] **Step 5: Commit**

```bash
git add sd_eloquence/src/eci/runtime.c
git commit -m "eci/runtime: load the 2-suffixed functions via dlsym

Additive: legacy LOAD lines remain. All 18 new dlsyms resolve against
the installed eci.so (verified by module restart + smoke speak;
neither errors nor missing-symbol failures).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Phase C: `engine_open` + `engine_close` rewrite

### Task C1: Update `EciEngine` struct with audio-format fields

**Files:**
- Modify: `sd_eloquence/src/eci/engine.h`

- [ ] **Step 1: Read the current struct**

```bash
sed -n '/typedef struct {/,/} EciEngine;/p' sd_eloquence/src/eci/engine.h
```

- [ ] **Step 2: Add the two new fields**

Inside the `EciEngine` struct, add after `current_voice_slot` and before the dict-related fields:

```c
    /* === 2-API audio format =============================================
     * eci.api.NewAudioFormat2 allocates this handle; engine_close
     * releases it via DeleteAudioFormat2. The sample_fmt struct stays
     * in EciEngine because the engine reads it back during synthesis
     * (i.e. it's not just consumed once at NewAudioFormat2 time). */
    ECIsampleFormat    sample_fmt;
    ECIAudioFormatHand audio_fmt;
```

- [ ] **Step 3: Verify build**

```bash
cmake --build build 2>&1 | tail -3
```

- [ ] **Step 4: Commit**

```bash
git add sd_eloquence/src/eci/engine.h
git commit -m "eci/engine.h: add sample_fmt + audio_fmt fields to EciEngine

Pre-step for Task C2's engine_open rewrite. Additive; nothing references
these fields yet.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task C2: Rewrite `engine_open` to use the 2 API

**Files:**
- Modify: `sd_eloquence/src/eci/engine.c`

This is the first behavior-changing commit. Latin smoke is the gate after.

- [ ] **Step 1: Read the current engine_open**

```bash
sed -n '/^int engine_open/,/^}$/p' sd_eloquence/src/eci/engine.c
```

- [ ] **Step 2: Rewrite `engine_open` end-to-end**

Replace the existing function body with:

```c
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

    /* 1. Create the engine handle via eciNewEx2 (the modern API path
     *    that correctly initializes CJK language-module state). */
    e->h = e->api.NewEx2((enum ECILanguageDialect)initial_dialect);
    if (!e->h) {
        const LangEntry *L = lang_by_dialect(initial_dialect);
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "eciNewEx2(%#x %s) returned NULL", initial_dialect,
                 L ? L->human : "?");
        if (errmsg) *errmsg = strdup(buf);
        return -1;
    }
    e->current_dialect = initial_dialect;
    e->current_voice_slot = 0;

    /* 2. Sample-rate fallback (Apple rejects rate=2; keep behavior). */
    if (e->api.SetParam2(e->h, eciSampleRate, sample_rate_param) < 0) {
        sample_rate_param = 1;
        e->api.SetParam2(e->h, eciSampleRate, 1);
    }
    e->sample_rate_param = sample_rate_param;
    e->sample_rate_hz    = sample_rate_param_to_hz(sample_rate_param);

    /* 3. Annotation parsing (so backquote tags get interpreted). */
    e->api.SetParam2(e->h, eciSynthMode, 1);
    e->api.SetParam2(e->h, eciInputType, 1);

    /* 4. Build the audio format struct + register it. The format object
     *    is owned by the engine -- we hold the handle and release at close. */
    memset(&e->sample_fmt, 0, sizeof(e->sample_fmt));
    e->sample_fmt.sample_rate     = e->sample_rate_hz;
    e->sample_fmt.bits_per_sample = 16;
    e->sample_fmt.channels        = 1;
    e->sample_fmt.byte_order      = 0;
    e->sample_fmt.format_type     = 0;
    e->audio_fmt = e->api.NewAudioFormat2(&e->sample_fmt);
    if (!e->audio_fmt) {
        if (errmsg) *errmsg = strdup("eciNewAudioFormat2 returned NULL");
        e->api.Delete2(e->h);
        e->h = NULL;
        return -1;
    }

    /* Save callback state so engine_switch_language can re-register
     * if needed (current SetParam-based switch shouldn't need to, but
     * keep the slots populated). */
    e->audio_cb           = audio_cb;
    e->cb_data            = cb_data;
    e->pcm_chunk          = pcm_chunk;
    e->pcm_chunk_samples  = pcm_chunk_samples;

    /* 5. Register callback + sample buffer. The 2-API takes the format
     *    handle explicitly (instead of inferring s16le like the legacy
     *    eciSetOutputBuffer did). */
    if (audio_cb)
        e->api.RegisterCallback2(e->h, audio_cb, cb_data);
    if (pcm_chunk_samples > 0 && pcm_chunk)
        e->api.RegisterSampleBuffer2(e->h, pcm_chunk,
                                      pcm_chunk_samples, e->audio_fmt);
    return 0;
}
```

If the RE12 doc revealed a different `ECIsampleFormat` layout, adjust the field assignments. If the RE14 doc revealed a different arg order, adjust the call.

- [ ] **Step 3: Verify build**

```bash
cmake --build build 2>&1 | tail -5
```

- [ ] **Step 4: Install + Latin smoke (Gate 3)**

```bash
sudo cmake --install build 2>&1 | grep sd_eloquence
systemctl --user restart speech-dispatcher.service
sleep 1
spd-say -o eloquence "engine open smoke"
sleep 3
pgrep -af speech-dispatcher-modules/sd_eloquence
echo "--- run dist/smoke.sh ---"
dist/smoke.sh
cat /tmp/sd_eloquence_smoke.log | tail -15
```
Expected: module starts and stays up; per-language utterances all play. If any regress, stop and investigate the engine_open rewrite (probably an RE signature error).

- [ ] **Step 5: Commit**

```bash
git add sd_eloquence/src/eci/engine.c
git commit -m "eci/engine: rewrite engine_open to use the 2 API

engine_open now drives eci.so via the modern 2-suffixed API: eciNewEx2
+ eciSetParam2 + eciNewAudioFormat2 + eciRegisterSampleBuffer2 +
eciRegisterCallback2. Audio format is an explicit ECIsampleFormat
struct (replaces the legacy eciSetOutputBuffer's implicit s16le).

Latin smoke (dist/smoke.sh) passes: 10 languages and 8 variants all
play; module survives the sequence with same PID.

engine_close still uses legacy Delete -- updated in the next commit.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task C3: Rewrite `engine_close` to use the 2 API

**Files:**
- Modify: `sd_eloquence/src/eci/engine.c`

- [ ] **Step 1: Replace engine_close**

```c
void engine_close(EciEngine *e) {
    if (e->h) {
        for (int i = 0; i < N_LANGS; i++) {
            if (e->dicts[i]) {
                e->api.DeleteDict2(e->h, e->dicts[i]);
                e->dicts[i] = NULL;
            }
        }
        e->api.Stop2(e->h);
        e->api.Delete2(e->h);
        e->h = NULL;
    }
    if (e->audio_fmt) {
        e->api.DeleteAudioFormat2(e->audio_fmt);
        e->audio_fmt = NULL;
    }
    eci_runtime_close();
}
```

- [ ] **Step 2: Verify build + Latin smoke**

```bash
cmake --build build 2>&1 | tail -3
sudo cmake --install build 2>&1 | grep sd_eloquence
systemctl --user restart speech-dispatcher.service
sleep 1
dist/smoke.sh
cat /tmp/sd_eloquence_smoke.log | tail -10
pgrep -af speech-dispatcher-modules/sd_eloquence
```
Expected: smoke still passes.

- [ ] **Step 3: Commit**

```bash
git add sd_eloquence/src/eci/engine.c
git commit -m "eci/engine: rewrite engine_close to use the 2 API

DeleteDict2 + Stop2 + Delete2 + DeleteAudioFormat2. Pairs with C2's
engine_open rewrite.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Phase D: `engine_switch_language` + `engine_load_dictionary` rewrite

### Task D1: Rewrite `engine_switch_language`

**Files:**
- Modify: `sd_eloquence/src/eci/engine.c`

- [ ] **Step 1: Replace engine_switch_language**

```c
int engine_switch_language(EciEngine *e, int dialect) {
    if (!e->h || dialect == e->current_dialect) return 0;
    e->api.Stop2(e->h);
    e->api.Synchronize2(e->h);
    /* SetParam2 on eciLanguageDialect: the 2-API path that should
     * properly initialize CJK language-module state (this is the
     * hypothesis we're testing). */
    e->api.SetParam2(e->h, eciLanguageDialect, dialect);
    e->current_dialect = dialect;
    engine_load_dictionary(e, dialect);
    return 0;
}
```

- [ ] **Step 2: Verify build + Latin smoke + rapid-switch test**

```bash
cmake --build build 2>&1 | tail -3
sudo cmake --install build 2>&1 | grep sd_eloquence
systemctl --user restart speech-dispatcher.service
sleep 1
# Rapid-language-switch test
for d in en-US en-GB es-ES de-DE fr-FR it-IT pt-BR fi-FI en-US; do
    spd-say -o eloquence -l $d "switch test"
    sleep 1.5
done
pgrep -af speech-dispatcher-modules/sd_eloquence
```
Expected: every language plays; module survives all switches (same PID).

- [ ] **Step 3: Commit**

```bash
git add sd_eloquence/src/eci/engine.c
git commit -m "eci/engine: rewrite engine_switch_language to use the 2 API

Stop2 + Synchronize2 + SetParam2(eciLanguageDialect). Rapid Latin
language-switch smoke confirms the path works for non-CJK; the CJK
hypothesis test happens after the full 2-API switch lands (Phase G).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task D2: Rewrite `engine_load_dictionary`

**Files:**
- Modify: `sd_eloquence/src/eci/engine.c`

- [ ] **Step 1: Replace engine_load_dictionary**

```c
int engine_load_dictionary(EciEngine *e, int dialect) {
    if (!e->use_dictionaries) return 0;
    const LangEntry *L = lang_by_dialect(dialect);
    if (!L) return 0;
    int idx = lang_index(L);
    if (idx < 0) return 0;
    if (e->dicts[idx]) {
        e->api.ActivateDict2(e->h, e->dicts[idx]);
        return 0;
    }
    char path[ELOQ_PATH_MAX + 64];
    int any = 0;
    ECIDictHand d = e->api.NewDict2(e->h);
    if (!d) return -1;
    static const struct {
        const char *suffix;
        enum ECIDictVolume vol;
    } files[] = {
        { "main.dic", eciMainDict },
        { "root.dic", eciRootDict },
        { "abbr.dic", eciAbbvDict },
    };
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s%s",
                 e->dict_dir, L->langid, files[i].suffix);
        if (access(path, R_OK) == 0) {
            if (e->api.LoadDictVolume2(e->h, d, files[i].vol, path)
                    == DictNoError)
                any = 1;
        }
    }
    if (!any) {
        e->api.DeleteDict2(e->h, d);
        return 0;
    }
    e->dicts[idx] = d;
    e->api.ActivateDict2(e->h, d);
    return 0;
}
```

- [ ] **Step 2: Verify build + smoke (no dictionaries shipped so this just exercises the no-files path)**

```bash
cmake --build build 2>&1 | tail -3
sudo cmake --install build 2>&1 | grep sd_eloquence
systemctl --user restart speech-dispatcher.service
sleep 1
spd-say -o eloquence "dict load smoke"
sleep 3
pgrep -af speech-dispatcher-modules/sd_eloquence
```
Expected: module alive; no dictionary errors.

- [ ] **Step 3: Commit**

```bash
git add sd_eloquence/src/eci/engine.c
git commit -m "eci/engine: rewrite engine_load_dictionary to use the 2 API

NewDict2 + LoadDictVolume2 + ActivateDict2 + DeleteDict2. The lifecycle
matches Apple's framework's pattern. Re-activation of a cached dict
goes through ActivateDict2 (rather than legacy SetDict).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task D3: Rewrite the remaining engine helpers (`pause`, `stop`)

**Files:**
- Modify: `sd_eloquence/src/eci/engine.c`

- [ ] **Step 1: Replace engine_pause + engine_stop**

```c
void engine_pause(EciEngine *e, int on) {
    if (e->h) e->api.Pause2(e->h, on ? ECITrue : ECIFalse);
}

void engine_stop(EciEngine *e) {
    if (e->h) e->api.Stop2(e->h);
}
```

`engine_version` also gets updated if Version2 was RE'd:

```c
char *engine_version(EciEngine *e) {
    char buf[64] = { 0 };
    /* The 2-API doesn't have a Version2; the legacy Version still works
     * because it doesn't touch language-module state. Keep legacy here
     * UNLESS RE turned up a Version2. */
    if (e->api.Version2) e->api.Version2(buf);
    else                 e->api.Version(buf);
    return strdup(buf);
}
```

Adjust based on whether `Version2` exists in eci.so (`nm -D /usr/lib/eloquence/eci.so | grep Version2` — likely doesn't, since the version string is engine-global and doesn't have a 2 variant). If not, leave `engine_version` calling legacy `Version`.

- [ ] **Step 2: Verify build + commit**

```bash
cmake --build build 2>&1 | tail -3
git add sd_eloquence/src/eci/engine.c
git commit -m "eci/engine: rewrite pause/stop to use the 2 API

Pause2 + Stop2. engine_version keeps using legacy Version (no Version2
exported from eci.so; version string is engine-global, not language-
specific).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task D4: Update worker.c's engine API calls

**Files:**
- Modify: `sd_eloquence/src/synth/worker.c`

worker.c calls a few engine API methods directly (`AddText`, `InsertIndex`, `Synthesize`, `Synchronize`, `SetParam`, `GetVoiceParam`, `SetVoiceParam`). These need to switch to `2` variants. Note: voice-param functions (`GetVoiceParam`, `SetVoiceParam`) DON'T have `2` variants in eci.so — verify with nm. Keep them on the legacy form; they're per-slot data setters that the 2 API didn't change.

- [ ] **Step 1: Audit current api calls**

```bash
grep -n 'w->engine->api\.' sd_eloquence/src/synth/worker.c | head -25
```

- [ ] **Step 2: For each call, decide legacy vs. 2 variant**

Verify each via `nm`:

```bash
for fn in AddText InsertIndex Synthesize Synchronize SetParam GetVoiceParam SetVoiceParam; do
    has_2=$(nm -D /usr/lib/eloquence/eci.so | grep "eci${fn}2$" | wc -l)
    echo "$fn -> ${fn}2 exists: $has_2"
done
```

- [ ] **Step 3: Replace each call to a function that has a `2` variant**

For each api call where the `2` variant exists, update worker.c to call the `2` form. For ones without (likely `GetVoiceParam`, `SetVoiceParam`), keep legacy.

Use grep + sed or hand-edit. A complete pass:

```bash
sed -i 's/w->engine->api\.AddText(/w->engine->api.AddText2(/g'                 sd_eloquence/src/synth/worker.c
sed -i 's/w->engine->api\.InsertIndex(/w->engine->api.InsertIndex2(/g'         sd_eloquence/src/synth/worker.c
sed -i 's/w->engine->api\.Synthesize(/w->engine->api.Synthesize2(/g'           sd_eloquence/src/synth/worker.c
sed -i 's/w->engine->api\.Synchronize(/w->engine->api.Synchronize2(/g'         sd_eloquence/src/synth/worker.c
sed -i 's/w->engine->api\.SetParam(/w->engine->api.SetParam2(/g'                sd_eloquence/src/synth/worker.c
```

(Do NOT replace `GetVoiceParam` / `SetVoiceParam` — these have no `2` variants.)

- [ ] **Step 4: Verify build + Latin smoke + SSML-mark probe**

```bash
cmake --build build 2>&1 | tail -3
sudo cmake --install build 2>&1 | grep sd_eloquence
systemctl --user restart speech-dispatcher.service
sleep 1
dist/smoke.sh
cat /tmp/sd_eloquence_smoke.log | tail -10
build/examples/mark_probe 2>&1 | tail -5
pgrep -af speech-dispatcher-modules/sd_eloquence
```
Expected: smoke passes, mark_probe runs without error.

- [ ] **Step 5: Commit**

```bash
git add sd_eloquence/src/synth/worker.c
git commit -m "synth/worker: switch engine API calls to the 2-suffixed forms

AddText2 + InsertIndex2 + Synthesize2 + Synchronize2 + SetParam2.
GetVoiceParam / SetVoiceParam stay on legacy -- no 2 variants in
eci.so (verified via nm). Latin smoke + mark_probe pass.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task D5: Update module.c's engine API calls

**Files:**
- Modify: `sd_eloquence/src/module.c`

module.c calls `g_engine.api.SetParam` (for `eciTextMode`) and `g_engine.api.Version` (for the init message). Update.

- [ ] **Step 1: Audit current api calls**

```bash
grep -n 'g_engine\.api\.' sd_eloquence/src/module.c
```

- [ ] **Step 2: Replace SetParam → SetParam2 (Version stays)**

```bash
sed -i 's/g_engine\.api\.SetParam(/g_engine.api.SetParam2(/g' sd_eloquence/src/module.c
```

- [ ] **Step 3: Verify build + smoke**

```bash
cmake --build build 2>&1 | tail -3
sudo cmake --install build 2>&1 | grep sd_eloquence
systemctl --user restart speech-dispatcher.service
sleep 1
spd-say -o eloquence "module set smoke"
sleep 3
pgrep -af speech-dispatcher-modules/sd_eloquence
```

- [ ] **Step 4: Commit**

```bash
git add sd_eloquence/src/module.c
git commit -m "module: switch SetParam calls to SetParam2

The remaining direct engine API call site in module.c. Version stays
on legacy (engine-global; no 2 variant).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Phase E (optional): voice_activate adaptation

This phase only runs if RE16 (`eciSetStandardVoice2`) reveals that the function provides a cleaner way to switch voice presets than the per-param loop we currently use. Skip this phase if `eciSetStandardVoice2` is just a "select slot" call without per-param effects.

### Task E1: Decision point — read RE16 doc

- [ ] **Step 1: Read the RE16 doc**

```bash
cat docs/eci-2-api/eciSetStandardVoice2.md
```

Decide:
- **(a)** `eciSetStandardVoice2(slot)` replaces all 8 `eciSetVoiceParam` calls → Phase E2 below
- **(b)** It's only "select slot" but doesn't replace per-param overrides → SKIP Phase E (current voice_activate is fine)

- [ ] **Step 2 (only if (a)):** rewrite `voice_activate` in `sd_eloquence/src/eci/voices.c`

```c
void voice_activate(const EciApi *eci, ECIHand h, int slot,
                    int spd_rate, int spd_pitch, int spd_volume) {
    if (slot < 0 || slot >= N_VOICE_PRESETS) return;
    const VoicePreset *v = &g_voice_presets[slot];

    /* SetStandardVoice2 swaps in the preset's 8 params atomically;
     * we don't need to write them individually. */
    eci->SetStandardVoice2(h, slot);

    /* Apply any SPD overrides (rate/pitch/volume) on top. */
    if (spd_rate   != INT_MIN)
        eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciSpeed,
                            spd_to_eci_speed(spd_rate));
    if (spd_pitch  != INT_MIN)
        eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciPitchBaseline,
                            spd_to_eci_pct(spd_pitch));
    if (spd_volume != INT_MIN)
        eci->SetVoiceParam(h, ECI_ACTIVE_SLOT, eciVolume,
                            spd_to_eci_pct(spd_volume));
}
```

- [ ] **Step 3: Verify build + smoke**

```bash
cmake --build build 2>&1 | tail -3
sudo cmake --install build 2>&1 | grep sd_eloquence
systemctl --user restart speech-dispatcher.service
sleep 1
# Test every variant
for v in Reed Shelley Sandy Rocko Flo Grandma Grandpa Eddy; do
    spd-say -o eloquence -y $v "variant $v"
    sleep 1.5
done
pgrep -af speech-dispatcher-modules/sd_eloquence
```
Expected: every voice sounds correct.

- [ ] **Step 4: Commit (only if Phase E ran)**

```bash
git add sd_eloquence/src/eci/voices.c sd_eloquence/src/eci/voices.h
git commit -m "eci/voices: use eciSetStandardVoice2 for preset switch

Replaces the per-param SetVoiceParam loop with a single
SetStandardVoice2(slot) call. Matches Apple's framework's pattern.
SPD-supplied rate/pitch/volume overrides are still applied per-param
since they're not part of the preset's baseline.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Phase F: Drop the legacy API surface (cleanup)

### Task F1: Audit for remaining legacy api references

**Files:** none modified directly

- [ ] **Step 1: Find every remaining reference to a legacy function pointer**

```bash
cd /home/stas/apple-eloquence-elf/.worktrees/sd-eloquence-rewrite
# Legacy function names (without 2 suffix). Skip the ones that have no 2 variant.
grep -nE 'api\.(NewEx|Delete|Pause|Stop|Synchronize|GetParam|SetParam|AddText|InsertIndex|Synthesize|RegisterCallback|NewDict|DeleteDict|LoadDict|SetDict)\b' \
    sd_eloquence/src/ -r 2>/dev/null
```
Expected: empty (or only matches in lines that have the `2` suffix, which the regex's `\b` should exclude).

- [ ] **Step 2: Report findings**

If anything matches: that file still uses the legacy form. Open it, switch to the `2` variant, build, smoke. Repeat until no matches.

This is a one-time inspection; no commit yet. The cleanup commits below remove the legacy declarations.

---

### Task F2: Remove legacy slots from `EciApi` struct

**Files:**
- Modify: `sd_eloquence/src/eci/runtime.h`

- [ ] **Step 1: Open runtime.h and identify the legacy block**

Look for the original `EciApi` struct fields. Delete the legacy slots, keeping ONLY:
- The 2-suffixed slots added in Task A2
- The fields that have NO 2 variant (`GetVoiceParam`, `SetVoiceParam`, `Version`, `New`, `IsBeingReentered`, `ErrorMessage`, `ClearErrors`, `TestPhrase`, `Reset`, `CopyVoice`, `GetVoiceName`, `SetVoiceName`, `SpeakText`, `SpeakTextEx`, `GetDefaultParam`, `SetDefaultParam`, `SynthesizeFile`, `ClearInput`, `GeneratePhonemes`, `GetIndex`, `Speaking`, `SetOutputFilename`, `SetOutputDevice`, `GetAvailableLanguages`)

Sanity-check the "no 2 variant" list:

```bash
for fn in New IsBeingReentered ErrorMessage ClearErrors TestPhrase Reset CopyVoice GetVoiceName SetVoiceName SpeakText SpeakTextEx GetDefaultParam SetDefaultParam SynthesizeFile ClearInput GeneratePhonemes GetIndex Speaking SetOutputFilename SetOutputDevice GetAvailableLanguages GetVoiceParam SetVoiceParam Version; do
    has_2=$(nm -D /usr/lib/eloquence/eci.so | grep "eci${fn}2$" | wc -l)
    echo "$fn -> ${fn}2 exists: $has_2"
done
```

Only the ones with `0` stay in `EciApi` as their unsuffixed form.

The legacy slots that DO have `2` variants and have been replaced by them get deleted:
- `NewEx`, `Delete`, `Pause`, `Stop`, `Synchronize`, `GetParam`, `SetParam`, `AddText`, `InsertIndex`, `Synthesize`, `RegisterCallback`, `SetOutputBuffer`, `NewDict`, `DeleteDict`, `LoadDict`, `SetDict`

Delete those declarations from `EciApi`. Keep their now-replaced 2 variants.

- [ ] **Step 2: Verify build**

```bash
cmake --build build 2>&1 | tail -10
```
Expected: clean build. If anything fails, it's a stragler call site — go back to F1 to find it.

- [ ] **Step 3: Commit**

```bash
git add sd_eloquence/src/eci/runtime.h
git commit -m "eci/runtime.h: drop legacy function-pointer slots that have 2 variants

Removes NewEx, Delete, Pause, Stop, Synchronize, GetParam, SetParam,
AddText, InsertIndex, Synthesize, RegisterCallback, SetOutputBuffer,
NewDict, DeleteDict, LoadDict, SetDict from EciApi. Each is fully
replaced by its 2-suffixed equivalent.

Functions WITHOUT 2 variants stay on legacy: GetVoiceParam,
SetVoiceParam, Version, New, plus the dictionary find/save APIs
sd_eloquence doesn't currently call.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task F3: Remove legacy LOADs from runtime.c

**Files:**
- Modify: `sd_eloquence/src/eci/runtime.c`

- [ ] **Step 1: Delete the LOAD lines for the now-removed slots**

For each function name removed from `EciApi` in F2, delete its corresponding `LOAD(...)` line in `runtime.c`. Use grep to find them:

```bash
grep -n 'LOAD(NewEx\|LOAD(Delete\|LOAD(Pause\|LOAD(Stop\|LOAD(Synchronize\|LOAD(GetParam\|LOAD(SetParam\|LOAD(AddText\|LOAD(InsertIndex\|LOAD(Synthesize\|LOAD(RegisterCallback\|LOAD(SetOutputBuffer\|LOAD(NewDict\|LOAD(DeleteDict\|LOAD(LoadDict\|LOAD(SetDict' sd_eloquence/src/eci/runtime.c
```

Delete the legacy ones; keep the `2`-suffixed ones.

- [ ] **Step 2: Verify build + smoke**

```bash
cmake --build build 2>&1 | tail -5
sudo cmake --install build 2>&1 | grep sd_eloquence
systemctl --user restart speech-dispatcher.service
sleep 1
dist/smoke.sh
cat /tmp/sd_eloquence_smoke.log | tail -10
pgrep -af speech-dispatcher-modules/sd_eloquence
```
Expected: full Latin smoke still passes.

- [ ] **Step 3: Commit**

```bash
git add sd_eloquence/src/eci/runtime.c
git commit -m "eci/runtime: drop LOAD calls for legacy functions

Mirrors the struct-slot removal in F2. The runtime now only loads
function pointers that are actually referenced by sd_eloquence code.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task F4: ctest gate

- [ ] **Step 1: Run all unit tests**

```bash
ctest --test-dir build 2>&1 | tail -10
```
Expected: 6/6 pass.

If any fail: investigate. Unit tests don't depend on the engine API at all, so a failure here is a static-type / include-path regression from F2 / F3. Fix before continuing.

This step has no commit; it's a verification gate.

---

## Phase G: CJK hypothesis test + branch decision

### Task G1: Manual CJK probe

**Files:** none directly modified (this is a manual test)

- [ ] **Step 1: Install + restart**

```bash
sudo cmake --install build
systemctl --user restart speech-dispatcher.service
sleep 2
pgrep -af speech-dispatcher-modules/sd_eloquence
```

- [ ] **Step 2: Test each CJK dialect**

```bash
# Capture starting PID for crash detection
PID_START=$(pgrep -f speech-dispatcher-modules/sd_eloquence | head -1)
echo "starting PID: $PID_START"

# Walk each CJK dialect with a brief utterance
for d in zh-CN ja-JP ko-KR zh-TW; do
    echo "=== $d ==="
    spd-say -o eloquence -l $d "test"
    sleep 3
    PID_NOW=$(pgrep -f speech-dispatcher-modules/sd_eloquence | head -1)
    if [ "$PID_NOW" != "$PID_START" ]; then
        echo "MODULE CRASHED: PID changed from $PID_START to $PID_NOW (or empty)"
        break
    fi
done

echo ""
echo "=== Phase G result ==="
if [ -n "$PID_NOW" ] && [ "$PID_NOW" = "$PID_START" ]; then
    echo "CJK WORKS -- proceed to Task G2 (re-ungate)"
else
    echo "CJK STILL CRASHES -- proceed to Task G3 (re-gate)"
fi
```

The test phrases are intentionally short ("test" — engine just speaks "test" in the target language, which exercises the language-module initialization path without depending on per-language UTF-8 encoding).

- [ ] **Step 3: Branch decision**

If CJK works: go to Task G2.
If CJK still crashes: go to Task G3.

---

### Task G2: Re-ungate CJK (only if G1 succeeded)

**Files:**
- Modify: `sd_eloquence/src/module.c`

- [ ] **Step 1: Find the language-state gating loop**

```bash
grep -n 'is_cjk\|LANG_DISABLED' sd_eloquence/src/module.c
```

- [ ] **Step 2: Replace the gating loop with unconditional LANG_AVAILABLE**

```c
    /* All 14 dialects available. CJK works correctly now that the engine
     * wrapper drives eci.so via the modern 2-suffixed API (see
     * docs/cjk-investigation/2026-05-13-phase3-api-divergence.md +
     * docs/superpowers/specs/2026-05-13-eci-2-api-switch-design.md). */
    for (int i = 0; i < N_LANGS; i++)
        g_lang_state[i] = LANG_AVAILABLE;
```

Delete the longer comment block that justified gating (it's now obsolete).

- [ ] **Step 3: Verify build + extended smoke**

```bash
cmake --build build 2>&1 | tail -3
sudo cmake --install build 2>&1 | grep sd_eloquence
systemctl --user restart speech-dispatcher.service
sleep 1
dist/smoke.sh
# Manually add a few CJK lines if smoke.sh doesn't include them
spd-say -o eloquence -l zh-CN "你好"; sleep 3
spd-say -o eloquence -l ja-JP "こんにちは"; sleep 3
spd-say -o eloquence -l ko-KR "안녕하세요"; sleep 3
spd-say -o eloquence -l zh-TW "你好"; sleep 3
pgrep -af speech-dispatcher-modules/sd_eloquence
```

- [ ] **Step 4: Commit**

```bash
git add sd_eloquence/src/module.c
git commit -m "module: re-ungate CJK now that the 2-API switch fixed it

Per docs/cjk-investigation/2026-05-13-phase3-api-divergence.md +
the 2-API switch in earlier commits on this branch, all 14 dialects
now synthesize correctly. The CJK gate from commit 63f08cc is no
longer needed; all g_lang_state[] entries start LANG_AVAILABLE.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 5: Manual Orca gate**

Owner manually:
1. Switches Orca's voice to each CJK dialect
2. Scrolls through Settings dialogs with rapid Tab
3. Scrolls lines in a terminal with text in the target language
4. Confirms: no crashes, no skipped utterances, audio sounds reasonable

If anything regresses in Orca specifically: file separate issue, don't block this branch's release.

---

### Task G3: Re-gate CJK with updated comment (only if G1 failed)

**Files:**
- Modify: `sd_eloquence/src/module.c`

- [ ] **Step 1: Find the language-state initialization**

```bash
grep -n 'is_cjk\|LANG_DISABLED\|LANG_AVAILABLE' sd_eloquence/src/module.c
```

- [ ] **Step 2: Update the gating comment**

Edit `module_init`'s language-state loop. Keep the CJK gating in place, but update the explanatory comment to reflect what we now know:

```c
    /* Latin-script dialects: AVAILABLE. CJK: DISABLED.
     *
     * Status: macho2elf converter is fully audited (every relocation
     * kind in vendor/tvOS-18.2/ is handled correctly; see
     * docs/macho2elf-audit/relocation-catalog.md). Engine wrapper was
     * switched to Apple's modern 2-suffixed ECI API (see
     * docs/superpowers/specs/2026-05-13-eci-2-api-switch-design.md +
     * docs/cjk-investigation/2026-05-13-phase3-api-divergence.md).
     * The 2-API switch did NOT fix the CJK runtime crash.
     *
     * Remaining hypotheses (next investigation phase):
     *   - eciSetIniContent: Apple builds eci.ini content in-memory; we
     *     use disk eci.ini. Possibly the in-memory content has
     *     per-language config we don't replicate.
     *   - Engine-internal state we still haven't initialized via API
     *     surface RE didn't reveal.
     *   - chs/cht/jpn/kor language modules genuinely require something
     *     beyond what's reachable from the API (deeper RE needed).
     *
     * korrom + ko-KR may not have triggered the same crash in isolated
     * tests; we gate it too for parity until validated under Orca's
     * traffic pattern. */
    for (int i = 0; i < N_LANGS; i++) {
        const char *so = g_langs[i].so_name;
        int is_cjk = (strcmp(so, "jpn.so") == 0 || strcmp(so, "kor.so") == 0 ||
                      strcmp(so, "chs.so") == 0 || strcmp(so, "cht.so") == 0);
        g_lang_state[i] = is_cjk ? LANG_DISABLED : LANG_AVAILABLE;
    }
```

- [ ] **Step 3: Build, install, verify Latin still works**

```bash
cmake --build build 2>&1 | tail -3
sudo cmake --install build 2>&1 | grep sd_eloquence
systemctl --user restart speech-dispatcher.service
sleep 1
dist/smoke.sh
cat /tmp/sd_eloquence_smoke.log | tail -10
pgrep -af speech-dispatcher-modules/sd_eloquence
```

- [ ] **Step 4: Commit**

```bash
git add sd_eloquence/src/module.c
git commit -m "module: keep CJK gated; 2-API switch didn't fix the runtime crash

The 2-API rewrite of the engine wrapper was the right thing to do
(matches Apple's framework's usage, robustly initializes engine state
for Latin dialects), but CJK still crashes -- evidence that the bug
is somewhere we haven't touched. Updated the gate comment with the
remaining hypotheses; new investigation needed.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 5: File follow-up brainstorm**

Open a new conversation with the brainstorming skill, anchored on the new findings:
- The macho2elf relocation audit found no converter bugs
- The 2-API switch didn't fix CJK
- Likely next paths: investigate `eciSetIniContent`, or deeper engine-state RE, or accept CJK is unsupportable on this engine build

This step has no commit; it's a workflow gate for the next session.

---

## End of plan

After Phase G:

- **Branch state:** `feat/sd-eloquence-rewrite` is in shippable v1 state. The 2-API switch landed regardless of CJK outcome.
- **CJK ungated** (G2 path) OR **CJK gated with clear explanation** (G3 path).
- **macho2elf relocation audit** project (commits f2639bf → 87527a6 → 63b7153) stands as durable documentation; converter is verified correct for every relocation kind in vendor/.
- **No runtime workarounds in sd_eloquence** for converter bugs — there are none. The `cjk_atexit_override.c` solves an independent exit-time issue and stays.

Final pre-merge checklist (owner):

- [ ] All 5 migration commits build + Latin smoke passes
- [ ] Phase F audit (Task F1) found no straggling legacy api calls
- [ ] 6/6 ctest passes after F4 gate
- [ ] Phase G manual gate completed; either G2 (CJK works → ungate) or G3 (CJK fails → re-gate with clearer comment + follow-up filed)
- [ ] Manual Orca smoke per release checklist (if G2 ran)

---

## Self-review

**1. Spec coverage:**

| Spec section | Plan task(s) |
|---|---|
| §4.1 file-level change map | RE1-RE16 (docs/eci-2-api/), A1-A2 (types), B1 (dlsym), C1-C3 (engine.c open/close), D1-D5 (engine.c switch/dict + worker + module), F2-F3 (cleanup) |
| §4.2 five-commit migration | C2, C3, D1+D2, F2+F3 — actual plan has more commits (split for bite-size) but same logical phases |
| §5 signature acquisition | RE1 (workflow + first function), RE2-RE16 (per-function) |
| §6 new types | A1 (eci.h), A2 (runtime.h) |
| §7 engine.c rewrite | C2 (open), C3 (close), D1 (switch), D2 (dict), D3 (pause/stop), D4 (worker.c), D5 (module.c), E1-E3 (optional voice_activate) |
| §8 testing + validation | Gate 1 (build verify at every commit), Gate 2 (F4 ctest), Gate 3 (Latin smoke at C2/C3/D1/D2/D5/E3/F3), Gate 4 (G1 CJK probe) |
| §9.1 top risks | risk 1 (signature inference wrong) caught by Gate 3 per-commit; risk 2 (ECIsampleFormat layout) caught at C2's smoke; risk 3 (2 API doesn't fix CJK) handled by G3 branch; risk 4 (callback signature) caught at C2's first speak; risk 5 (SetStandardVoice2) gated by E1 read; risk 6 (NewEx2 init ordering) caught at C2 |
| §9.2 rollout sequence | Phase order in this plan |
| §9.3 branch + tag | all on feat/sd-eloquence-rewrite; tag after G2 or G3 + Orca gate |
| §9.4 stays/removed | F2 removes legacy slots from EciApi; F3 removes their LOADs |

**2. Placeholder scan:** no TBD / TODO / FIXME. The RE2-RE14 task table uses parameter substitution (`<funcname>`) which is template-style, not placeholder — each task substitutes one concrete function name. Task E1 has an explicit decision point ("skip Phase E if (b)"), which is a documented branch, not a placeholder.

**3. Type consistency:** `ECIsampleFormat`, `ECIAudioFormatHand`, `EciApi`, `EciEngine`, `ECIDictHand`, `ECIHand` — all referenced consistently across A1, A2, C1, C2, D2. Function-pointer member names match between the struct declaration (A2) and the LOAD macros (B1), since LOAD auto-derives the dlsym name from the member name (the `eci` prefix).

---

## Execution choice

Plan complete and saved to `docs/superpowers/plans/2026-05-13-eci-2-api-switch.md`. Two execution options:

1. **Subagent-Driven (recommended)** — fresh subagent per task, review between tasks, fast iteration.
2. **Inline Execution** — execute tasks in this session using `superpowers:executing-plans`, batch execution with checkpoints.

Which approach?
