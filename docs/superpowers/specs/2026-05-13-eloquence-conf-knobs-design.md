# eloquence.conf knob audit + NVDA-parity additions — design

**Date:** 2026-05-13
**Status:** Draft for review.

## Goal

Reconcile `sd_eloquence`'s configuration surface with what's actually
useful for v1 users, taking the NVDA-IBMTTS-Driver feature set as the
reference point. Drop one knob that's no longer relevant, add NVDA-
style voice and dictionary controls, and implement the two NVDA
features (`RateBoost`, `PauseMode`) that were dropped as stubs in 1.0.3.

This work is a single follow-on patch release (1.0.4 or 1.1.0 depending
on whether semver treats the new working features as additions —
default to **1.1.0** since `RateBoost` and `PauseMode` are net-new
working features).

## Decisions (from the brainstorm interview)

| Topic | Decision |
|---|---|
| Engine pass-through params (`RealWorldUnits` / `NumberMode` / `TextMode`) | Skip. Current defaults are fine. |
| Voice param overrides | Active-voice global overrides. Five keys: head size, roughness, breathiness, pitch baseline, pitch fluctuation. |
| Dictionary granularity | Master `EloquenceUseDictionaries` + secondary `EloquenceLoadAbbrDict` (NVDA parity). |
| `RateBoost` / `PauseMode` | Implement both as real working features. |
| Sample-rate doc | Add a short signal-flow paragraph. Drop the "Apple's 'Higher sample rate' toggle in VoiceOver" reference. |
| `EloquenceSendParams` cleanup | Hard-remove. No transitional warning. |

## What changes

### Removed

`EloquenceSendParams` and everything that consults it.

- `EloqConfig::send_params` field (config.h)
- `c->send_params = 0` default (config.c)
- `EloquenceSendParams` parse branch (config.c)
- The `if (w->cfg->send_params)` `​`vv<vol> `vs<speed>` prefix logic in
  `synth/worker.c::exec_job`
- The comment about it in `eci/engine.c::engine_open`

Rationale: `sendParams` was an IBMTTS-specific workaround for an
ibmeci 6.7 bug where the engine would reset voice params silently
between utterances. Apple's bundled Eloquence 6.1 doesn't have that
bug. The workaround introduces real cost (extra annotation parsing on
every utterance) for no benefit on Apple Eloquence.

### Added — voice param overrides

Five new keys, applied in `voices.c::voice_activate` after the preset
values are written:

| Key | ECI param | Range |
|---|---|---|
| `EloquenceHeadSize` | `eciHeadSize` | 0–100 |
| `EloquenceRoughness` | `eciRoughness` | 0–100 |
| `EloquenceBreathiness` | `eciBreathiness` | 0–100 |
| `EloquencePitchBaseline` | `eciPitchBaseline` | 0–100 |
| `EloquencePitchFluctuation` | `eciPitchFluctuation` | 0–100 |

Storage: integers on `EloqConfig`, each with a sentinel value
`ELOQ_VOICE_PARAM_UNSET = -1` meaning "don't override; use the
preset's value." Default is `UNSET` for all five.

Wiring: `voice_activate` takes an additional `const EloqConfig *cfg`
argument. After writing the preset value for each voice param, if the
matching override field is not `UNSET`, it issues a second
`SetVoiceParam` with the override. The preset still drives the
baseline (so switching presets still feels different), but the
override applies on top.

Speed and volume are deliberately not in this set: they flow through
the SSML prosody path per-utterance, which is the right place for
them to live.

### Added — dictionary granularity

`EloquenceLoadAbbrDict` (boolean, **default 0** — opt-in) gates
loading of the `.abr` abbreviation-expansion dictionary specifically.
It's subordinate to `EloquenceUseDictionaries`: if the master switch
is off, the abbreviation toggle has no effect; if the master switch
is on, this controls whether the `.abr` file is loaded alongside the
main / phrase / rule dictionaries.

Rationale for off-by-default: Eloquence's built-in abbreviation
expansion is opinionated and surprising for screen-reader users — it
will silently rewrite tokens that look like abbreviations even when
the user wants the literal text. Per-user opt-in lets people who
want the behaviour turn it on, without forcing it on everyone.

Implementation: `engine_load_dictionaries` in `eci/engine.c` already
loops over four dictionary file types via a small table. Add a flag
on the `EciEngine` struct (`load_abbr_dict`) and skip the `.abr`
iteration when it's 0. `module.c` propagates the config value into
the engine struct alongside the existing `use_dictionaries` and
`dict_dir`.

### Implemented — `RateBoost`

NVDA-IBMTTS model, exactly:

- `EloquenceRateBoost` (boolean, default 0)
- Scalar multiplier `RATE_BOOST_MULTIPLIER = 1.6`
- Only applies on the **SSML prosody rate** path
  (`ssml.c::prosody_rate_value` → `FRAME_PROSODY_PUSH` →
  `worker.c::exec_prosody_push`). When boost is on, the worker
  multiplies the new value by 1.6 before `SetVoiceParam(eciSpeed,
  ...)`. Clamped to the engine max (200).
- Does **not** apply on the `voice_activate` path. The preset's
  natural speed (90–95) is a baseline; rate-boost is a user-facing
  amplifier of the SSML-driven dynamic rate. This matches NVDA's
  semantic: the boost only changes how the rate-slider values map to
  eciSpeed, not how a voice "sounds at rest."
- The inverse (reading eciSpeed back and reporting to a UI) is not
  needed for sd_eloquence because we don't expose a GET path back to
  speech-dispatcher beyond what eciGetVoiceParam already returns.

Effect for users: with the default `MAX_RATE = 100` SSML mapping,
boost off → eciSpeed=100 at max; boost on → eciSpeed=160 at max,
which is noticeably faster.

### Implemented — `PauseMode`

NVDA-IBMTTS model, exactly:

- `EloquencePauseMode` (integer, default 2, valid 0/1/2)
- Mode 0: pass-through. No text rewriting. Engine's natural pauses
  apply.
- Mode 1: append `​`p1 ` once at the **end** of each utterance.
  Shortens the trailing inter-utterance pause that the engine emits
  when it sees a final period.
- Mode 2: regex-rewrite all punctuation pauses to `​`p1`. The pattern,
  copied from NVDA-IBMTTS's `pause_re`:

      ([a-zA-Z0-9]|\s)([-,.:;)(?!])(\2*?)(\s|[\\/]|$)

  …rewritten to insert `​`p1` between the leading word/space and the
  punctuation. Done with libpcre2 (already a dep) in a new transform
  step in `filters_apply` or as a small sibling helper in
  `synth/worker.c::exec_text_frame`.

Wired in `worker.c::exec_text_frame`, after `filters_apply` and
before backquote sanitization. The pause rewrite injects `​`p1`
sequences, which look like backquote tags — these MUST survive the
backquote-sanitization step. Practical fix: do the pause rewrite
**after** the user-text backquote sanitization (i.e., after `*p = ' '`
on user-supplied backticks), so the injected `​`p1` tags are the only
backquotes left in the string when ECI parses them.

Order in `exec_text_frame` becomes:

```
1. filters_apply(text, dialect)        # language anti-crash filters
2. for each '`' in user text: ' '       # sanitize unless backquote_tags=1
3. if pause_mode == 2: regex-rewrite    # inject `p1 at punctuation
4. transcode(utf-8 → engine encoding)
5. AddText
6. if pause_mode == 1: AddText("`p1 ")  # one-shot trailing pause
```

Edge cases:
- Mode 1 + AbbrDict-driven sentence-splitting: the `​`p1` lands at the
  end of the assembled job, which is what we want — a single
  utterance-final shortened pause regardless of how many text frames
  the job contains.
- Mode 2 + `EloquenceBackquoteTags=1`: power user wanted backquote
  passthrough. They still get punctuation rewriting; their hand-
  authored backquote tags are unaffected by the punctuation regex
  (which only fires on `[-,.:;)(?!]`).

### Docs

- Add a 2-sentence signal-flow paragraph above the SampleRate /
  ResampleRate section in `eloquence.conf`:

  > Audio flows engine → optional libsoxr resampler → speech-dispatcher.
  > `EloquenceSampleRate` sets the engine's native PCM rate;
  > `EloquenceResampleRate` (optional) upscales after.

- Remove the "Apple's 'Higher sample rate' toggle in VoiceOver is the
  same thing" sentence (true, but misleading).

- All new keys documented in `eloquence.conf` with realistic value
  ranges, default values, and what they do. Voice param overrides
  collected under a single `## Voice tuning` section. PauseMode +
  RateBoost go next to PhrasePrediction in the synthesis tweaks
  block. Dictionary toggles together.

## Files to change

- `sd_eloquence/src/config.h` — drop `send_params`; add 5 voice param
  override fields, `load_abbr_dict`, `rate_boost`, `pause_mode`.
- `sd_eloquence/src/config.c` — drop `EloquenceSendParams` parse
  branch + default. Add new keys + defaults. `ELOQ_VOICE_PARAM_UNSET`
  default for the five overrides.
- `sd_eloquence/src/eci/voices.c` and `voices.h` — `voice_activate`
  takes `const EloqConfig *cfg`; layer overrides over preset values
  for the five params.
- `sd_eloquence/src/eci/engine.h` — add `load_abbr_dict` field.
- `sd_eloquence/src/eci/engine.c` — gate the `.abr` iteration on
  `load_abbr_dict`.
- `sd_eloquence/src/module.c` — pass `&g_cfg` to `voice_activate`;
  propagate `load_abbr_dict` to engine struct.
- `sd_eloquence/src/synth/worker.c` — drop `send_params` prefix code;
  add `RateBoost` scalar in the prosody-rate-push path; add
  `PauseMode` rewrite in `exec_text_frame`.
- `sd_eloquence/eloquence.conf` — remove SendParams; document the
  new keys; sample-rate doc tweak.
- `sd_eloquence/tests/test_config.c` — drop the SendParams-implicit
  assertions; add coverage for the new keys (defaults + override
  parsing + invalid values).
- `CHANGELOG.md` — entry for 1.1.0.
- `CMakeLists.txt` — bump `VERSION` to 1.1.0.

No changes to `eci/eci.h`, the SSML parser, or the converted ELFs.

## Test plan

Unit tests in `sd_eloquence/tests/test_config.c`:
- Each new key parses with valid values; out-of-range integers
  rejected.
- `ELOQ_VOICE_PARAM_UNSET` default for the five overrides.
- `EloquenceLoadAbbrDict` parses; default = 1.

A new `test_pause_rewrite.c` (or extending an existing test file)
exercises the PauseMode regex against a handful of fixed inputs:
- Mode 0: identity.
- Mode 2 + `"Hello, world."` → `"Hello `p1, world`p1."` (or similar
  pattern — match NVDA's exact regex semantics).
- Mode 2 + `"a,b"` (no spaces) → no rewrite (regex requires the
  `(\s|[\\/]|$)` trailing context).

Smoke test in container (extending `test_in_container_gz.sh`):
- Verify the new keys roundtrip through install.sh.
- Run an `INIT`-only round-trip and check that setting
  `EloquenceRateBoost 1` in the conf doesn't crash the module.

Full container re-test on all 4 distros after the implementation
lands.

## Open items

- Decide which version to ship as: 1.0.4 (patch) or 1.1.0 (minor).
  Recommendation: **1.1.0** since RateBoost + PauseMode are new
  user-visible features, not just config plumbing.
- Whether to deprecate `EloquenceUseDictionaries=0` as a route to
  disabling abbreviations (since `EloquenceLoadAbbrDict=0` would now
  be the precise knob). Recommendation: keep both supported.

## Defaults summary (post-implementation)

These are the defaults a fresh install will see; users opt-in to the
features that are off-by-default.

| Key | Default | Notes |
|---|---|---|
| `EloquenceUseDictionaries` | 1 | Master switch; loads main / phrase / rule dictionaries when present. |
| `EloquenceLoadAbbrDict` | **0** | Opt-in; `.abr` expansion is surprising / unwanted for many screen-reader users. |
| `EloquencePhrasePrediction` | 0 | Opt-in; `​`pp1` engine-side phrase prediction. Already shipped at this default in 1.0.3; this work doesn't change it. |
| `EloquenceRateBoost` | 0 | Opt-in. |
| `EloquencePauseMode` | 2 | NVDA's default (shorten all pauses). |
| Voice param overrides | UNSET | Preset values win unless explicitly overridden. |
