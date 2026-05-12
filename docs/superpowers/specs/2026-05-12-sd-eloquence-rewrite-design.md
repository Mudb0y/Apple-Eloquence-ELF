# sd_eloquence rewrite — design

**Status:** approved 2026-05-12
**Owner:** Stas Przecinek
**Scope:** Replace the monolithic `sd_eloquence/src/sd_eloquence.c` (~926 LOC) and supporting files with a properly-engineered speech-dispatcher output module modeled on the IBM ECI 6.x SDK and the NVDA-IBMTTS-Driver reference implementation. v1 release ships full NVDA-class parity (threading, real SSML, anti-crash filters, dictionaries, voice tags) plus CJK working end-to-end.

---

## 1. Motivation

The current module works for basic Orca use on the 10 Latin-script dialects but accreted six concerns (config parsing, language table, voice activation, SSML stripping, resampling, speechd glue) into one translation unit. Recent commits show iterative bug-fixing of the same file: resampler segfault mid-utterance, voice preset cross-contamination, language-switch crashes, sample-rate fallback, CJK gating. Each fix risks regression elsewhere. NVDA's IBMTTS driver has fourteen years of accumulated edge-case handling (per-language regex anti-crash filters, dictionary integration, proper threading, SSML→ECI translation, mark events) that we lack entirely.

Rather than continue patching the monolith, we rewrite from scratch using the IBM SDK documentation (`docs/ibm-sdk/eci.h`, `docs/ibm-sdk/tts.pdf`) and the NVDA-IBMTTS-Driver codebase as references.

## 2. Scope

**v1 release blockers:**
- Threaded synth (single dedicated worker thread + job queue), responsive cancel
- SSML 1.0 translation: `<mark>`, `<break>`, `<prosody rate|pitch|volume>`, `<voice>`, `<lang>`, `<say-as>`, `<sub>`, `<p>`, `<s>`, `<phoneme>` (passthrough), `<emphasis>`
- Index marks reported via `module_report_index_mark` (eciInsertIndex ↔ SSML `<mark>`)
- Per-language anti-crash regex filters ported from NVDA
- Optional per-language user dictionaries loaded from `${EloquenceDataDir}/dicts/`
- Working pause/resume via `eciPause`
- NVDA-style settings: rate boost, pause-mode shortening, phrase prediction, send-params workaround, backquote-tag passthrough (security-default off)
- All 10 Latin-script dialects working (en-US, en-GB, es-ES, es-MX, fr-FR, fr-CA, de-DE, it-IT, pt-BR, fi-FI)
- **CJK working end-to-end** (zh-CN, zh-TW, ja-JP, ko-KR) — see §6 for the staged plan and explicit fallback

**Explicit non-goals for v1:**
- 32/64-bit IPC bridging (NVDA needs it; Linux doesn't)
- Phoneme-level IPA → ECI alphabet translation (passthrough only)
- Word-index callbacks (only mark-index)
- Sound icons, audio mixing (speechd handles upstream)
- A GUI control panel

## 3. License

The macho2elf converter and project root remain MIT. The rewritten `sd_eloquence/` subtree relicenses to **GPL-2.0-or-later** so we can port NVDA's regex anti-crash tables and dictionary-loading patterns verbatim with attribution. Every new file under `sd_eloquence/src/` carries `SPDX-License-Identifier: GPL-2.0-or-later`. A `sd_eloquence/LICENSE.GPL` file holds the full GPLv2 text. README updates to document the per-subtree split.

## 4. High-level architecture

Two threads total:
- **speechd loop thread** (the speechd library's `module_loop` blocks on stdin and dispatches our `module_*` entry points)
- **synth thread** (ours; one per module lifetime)

Communication: a single `synth_job` queue (linked list + mutex + condvar) and one `atomic_int g_stop_requested`. The ECI callback runs synchronously inside `eci.Synchronize`, on the synth thread — same TLS context, no inter-thread audio handoff.

```
   speechd loop (main)                      synth thread (ours)
   ───────────────────                      ──────────────────
   module_speak_sync(text)
      │
      ├─ ssml_parse → job{frames[]}
      ├─ enqueue(job)
      ├─ module_speak_ok()                  wait_for_job() ◀┐
      └─ return                                              │
                                              dequeue(job) ──┘
                                              for each frame:
                                                FRAME_TEXT  → filter → eci.AddText
                                                FRAME_MARK  → eci.InsertIndex
                                                FRAME_BREAK → eci.AddText("`pN")
                                                FRAME_PROSODY_*  / VOICE_* / LANG_*
                                                            → eci.SetVoiceParam / CopyVoice
                                                              / SetParam
                                                FRAME_TEXTMODE  → eci.SetParam(textMode)
                                              eci.InsertIndex(END_STRING_ID)
                                              eci.Synthesize()
                                              eci.Synchronize()
                                                │
                                                └─ECI invokes eci_callback per chunk:
                                                  eciWaveformBuffer → resample
                                                    → module_tts_output_server
                                                  eciIndexReply → module_report_index_mark
                                                                  (or break on END_STRING_ID)
                                              audio_sink_flush()
                                              report_event_end OR _stop
```

### 4.1 File layout

```
sd_eloquence/src/
├── eci/
│   ├── eci.h               ABI types (vendored, GPLv2 header added)
│   ├── runtime.h, runtime.c   dlopen wrapper (existing logic, moved)
│   ├── voices.h, voices.c     Apple preset table + slot-0 activation
│   ├── languages.h, languages.c   language table, IETF↔dialect, availability
│   └── engine.h, engine.c     quirks-aware open/close/switch/pause wrappers
├── filters/
│   ├── filters.h, filters.c   PCRE2-driven anti-crash filter engine
│   ├── lang_global.c, lang_en.c, lang_es.c, lang_fr.c, lang_de.c, lang_pt.c
│   └── README.GPL.md          attribution to NVDA-IBMTTS-Driver
├── ssml/
│   └── ssml.h, ssml.c         libxml2 SAX → synth_job{frames[]} translator
├── audio/
│   ├── resampler.h, resampler.c   libsoxr wrapper
│   └── sink.h, sink.c             buffer accumulator + module_tts_output_server
├── synth/
│   ├── job.h                  synth_job + frame definitions
│   ├── worker.h, worker.c     synth thread + job queue
│   └── marks.h, marks.c       numeric-id ↔ name table for index marks
├── config.h, config.c         eloquence.conf parser
└── module.c                   speechd entry points; ~150 LOC max
```

### 4.2 Dependencies

- pthreads (already linked)
- libsoxr (existing, optional via `-DWITH_SOXR=ON`)
- **libxml2** — new, for SAX-based SSML parsing
- **libpcre2-8** — new, for Perl-compatible anti-crash filters; degrades to no-op if `-DWITH_PCRE2=OFF`
- speech-dispatcher dev headers (existing)
- libiconv (transitively from glibc on Linux; on musl-based distros the cp932 / cp949 / gb18030 / big5 converters may need `libiconv` explicitly — install script detects and adds the dep)

Install script (`dist/install.sh`) extended to resolve libxml2 and libpcre2 via the host package manager.

## 5. Data flow

### 5.1 Synth job model

```c
typedef enum {
    FRAME_TEXT,           // run of plain text to AddText
    FRAME_MARK,           // <mark name="X"/> → eci.InsertIndex(uniq_id)
    FRAME_BREAK,          // <break time="500ms"/> → "`pN " backquote
    FRAME_PROSODY_PUSH,   // <prosody rate=80%> → SetVoiceParam, remember old
    FRAME_PROSODY_POP,
    FRAME_VOICE_PUSH,     // <voice name="Shelley"> → eciCopyVoice(1,0), remember
    FRAME_VOICE_POP,
    FRAME_LANG_PUSH,      // <lang xml:lang="de-DE"> → SetParam(lang), remember
    FRAME_LANG_POP,
    FRAME_TEXTMODE,       // <say-as interpret-as="characters"> → eciTextMode=2
    FRAME_END_MARK,       // sentinel; END_STRING_ID = 0xFFFF for completion
} synth_frame_kind;

typedef struct {
    synth_frame_kind kind;
    union {
        struct { char *text; } text;
        struct { uint32_t mark_id; } mark;
        struct { int millis; } brk;
        struct { int param; int new_value; int saved_value; } prosody;
        struct { int slot; int saved_slot; } voice;
        struct { int dialect; int saved_dialect; } lang;
        struct { int mode; int saved_mode; } textmode;
    } u;
} synth_frame;

typedef struct synth_job {
    struct synth_job *next;
    uint32_t          seq;
    SPDMessageType    msgtype;
    synth_frame      *frames;
    size_t            n_frames;
    void             *arena;       // bump allocator for frame text + mark names
    size_t            arena_used, arena_cap;
} synth_job;
```

### 5.2 Happy path (speak with no concurrent stop)

1. **speechd loop thread**: receives `module_speak_sync(data, len, type)`.
2. `module.c` calls `ssml_parse(data, len, type, &job)`. For `SPD_MSGTYPE_TEXT`, attempts SAX parse via libxml2; on any parse error, falls back to plain-text path with minimal entity decoding. For `SPD_MSGTYPE_CHAR | KEY | SPELL`, bypasses SSML and produces `FRAME_TEXTMODE(2) + FRAME_TEXT + FRAME_TEXTMODE(restore)`.
3. `module.c` assigns `job->seq = atomic_fetch_add(&g_job_seq, 1) + 1` (atomic_uint), calls `worker_submit(job)` (lock + push + cond_signal + unlock).
4. `module.c` calls `module_speak_ok()` and returns. speechd-side emits BEGIN.
5. **Synth thread** wakes, dequeues, walks frames sequentially:
   - `FRAME_TEXT` → `filters_apply(text, dialect)` → `eci.AddText(filtered_utf8 → iconv → target_encoding)`. Check `g_stop_requested` between frames.
   - `FRAME_MARK` → `marks_register(name, job_seq)` returns `(job_seq<<16)|idx`. `eci.InsertIndex(id)`.
   - `FRAME_BREAK` → compute factor from current `eciSpeed` using NVDA's empirical scale (`{10:1, 43:2, 60:3, 75:4, 85:5}` interpolated), `eci.AddText(" \`pN ")`.
   - `FRAME_PROSODY_PUSH` → save `eci.GetVoiceParam(0, param)` into frame, `eci.SetVoiceParam(0, param, new)`. POP restores from saved.
   - `FRAME_VOICE_PUSH` → save current slot, `eci.CopyVoice(slot, 0)`. POP reverses.
   - `FRAME_LANG_PUSH` → save current dialect, switch via engine.c quirks-aware path. POP reverses. Active dialect drives `filters_apply` for subsequent text frames within the lang scope.
   - `FRAME_TEXTMODE` → save/set `eci.SetParam(eciTextMode, mode)`.
6. Worker appends sentinel: `marks_register("__end__", job_seq) = END_STRING_ID`, `eci.InsertIndex(END_STRING_ID)`.
7. Worker calls `eci.Synthesize()` then `eci.Synchronize()`. ECI invokes `eci_callback` on this thread for each PCM chunk and index reply.
8. Callback handler:
   - `eciWaveformBuffer`: copy/resample → `audio_sink_push`; if `g_stop_requested`, return `eciDataAbort`.
   - `eciIndexReply`: if `lParam == END_STRING_ID for current job` → flush sink and signal completion; else if `(lParam>>16) == g_current_job_seq` → `module_report_index_mark(name)`; else stale mark from cancelled job → drop.
9. `Synchronize` returns. `audio_sink_flush()`. Emit `module_report_event_end()`. Free job arena. Loop back.

### 5.3 Stop / cancel

1. speechd loop calls `module_stop()`.
2. `atomic_store(&g_stop_requested, 1)`. Call `eci.Stop(h_engine)`.
3. Synth thread's next callback returns `eciDataAbort`; `Synchronize` returns early.
4. Worker checks flag, emits `module_report_event_stop()`, drops current job.
5. Worker drains queued jobs with `event_stop` for each.
6. `atomic_store(&g_stop_requested, 0)`.

### 5.4 Pause / resume

- `module_pause()`: `eci.Pause(h_engine, ECITrue)`, set `g_paused=1`. PAUSE event reported from worker after the next `eciIndexReply` (speechd protocol expects pause-at-mark).
- Resume: triggered by next `module_speak_sync`. Worker calls `eci.Pause(h, ECIFalse)` before continuing.

## 6. CJK strategy

The crashes are inside Apple's closed-source eci.so. We characterize, theorize, and work around — phases below are cheapest-first.

### Phase 0 — Characterize before fixing
Add `examples/cjk_probe.c` that loads `eci.so`, switches to each CJK dialect via `eciNewEx`, sends a 4-byte test phrase, captures PCM. Run under valgrind, then gdb, then with `eci.dbg` enabled. Build symbol map for `eci.so` (`nm --defined-only` + `addr2line`). Capture readable backtrace for `reset_sent_vars` SIGSEGV. Pin down `jpn.so` "destabilises Orca" to a specific failure mode (crash / silence / stall / corruption).

### Phase 1 — Encoding hypothesis
The current code sends UTF-8 to `eci.AddText`. CJK modules expect gb18030 / cp932 / cp949 / big5. Implementing iconv per-dialect (§5.2 step 5, required for §7 anti-crash filtering anyway) is hypothesized to be a partial or full fix. Re-run the probe.

### Phase 2 — Engine context per language family
Try in order, stop at the first that works:
1. Load `chsrom.so` / `chtrom.so` / `jpnrom.so` / `korrom.so` via `dlopen(RTLD_GLOBAL)` *before* the corresponding CJK eciNewEx. The romanizers are confirmed-loading via eci.ini already, but a pre-load may matter for symbol visibility.
2. `eciReset(h_engine)` before `SetParam(eciLanguageDialect, cjk_dialect)`.
3. Fall back to `eciDelete` + fresh `eciNewEx(cjk_dialect)` for first-time CJK activation. We've previously avoided Delete+NewEx because Apple's build crashes on *second* reload of a language .so; a *first* reload may be fine.

### Phase 3 — Warm-up + retry
If §2 still crashes:
- Send a 1-byte throwaway AddText immediately after CJK init, `Synchronize`, discard PCM. Theory: `reset_sent_vars` is one-shot initialization.
- If even that crashes: install `sigsetjmp`/`siglongjmp` SIGSEGV handler around CJK init, scoped (installed before, removed after). On trip, mark dialect wedged for process lifetime, surface as unavailable in voice list, emit `event_stop`. Gated by `EloquenceCjkSegvGuard 1` opt-in flag (default 0). Signal trampoline is risky enough that users must opt in explicitly.

### Phase 4 — Subprocess sandbox: **DROPPED**
Owner declined to invest in fork+IPC architecture. If phases 0–3 fail, we **re-decompose**: ship v1 Latin-only, file v2 spec for "CJK with deeper engine-internals analysis." Fresh conversation at that point.

### Extra Chinese asset
Owner has noted there is an additional file in the tvOS TextToSpeechKona framework that hasn't been wired up yet. To be investigated during CJK work, not in this design.

### Schedule
Phases 0–2 are bounded and cheap (~few days, parallelizable with other work). Phase 3 is bounded but uglier. The non-CJK deliverables land first; CJK lands when it works.

## 7. SSML translation

`module_speak_sync` may receive SSML 1.0 wrapped text. Detection: if the buffer contains `<`, attempt SAX parse via libxml2; on error, plain-text fallback with entity decoding. `SPD_MSGTYPE_CHAR | KEY | SPELL` bypass SSML and force verbatim text mode.

| SSML construct | ECI translation |
|---|---|
| `<speak>` root | initial xml:lang sets dialect |
| text content | `FRAME_TEXT` (merged across adjacent CDATA) |
| `<mark name="X"/>` | `FRAME_MARK(register(X))` — name >30 chars or NUL dropped with debug log |
| `<break time="500ms"/>` | `FRAME_BREAK(500)` |
| `<break strength="medium"/>` | strength × 200ms (NVDA scale) |
| `<break/>` | `FRAME_BREAK(200)` |
| `<prosody rate="N%">` | `FRAME_PROSODY_PUSH(eciSpeed, percent→[40..156])` |
| `<prosody rate="x-slow\|slow\|medium\|fast\|x-fast">` | maps to 20/40/60/80/100 % |
| `<prosody pitch="N%">` | `FRAME_PROSODY_PUSH(eciPitchBaseline, 0..100)` |
| `<prosody pitch="+Nst">` | semitones × 6% heuristic |
| `<prosody volume="…">` | `FRAME_PROSODY_PUSH(eciVolume, 0..100)` (named values: silent/soft/medium/loud/x-loud → 0/25/50/75/100) |
| `<voice name="Reed">` | `FRAME_VOICE_PUSH(slot_for_name)` (variant swap, not engine swap) |
| `<voice gender="male\|female">` | slot 0 (Reed) male, slot 1 (Shelley) female |
| `<voice xml:lang="de-DE">` or `<lang xml:lang="de-DE">` | `FRAME_LANG_PUSH(de-DE)`; filter set updates for scope |
| `<say-as interpret-as="characters\|spell">` | `FRAME_TEXTMODE(2)` + restore |
| `<say-as interpret-as="digits\|cardinal\|ordinal">` | passthrough (engine handles natively) |
| `<sub alias="X">orig</sub>` | `FRAME_TEXT(X)` |
| `<p>`, `<s>` | inserts `FRAME_BREAK(400)` after close |
| `<phoneme>word</phoneme>` | `FRAME_TEXT(word)` (ph attribute dropped — ECI phoneme alphabet is non-standard) |
| `<emphasis level="…">` | wraps in `\`emph<N>` when backquote tags enabled; passthrough otherwise |
| Unknown elements | children processed, element ignored (per SSML spec) |

### 7.1 Mark name → id encoding

```c
// 16 high bits = job_seq, 16 low bits = per-job mark index (0..0xFFFE)
// 0xFFFF reserved for END_STRING_ID
typedef struct {
    uint32_t  id;
    char     *name;        // in job arena
    uint32_t  job_seq;
    bool      consumed;
} mark_entry;

#define END_STRING_ID 0xFFFF
```

Marks table is a flat array of fixed cap (256 in-flight). When a job is freed, entries are recycled. If a job requests more than 256 marks (extremely rare in practice — Orca typically sends 1–10 per utterance), additional marks are dropped with a debug log and the underlying text still synthesizes normally; only mark-event reporting is lost for the dropped ones.

### 7.2 Filter ordering per FRAME_TEXT

1. `lang_global` rules (NVDA `ibm_global_fixes`) — always applied
2. Per-current-dialect rules — `lang_en`, `lang_es`, `lang_fr`, `lang_de`, `lang_pt`
3. Pause-mode rewrite if `EloquencePauseMode != 0` (NVDA `ibm_pause_re`)
4. Backquote sanitization: replace `` ` `` with space unless `EloquenceBackquoteTags=1`
5. iconv to target encoding (`encoding_for(dialect)`)

### 7.3 Encoding table

```c
static const char *encoding_for(int dialect) {
    switch (dialect) {
        case eciMandarinChinese:   return "gb18030";
        case eciStandardJapanese:  return "cp932";
        case eciStandardKorean:    return "cp949";
        case eciHongKongCantonese: return "big5";
        default:                   return "cp1252";  // all Latin-script
    }
}
```

Unmappable bytes → `?` (NVDA behavior).

## 8. Porting NVDA assets

### 8.1 Anti-crash filters

NVDA's tables in `addon/synthDrivers/ibmeci.py`:

- `ibm_global_fixes` (~6 patterns) — `lang_global.c`
- `english_ibm_fixes` + `english_fixes` union — `lang_en.c`
- `spanish_ibm_fixes` + `spanish_ibm_anticrash` + `spanish_fixes` — `lang_es.c`
- `french_ibm_fixes` + `french_fixes` — `lang_fr.c`
- `german_ibm_fixes` + `german_fixes` — `lang_de.c`
- `portuguese_ibm_fixes` — `lang_pt.c`

Apple's eci.so descends from the ETI/SpeechWorks tree but shares the IBM-era idiosyncrasies (e.g. `Mc`-prefix crashes, weird time parsing). We ship the union of `_ibm_fixes` and non-`_ibm_fixes` per language, sequenced as IBM-rules first. Selectively trim later if regressions appear.

Each `lang_XX.c` follows:

```c
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from NVDA-IBMTTS-Driver/addon/synthDrivers/ibmeci.py
// Original: Copyright (C) 2009-2026 David CM and contributors, GPL-2.0
#include "filters.h"

const filter_rule lang_en_rules[] = {
    { "\\b(Mc)\\s+([A-Z][a-z]|[A-Z][A-Z]+)", "\\1\\2", 0 },
    { "c(ae|\xe6)sur(e)?",                   "seizur", PCRE2_CASELESS },
    /* ... */
    { NULL, NULL, 0 }
};
```

The compiled regex is lazy-cached on first use for process lifetime. If `-DWITH_PCRE2=OFF`, filters become no-ops and we log a one-time warning at init.

### 8.2 User dictionaries

NVDA loads `<langid>main.dic`, `<langid>root.dic`, `<langid>abbr.dic` via `eciNewDict` + `eciLoadDict` + `eciSetDict`. We mirror:

- After `eciNewEx` or language switch, scan `${EloquenceDictionaryDir}` (default `${EloquenceDataDir}/dicts/`) for files matching `<langid>main.dic` / `root.dic` / `abbr.dic` (langid is the 3-letter code from the existing `LangEntry` table: `enu`, `eng`, `esp`, `esm`, `fra`, `frc`, `deu`, `ita`, `chs`, `cht`, `ptb`, `jpn`, `fin`, `kor`).
- Allocate one `ECIDictHand` per language, load each available volume, `eciSetDict(h, dict)`. Cache in `g_lang_dicts[N_LANGS]` so subsequent switches reuse.
- Extend `EciApi` with `eciNewDict`, `eciSetDict`, `eciLoadDict`, `eciDeleteDict` (absent from current `eci_runtime.h`).
- Config keys: `EloquenceDictionaryDir`, `EloquenceUseDictionaries` (default `1`).
- Per-utterance toggle via `module_set("dictionary", "on|off")` mapping to `eciSetParam(eciDictionary, 0|1)`.

Dictionary files are *not* shipped in the tree. Users obtain them from upstream collections (`eigencrow/IBMTTSDictionaries` or `mohamed00/AltIBMTTSDictionaries`) and drop into `/usr/lib/eloquence/dicts/`. Install script README mentions this.

### 8.3 Other NVDA features (mapping)

| NVDA feature | Implementation | Config key |
|---|---|---|
| `rateBoost` | multiplier on `eciSpeed` (1.6× when on) | `EloquenceRateBoost 0\|1` |
| `pauseMode` | `ibm_pause_re` regex rewrite of punctuation runs | `EloquencePauseMode 0\|1\|2` |
| `phrasePrediction` | inject `` `pp1 `` prefix per speak | `EloquencePhrasePrediction 0\|1` |
| `sendParams` | inject `` `vv<vol> `vs<speed> `` per speak | `EloquenceSendParams 0\|1` (default `1`) |
| `backquoteVoiceTags` | passthrough when on; space-replace when off | `EloquenceBackquoteTags 0\|1` (default `0` — security) |

### 8.4 Not ported in v1
- 32/64-bit IPC bridge (Linux N/A)
- Windows message-pump thread model
- NVDA's settings UI (NVDA-side, not driver-side)

## 9. Error handling

`module_init` is strict — any failure returns `-1` with the full reason in `*msg`. Post-init, the module degrades gracefully on any external bad input.

| Failure | Behavior |
|---|---|
| `eci.so` dlopen/dlsym fails | init `-1` with full error |
| `eci.ini` missing | init `-1` |
| Default language unavailable | warn, fall back to first available |
| All languages unavailable | init `-1` |
| libpcre2 not built in | filters no-op, one-time warning at init |
| iconv transcode error | replace bad bytes with `?`, log once per dialect |
| SSML parse error | plain-text fallback with entity decoding |
| `eci.AddText` returns `ECIFalse` | log `eci.ProgStatus()`, drop text frame, continue |
| CJK NewEx returns NULL mid-process | mark dialect wedged for process, surface unavailable, event_stop |
| CJK first-AddText SIGSEGV | only when `EloquenceCjkSegvGuard=1`; sigsetjmp out, mark wedged, event_stop |
| Allocation failure | log, drop frame/job, no crash |
| Audio sink push fails (speechd socket closed) | log, drop remaining PCM, event_stop |
| Stop with no active job | no-op |

## 10. Configuration

```ini
# Diagnostics
Debug 0

# Engine data
EloquenceDataDir         /usr/lib/eloquence
EloquenceDictionaryDir   /usr/lib/eloquence/dicts   # if unset, derives from EloquenceDataDir/dicts

# Defaults
EloquenceDefaultLanguage en-US
EloquenceDefaultVoice    Reed
EloquenceSampleRate      1            # 0=8k 1=11025 2=22050

# Audio postprocessing
EloquenceResampleRate    0
EloquenceResampleQuality very-high
EloquenceResamplePhase   intermediate
EloquenceResampleSteep   0

# NVDA-style toggles
EloquenceUseDictionaries 1
EloquenceRateBoost       0
EloquencePauseMode       2
EloquencePhrasePrediction 0
EloquenceSendParams      1
EloquenceBackquoteTags   0

# CJK
EloquenceCjkSegvGuard    0
```

Every key has a hardcoded default in `config.c`. Unknown keys log debug warning, don't fail init.

**Speechd `module_set` keys:** `rate`, `pitch`, `volume`, `synthesis_voice`, `language`, `voice_type`, `punctuation_mode`, `cap_let_recogn`.

## 11. Testing

### 11.1 Unit tests (CMake `add_test()`, run in CI)
- `test_filters` — fixture-per-language with input → expected output, validates regex porting
- `test_ssml` — XML inputs → golden frame-sequence serializations
- `test_marks` — round-trip name → id → resolve, including stale-job handling
- `test_config` — `eloquence.conf` snippets → parsed values

Engine not required; cheap and fast.

### 11.2 Integration binaries (manual, post-build)
- `examples/speak` — existing, verifies PCM out for a known phrase
- `examples/cjk_probe` — new; eciNewEx per CJK dialect + 1-byte AddText + Synchronize → captures PCM or crash
- `examples/mark_probe` — new; sentence with three `<mark>`s → expects three callback fires with correct names

### 11.3 Manual smoke (`dist/smoke.sh`, pre-release)
~10 `spd-say` invocations: per supported language, per variant, with SSML marks, cancel-mid-sentence, pause/resume. Output to a small report with PCM lengths; diffable across releases.

## 12. Migration plan

The owner restored uncommitted changes to HEAD before the brainstorm, so we start clean.

1. Branch off `main` → `feat/sd-eloquence-rewrite`.
2. Land the new tree alongside the old one initially: rename current `sd_eloquence/src/*.c` to `sd_eloquence/src/old/`, update CMake to build it as `sd_eloquence_old` for fallback comparison.
3. Write the new module under `sd_eloquence/src/` per §4.1, one commit per subsystem in this order: `eci/`, `config.c`, `audio/`, `ssml/`, `filters/` (one commit per language), `synth/`, `module.c`.
4. Once the new module passes smoke on x86_64 with all 10 Latin languages (and CI confirms aarch64 builds), flip CMake's default to `sd_eloquence`. Runtime aarch64 smoke happens before tagging release.
5. CJK phases 0–3 as separate commits on the same branch.
6. **Decision point:** if CJK works, PR-merge to main, cut v1. If CJK phases 0–3 don't pan out (phase 4 dropped), open fresh discussion to either strip CJK and ship Latin-only v1, or hold the rewrite indefinitely.
7. Delete `sd_eloquence/src/old/` in same commit that flips CMake.

## 13. Open risks

- **CJK phases 0–3 may not yield a stable result.** Owner-decided fallback: re-decompose to v1 Latin-only + v2 CJK with separate spec.
- **libxml2 SSML SAX parsing may be heavier than expected on per-utterance overhead.** Mitigation: fast-path bypass if buffer contains no `<`.
- **Apple eci.dylib quirks we haven't catalogued yet.** Current code documents: 22kHz rejected, `Synchronize` non-blocking, can't `dlclose`, second reload of language .so crashes, voice slot 0 only. Likely more lurk.
- **GPLv2 license change of sd_eloquence subtree may surprise downstream packagers.** Mitigation: prominent README + dist/install.sh note + a clear LICENSE.GPL alongside the existing root LICENSE.

## 14. Implementation order (preview)

The writing-plans phase will produce a detailed multi-step plan. High-level ordering:

1. License + scaffolding (LICENSE.GPL, README split, CMake skeleton)
2. eci/ subsystem extraction from current code
3. config.c + audio/ extraction
4. ssml/ parser + unit tests
5. filters/ scaffolding + per-language ports + unit tests
6. synth/ worker + job model
7. module.c speechd entry points
8. Smoke + integration binaries
9. CJK Phase 0 (characterize)
10. CJK Phase 1 (encoding)
11. CJK Phase 2 (engine context)
12. CJK Phase 3 (warm-up + sigsetjmp opt-in)
13. CMake flip + old/ deletion
14. PR review + merge + v1 tag
