# sd_eloquence ECI `2`-API switch — design

**Status:** approved 2026-05-13
**Owner:** Stas Przecinek
**Scope:** Switch `sd_eloquence`'s engine wrapper layer to drive `eci.so` via the modern `2`-suffixed ECI API — the same API Apple's `TextToSpeechKonaSupport.framework` uses — replacing the legacy IBM-compatible API (`eciNewEx`, `eciAddText`, etc.) we currently call.

---

## 1. Motivation

The CJK runtime crashes we've been chasing (`chs.so + 0xfb0` from `reset_sent_vars` in the SynthThread, on first `AddText` after switching to a Chinese dialect) are not caused by macho2elf converter bugs. The Phase A–C audit completed earlier today proved every relocation in `vendor/tvOS-18.2/*.dylib` is correctly translated to ELF: `diff-converter.txt` is empty for all 18 modules.

Investigation of the original `TextToSpeechKonaSupport.framework` (see `docs/cjk-investigation/2026-05-13-phase3-api-divergence.md`) turned up the real cause: Apple's framework drives the engine via the **modern `2`-suffixed ECI API** (`eciNew2`, `eciAddText2`, `eciRegisterSampleBuffer2`, …). The 18 language modules — particularly the CJK ones — were built against that API. The legacy API our `sd_eloquence` uses appears to skip language-module state initialization that the modern API performs, leaving CJK modules in a half-initialized state where `reset_sent_vars` dereferences a stale pointer.

Both APIs are exported from `eci.so` (43 modern functions alongside 56 legacy). They coexist intentionally. Switching `sd_eloquence` to the modern API closes the gap with Apple's own usage and is the correct fix.

## 2. Scope

**In scope:**

- Replace `sd_eloquence/src/eci/runtime.h`'s `EciApi` struct with `2`-suffixed function pointers.
- Replace `sd_eloquence/src/eci/runtime.c`'s dlsym table.
- Rewrite `sd_eloquence/src/eci/engine.c`:
  - `engine_open` uses `eciNewEx2`, `eciSetParam2`, `eciRegisterCallback2`, `eciNewAudioFormat2` + `eciRegisterSampleBuffer2`.
  - `engine_close` uses `eciDelete2`, `eciDeleteAudioFormat2`.
  - `engine_switch_language` uses `Stop2`/`Synchronize2`/`SetParam2`.
  - `engine_load_dictionary` uses `eciNewDict2` / `eciLoadDictVolume2` / `eciDeleteDict2` / `eciActivateDict2`.
- Extend `sd_eloquence/src/eci/eci.h` with new types: `ECIsampleFormat` struct + `ECIAudioFormatHand` opaque pointer.
- Per-function signature documentation under `docs/eci-2-api/`.
- Minor touch-ups to `sd_eloquence/src/synth/worker.c` if the callback signature changed (most likely unchanged).

**Out of scope:**

- `eciSetIniContent` — Apple builds eci.ini content in memory; we keep disk-based eci.ini for now. The function pointer gets loaded but isn't called yet. Future enhancement.
- `eciRegisterKlattHooks2` — sd_eloquence doesn't customize Klatt synthesis.
- Re-converting any `.dylib` from `vendor/tvOS-18.2/` — converter is correct per the audit.
- Any change to `sd_eloquence/src/cjk_atexit_override.c` — solves an exit-time-destructor crash that's independent of which API the wrapper uses.
- Any change to `sd_eloquence/src/filters/`, `synth/`, or `ssml/` — untouched.

## 3. Success criteria

After all five migration commits land:

1. **Build + 6/6 unit tests pass** (`cmake --build build && ctest --test-dir build`).
2. **10 Latin dialects synthesize audibly** via `spd-say -o eloquence -l <lang> "<phrase>"`, all 8 voice variants distinct.
3. **`dist/smoke.sh` runs cleanly** — every utterance plays, no module respawn (same PID throughout).
4. **CJK hypothesis test** — `spd-say -l zh-CN "你好"` (and zh-TW / ja-JP / ko-KR equivalents) either:
   - succeeds → ungate CJK, project fully succeeds; or
   - fails → ship the 2-API switch as-is per the agreed fallback, re-gate CJK with a clearer "2-API switch didn't fix it" comment, file follow-up brainstorm.
5. **Orca manual smoke** — voices behave correctly under rapid Tab in Settings, terminal scrolling, voice switching.

## 4. High-level architecture

The wrapper layer (`eci/runtime.{h,c}` + `eci/engine.{h,c}`) gets rewritten. The public interface to `module.c` and `synth/worker.c` stays the same — those callers see no change beyond an updated callback signature if RE reveals one.

### 4.1 File-level change map

```
sd_eloquence/src/
├── eci/
│   ├── eci.h                  +ECIsampleFormat, +ECIAudioFormatHand
│   ├── runtime.h              EciApi struct rewritten to 2-API
│   ├── runtime.c              dlsym table rewritten
│   ├── engine.h               +sample_fmt, +audio_fmt fields in EciEngine
│   ├── engine.c               full rewrite: open, close, switch_language, dict
│   ├── languages.{c,h}        UNTOUCHED
│   └── voices.{c,h}           UNTOUCHED (voice_activate may gain SetStandardVoice2)
├── synth/worker.c             callback signature may need adjustment
├── module.c                   UNTOUCHED (consumes the engine wrapper)
├── filters/                   UNTOUCHED
├── ssml/                      UNTOUCHED
└── cjk_atexit_override.c      UNTOUCHED (solves independent exit-time bug)

docs/eci-2-api/                NEW: one .md per function we wrap
```

### 4.2 Five-commit migration

Each commit builds + passes Latin smoke independently.

1. **`eci: add 2-API types + signatures to header`** — pure additive. `ECIsampleFormat`, `ECIAudioFormatHand`, new function-pointer declarations in `EciApi`. Old slots stay. Build verifies the new types compile.

2. **`eci/runtime: load the 2-suffixed functions via dlsym`** — populate the new `EciApi` slots in `runtime.c`. Old slots still loaded. Build verifies all dlsyms resolve against the installed eci.so.

3. **`eci/engine: rewrite engine_open / engine_close to use the 2 API`** — `eciNewEx2`, `eciSetParam2`, `eciRegisterCallback2`, `eciNewAudioFormat2` + `eciRegisterSampleBuffer2`, `eciDelete2`, `eciDeleteAudioFormat2`. **Gate 3 (Latin smoke).**

4. **`eci/engine: rewrite engine_switch_language to use the 2 API`** — `eciStop2`, `eciSynchronize2`, `eciSetParam2(eciLanguageDialect)`. Plus `engine_load_dictionary` switches to `eciNewDict2` / `eciLoadDictVolume2` / `eciDeleteDict2` / `eciActivateDict2`. **Gate 3 + rapid-switch smoke.**

5. **`eci/engine: drop the legacy 1 API surface; cleanup`** — remove unused legacy slots from `EciApi`, drop their LOAD macros in `runtime.c`. Cleanup only; no behavior change.

After commit 5: **Gate 4 (CJK hypothesis test)** — manual.

### 4.3 What stays from the legacy API

- The C-level ABI types in `eci.h` (enums `ECIParam`, `ECIVoiceParam`, `ECILanguageDialect`, `ECIMessage`, `ECICallbackReturn`; types `ECIHand`, `ECIInputText`, `ECIMouthData`, `ECIVoiceAttrib`). These are unchanged across the API boundary — only the function names have suffix differences.

## 5. Signature acquisition

For each `2`-suffixed function we wrap, we infer its signature by reading Apple's framework code at the call site. The methodology is mechanical and uniform.

### 5.1 Tools

`llvm-objdump -d --x86-asm-syntax=intel`, `llvm-otool -tv`, `nm`. All already installed.

### 5.2 Per-function workflow

1. **Find every call site** in `TextToSpeechKonaSupport` and `KonaSynthesizer`:

   ```bash
   llvm-objdump -d --x86-asm-syntax=intel \
     /path/to/TextToSpeechKonaSupport \
     | grep -B 8 '<_eciNewEx2>'
   ```

2. **Infer arg list from register loads** per System V AMD64 ABI:
   - Integer/pointer args 1..6 → `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`
   - Float args 1..8 → `xmm0..xmm7`
   - Return value → `rax` (or `rax:rdx` for 16-byte structs; `xmm0` for float)

3. **Cross-reference against legacy version.** `eciNewEx2` should be similar to `eciNewEx` (known signature). The `2` version probably has same prefix args + maybe an extra one.

4. **Validate by reading the function prologue** in `eci.so`:

   ```bash
   llvm-objdump -d --x86-asm-syntax=intel /usr/lib/eloquence/eci.so \
     | awk '/<eciNewEx2>:/,/<eci/' | head -50
   ```

5. **Document inferences** in a per-function file `docs/eci-2-api/<function>.md`. Template (offsets filled in by the implementer at RE time):

   ```markdown
   ## eciNewEx2

   Apple call site (TextToSpeechKonaSupport offset <fill in at RE time>):
       mov   edi, dword [r14 + 0x40]      ; arg 1: dialect (int32)
       call  _eciNewEx2
       mov   r15, rax                      ; return: ECIHand

   Function prologue (eci.so + 0x1cda):
       push rbp; mov rbp, rsp; sub rsp, 0x20
       ...

   Inferred: ECIHand eciNewEx2(enum ECILanguageDialect dialect);
   ```

### 5.3 The critical-path functions

Wrapped in commit 1 (must have signatures before code lands):

`eciNewEx2`, `eciDelete2`, `eciSetParam2`, `eciAddText2`, `eciInsertIndex2`, `eciSynthesize2`, `eciSynchronize2`, `eciRegisterCallback2`, `eciNewAudioFormat2`, `eciDeleteAudioFormat2`, `eciRegisterSampleBuffer2`, `eciStop2`, `eciPause2`.

Wrapped in commit 4:

`eciNewDict2`, `eciLoadDictVolume2`, `eciDeleteDict2`, `eciActivateDict2`.

Optional, low-priority:

`eciSetStandardVoice2`, `eciSetIniContent`, `eciClearErrors2`, `eciGetParam2`, `eciVersion2`.

## 6. New types

### 6.1 `ECIsampleFormat`

Layout inferred from Apple's call to `eciNewAudioFormat2`. Strings in `TextToSpeechKonaSupport` mention `eciSampleFmt` as a `&`-passed (out) parameter and `KONA_AUDIOBUFFER_SIZE` for the buffer size. Conservatively expected:

```c
typedef struct ECIsampleFormat {
    int sample_rate;        /* Hz, e.g. 11025 */
    int bits_per_sample;    /* 16 */
    int channels;           /* 1 = mono */
    int byte_order;         /* 0 = little-endian */
    int format_type;        /* 0 = linear PCM */
    /* + likely a few more reserved/private fields */
} ECIsampleFormat;
```

Exact layout pinned by the §5 RE work before commit 1 ships.

### 6.2 `ECIAudioFormatHand`

Opaque pointer, same shape as `ECIHand` and `ECIDictHand`:

```c
typedef void *ECIAudioFormatHand;
#define NULL_AUDIO_FORMAT_HAND 0
```

### 6.3 New function-pointer declarations

Added to `EciApi`:

```c
/* Audio buffer registration (replaces eciSetOutputBuffer) */
ECIAudioFormatHand (*NewAudioFormat2)(ECIsampleFormat *out);
Boolean            (*DeleteAudioFormat2)(ECIAudioFormatHand h);
Boolean            (*RegisterSampleBuffer2)(ECIHand engine, short *buf,
                                             int n_samples,
                                             ECIAudioFormatHand format);
```

### 6.4 Deferred items

- **`eciSetIniContent`** — Apple's framework pushes eci.ini content as an in-memory string. We keep disk-based eci.ini for now; only load the function pointer (cheap) without calling it.
- **`eciRegisterKlattHooks2`** — Klatt synthesis customization. Not used by sd_eloquence; don't load.

### 6.5 Open RE questions resolved before §7

1. Exact layout of `ECIsampleFormat`.
2. Whether `eciNewAudioFormat2` allocates internally or expects caller storage.
3. Whether `eciRegisterSampleBuffer2`'s `n_samples` is samples or bytes (almost certainly samples).
4. Whether `eciRegisterCallback2`'s callback type matches the legacy `ECICallback`.

§5's per-function docs cover each.

## 7. `engine.c` rewrite

### 7.1 `engine_open`

```c
int engine_open(EciEngine *e, ..., char **errmsg) {
    /* 1. dlopen eci.so + populate api table */
    if (eci_runtime_open(eci_so_path, &e->api, &err) != 0) ...;

    /* 2. Create the engine handle via eciNewEx2 */
    e->h = e->api.NewEx2((enum ECILanguageDialect)initial_dialect);
    if (!e->h) ...;

    /* 3. Sample-rate fallback (Apple rejects rate=2; keep existing logic) */
    if (e->api.SetParam2(e->h, eciSampleRate, sample_rate_param) < 0) {
        sample_rate_param = 1;
        e->api.SetParam2(e->h, eciSampleRate, 1);
    }
    e->sample_rate_param = sample_rate_param;
    e->sample_rate_hz    = sample_rate_param_to_hz(sample_rate_param);

    /* 4. Annotation parsing (same as legacy) */
    e->api.SetParam2(e->h, eciSynthMode, 1);
    e->api.SetParam2(e->h, eciInputType, 1);

    /* 5. Audio format (NEW: explicit struct) */
    memset(&e->sample_fmt, 0, sizeof(e->sample_fmt));
    e->sample_fmt.sample_rate     = e->sample_rate_hz;
    e->sample_fmt.bits_per_sample = 16;
    e->sample_fmt.channels        = 1;
    e->sample_fmt.byte_order      = 0;
    e->sample_fmt.format_type     = 0;
    e->audio_fmt = e->api.NewAudioFormat2(&e->sample_fmt);
    if (!e->audio_fmt) ...;

    /* 6. Register audio callback + buffer */
    if (audio_cb) e->api.RegisterCallback2(e->h, audio_cb, cb_data);
    if (pcm_chunk_samples > 0 && pcm_chunk) {
        e->api.RegisterSampleBuffer2(e->h, pcm_chunk,
                                      pcm_chunk_samples, e->audio_fmt);
    }

    /* 7. Save callback state */
    e->audio_cb = audio_cb; e->cb_data = cb_data;
    e->pcm_chunk = pcm_chunk; e->pcm_chunk_samples = pcm_chunk_samples;
    return 0;
}
```

### 7.2 `engine_close`

```c
void engine_close(EciEngine *e) {
    if (e->h) {
        for (int i = 0; i < N_LANGS; i++)
            if (e->dicts[i]) e->api.DeleteDict2(e->h, e->dicts[i]);
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

### 7.3 `engine_switch_language`

Structure unchanged; `2` substitutions only:

```c
int engine_switch_language(EciEngine *e, int dialect) {
    if (!e->h || dialect == e->current_dialect) return 0;
    e->api.Stop2(e->h);
    e->api.Synchronize2(e->h);
    e->api.SetParam2(e->h, eciLanguageDialect, dialect);
    e->current_dialect = dialect;
    engine_load_dictionary(e, dialect);
    return 0;
}
```

The Delete+NewEx fallback path that we tried during the macho2elf debugging is **not used** here — the `2` API's SetParam may now correctly initialize the CJK module state.

### 7.4 `engine_pause` / `engine_stop`

```c
void engine_pause(EciEngine *e, int on) { if (e->h) e->api.Pause2(e->h, on); }
void engine_stop (EciEngine *e)         { if (e->h) e->api.Stop2(e->h); }
```

### 7.5 `engine_load_dictionary`

```c
int engine_load_dictionary(EciEngine *e, int dialect) {
    if (!e->use_dictionaries) return 0;
    /* ... lookup path same ... */
    ECIDictHand d = e->api.NewDict2(e->h);
    /* ... */
    e->api.LoadDictVolume2(e->h, d, vol, path);
    /* ... */
    e->api.ActivateDict2(e->h, d);
    return 0;
}
```

### 7.6 `EciEngine` struct additions

```c
typedef struct {
    EciApi      api;
    ECIHand     h;
    int         sample_rate_param;
    int         sample_rate_hz;
    int         current_dialect;
    int         current_voice_slot;

    /* NEW for 2-API */
    ECIsampleFormat    sample_fmt;
    ECIAudioFormatHand audio_fmt;

    /* Existing dict + callback state, unchanged */
    int          use_dictionaries;
    char         dict_dir[ELOQ_PATH_MAX];
    ECIDictHand  dicts[N_LANGS];
    ECICallback  audio_cb;
    void        *cb_data;
    short       *pcm_chunk;
    int          pcm_chunk_samples;
} EciEngine;
```

### 7.7 Callback signature

`eciRegisterCallback2`'s callback type is most likely identical to legacy `ECICallback`: the message enums (`eciWaveformBuffer`, `eciIndexReply`, …) are unchanged. RE confirms before commit 2 lands. If a difference surfaces (e.g., an extra cookie arg), `synth/worker.c`'s callback signature gets adjusted in commit 3.

### 7.8 Voice activation

`voice_activate` currently writes 8 `eciSetVoiceParam` calls into slot 0. The `2` API offers `eciSetStandardVoice2` to select among the 8 built-in presets, plus `eciSetVoiceParam2` for per-param overrides. Two plausible adaptations:

1. Replace the per-param loop with one `SetStandardVoice2(slot)`.
2. Keep both calls — `SetStandardVoice2(slot)` for the base preset + `SetVoiceParam2` for SPD-supplied rate/pitch/volume overrides.

RE pins the right answer. Most likely #2.

## 8. Testing + validation

Four stacked gates per commit.

### 8.1 Gate 1 — Build cleanly

```bash
cmake --build build 2>&1 | tail -10
```

No new errors. `nm -D build/sd_eloquence | grep eci` shows the expected `2`-suffixed undefined symbols (clean of legacy stragglers after commit 5).

### 8.2 Gate 2 — Unit tests pass

```bash
ctest --test-dir build
```

6/6 (voices, languages, config, marks, ssml, filters).

### 8.3 Gate 3 — Latin smoke

After every commit that changes runtime behavior (commits 3, 4, 5):

```bash
sudo cmake --install build
systemctl --user restart speech-dispatcher.service
dist/smoke.sh
cat /tmp/sd_eloquence_smoke.log | tail -20
pgrep -af speech-dispatcher-modules/sd_eloquence | wc -l   # exactly 1
```

10 Latin dialects audible, 8 variants distinct, SSML mark utterance plays, cancel-mid-sentence cleanly aborts.

### 8.4 Gate 4 — CJK hypothesis test (after commit 5)

```bash
for d in zh-CN ja-JP ko-KR zh-TW; do
    spd-say -o eloquence -l $d "test"
    sleep 3
done
pgrep -af speech-dispatcher-modules/sd_eloquence | wc -l   # still 1?
```

Branches:

- **CJK works** → ungate, run full Orca manual smoke, project succeeds.
- **CJK still crashes** → ship the 2-API switch anyway (agreed fallback). Re-gate CJK; file follow-up brainstorm.

### 8.5 Reproducer for in-progress debugging

`examples/probe_v2.c` added in commit 3 — replicates `cjk_probe.c` but loads + calls the `2` API directly. Useful as an isolated regression target.

## 9. Risks + rollout

### 9.1 Top risks

1. **Signature inference wrong.** Caught by gate 3. Mitigation: bisect to the failing commit, re-read the function's `docs/eci-2-api/<name>.md`, fix.

2. **`ECIsampleFormat` layout wrong.** Symptom: distorted/silent audio after commit 3. Mitigation: hex-dump format struct after `NewAudioFormat2`, compare against what Apple's framework constructs at the same call site.

3. **2 API doesn't fix CJK.** Owner agreed to ship anyway.

4. **Callback signature differs.** Symptom: segfault in callback during first speak. Mitigation: examine Apple's `globalEciCallback` (offset ~0x33c0 in TextToSpeechKonaSupport) and match exactly.

5. **`eciSetStandardVoice2` doesn't fully replace per-param writes.** Mitigation: fall back to per-param `eciSetVoiceParam2` overrides.

6. **`eciNewEx2` requires different init ordering than `eciNewEx`.** Symptom: engine_open fails. Mitigation: match Apple's call sequence exactly.

### 9.2 Rollout

1. Commits 1-2 (types + dlsym): no behavior change. Safe.
2. Commit 3 (engine_open + audio buffer): first behavior change. Gate 3 must pass.
3. Commit 4 (engine_switch_language + dict): second behavior change. Gate 3 + rapid-switch smoke.
4. Commit 5 (drop legacy slots): cleanup only.
5. Gate 4 (CJK hypothesis test).
6. **CJK works** → re-ungate CJK in module_init (same diff as the macho2elf-audit project's Phase H). Run extended smoke. Manual Orca smoke per release checklist.
7. **CJK still crashes** → re-gate commit with updated comment. Follow-up brainstorm.

### 9.3 Branch + tag

All commits on `feat/sd-eloquence-rewrite`. v1 release tag after manual Orca smoke passes.

### 9.4 What stays / what gets removed

**Stays:**
- `sd_eloquence/src/cjk_atexit_override.c` (independent exit-time fix).
- All filter ports, SSML translator, worker, etc.
- `eci.h`'s ABI type enums.

**Removed:**
- Legacy function-pointer slots from `EciApi` (`NewEx`, `AddText`, `Delete`, etc.).
- `runtime.c`'s LOAD macros for the legacy functions.

## 10. Implementation order (preview)

The writing-plans phase will produce a detailed multi-step plan. High-level ordering:

1. RE each critical-path function from §5.3 list. Commit per-function docs to `docs/eci-2-api/`.
2. Commit 1: new types + struct fields in `eci.h` / `runtime.h`.
3. Commit 2: `runtime.c` dlsym table populates new slots.
4. Commit 3: `engine_open` + `engine_close` rewrite. Latin smoke.
5. Commit 4: `engine_switch_language` + `engine_load_dictionary` rewrite. Rapid-switch smoke.
6. Commit 5: drop legacy slots; cleanup.
7. Manual CJK hypothesis test.
8. If CJK works: ungate, full smoke, release-tag prep.
9. If CJK still broken: ship 2-API switch, re-gate, follow-up brainstorm.
