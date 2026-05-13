# Changelog

All notable changes to apple-eloquence-elf are recorded here.

The format loosely follows [Keep a Changelog](https://keepachangelog.com/),
and the project adheres to [Semantic Versioning](https://semver.org/).

## [1.1.0] — 2026-05-13

A focused config-surface pass driven by a user-facing brainstorm of
what we want exposed in `eloquence.conf` vs. left at sensible
defaults.  Two of these are net-new working features (RateBoost,
PauseMode); the rest are voice-tuning and dictionary controls that
make existing engine knobs reachable from config without recompiling.

### Added

- **Active-voice param overrides.**  Five new keys layer over the
  Apple voice presets so users can tune the active voice's character
  without recompiling.  Each is 0..100; unset keeps the preset's own
  value.
    - `EloquenceHeadSize`
    - `EloquenceRoughness`
    - `EloquenceBreathiness`
    - `EloquencePitchBaseline`
    - `EloquencePitchFluctuation`
- **`EloquenceLoadAbbrDict`** (default 0): opt-in toggle for the
  abbreviation dictionary specifically.  Subordinate to
  `EloquenceUseDictionaries`.  Off by default because Eloquence's
  abbreviation expansion is opinionated and surprising in
  screen-reader contexts.
- **`EloquenceRateBoost`** (default 0): scales the SSML-driven rate
  value by 1.6 before it reaches the engine, extending the perceived
  top speed past the natural `eciSpeed` range.  Mirrors NVDA-IBMTTS-
  Driver's `rateBoost`.  Only fires on the SSML prosody path; the
  per-voice baseline speed and `module_set("rate", …)` are
  unaffected.
- **`EloquencePauseMode`** (default 2): NVDA-IBMTTS-Driver's
  punctuation-pause control.
    - `0` -- engine's natural long pauses everywhere.
    - `1` -- a single short `​`p1` at the very end of each utterance.
    - `2` -- NVDA's regex rewrite injects `​`p1` after every
      punctuation pause site so the engine emits a short one-unit
      pause instead of its longer natural one.
  Implementation lives in `synth/pause_mode.{c,h}` with a focused
  unit test (`tests/test_pause_mode.c`) that verifies NVDA's exact
  regex semantics.

### Removed

- `EloquenceSendParams`: the IBMTTS-specific workaround that prepended
  ``​`vv<vol> `vs<spd>`` to every utterance.  Apple's Eloquence 6.1
  doesn't have the param-reset bug ibmeci 6.7 had, so the prefix was
  pure overhead with no behavioural benefit.  Hard-removed: setting
  the key in `eloquence.conf` now logs an "ignored config" warning
  under `Debug 1` (existing fall-through) and has no effect.

### Changed

- `eloquence.conf` reorganised: the rate-related keys are now grouped
  under a "Audio rate" header with a short signal-flow note
  (engine → optional libsoxr → speech-dispatcher).  The Apple "Higher
  sample rate" comparison sentence is removed -- true but misleading
  given the two stages are independent.
- The dictionary docs in `eloquence.conf` now name the real file
  basenames the engine looks for (`$LANG.main.dic` / `$LANG.root.dic`
  / `$LANG.abbr.dic`) -- the previous four-file list (`.dic/.abr/
  .phr/.rul`) didn't match what `engine_load_dictionary` actually
  reads.

### Install layout

- `/usr/share/apple-eloquence-elf/` → `/usr/share/eloquence/`
- `/usr/share/doc/apple-eloquence-elf/` → `/usr/share/doc/eloquence/`
  Matches the existing `/usr/lib/eloquence/` and
  `/etc/speech-dispatcher/modules/eloquence.conf` naming.  The
  upstream project name itself (`apple-eloquence-elf` — repo, release
  tarball prefix) is unchanged; only on-disk install paths standardize.

### Tooling

- New `sd_eloquence/tools/render_resampler_previews.c`.  The CI release
  workflow runs it once per build to generate sixteen WAV files
  covering per-axis sweeps of the four libsoxr knobs:
    - `rate-{off,16000,22050,32000,44100,48000}.wav`
    - `quality-{quick,low,medium,high,very-high}.wav`
    - `phase-{intermediate,linear,minimum}.wav`
    - `steep-{off,on}.wav`
  All ship in `/usr/share/eloquence/resampler-previews/`; users
  audition each preset with `paplay` / `aplay` / a GUI player before
  setting `EloquenceResampleRate` / `EloquenceResampleQuality` etc in
  `eloquence.conf`.  The conf file's libsoxr section now points at
  this directory.

### Internal

- `voice_activate` gains a trailing `const EloqConfig *cfg` parameter
  (NULL-safe) so it can apply the override layer.  All call sites
  updated.
- `EciEngine` gains a `load_abbr_dict` field; `module.c` propagates
  from `g_cfg`.

## [1.0.3] — 2026-05-13

A "what the config file claims matches what the code actually does"
audit pass.

### Added

- `eloquence.conf` now documents the five user-tunable knobs that were
  wired through but undocumented:
  - `EloquenceUseDictionaries` (default 1) and `EloquenceDictionaryDir`
    for per-language pronunciation dictionaries.
  - `EloquencePhrasePrediction` for `​`pp1`​`-driven prosody (NVDA-style).
  - `EloquenceSendParams` and `EloquenceBackquoteTags` documented under
    an "Advanced / debugging" heading, with their security and
    annotation-parsing caveats spelled out.

### Removed

- Three config keys that were parsed and stored on the config struct
  but never read anywhere in the module:
  - `EloquenceRateBoost` — would have multiplied eciSpeed; never wired.
  - `EloquencePauseMode` — would have controlled punctuation pauses;
    never wired.
  - `EloquenceCjkSegvGuard` — irrelevant in v1 because CJK is gated
    out at the language-select layer anyway.

  Setting any of these in `eloquence.conf` now logs an "ignored
  config" warning at startup (under `Debug 1`) instead of silently
  no-op-ing.  When the matching features get implemented they'll come
  back as real working knobs, not stubs.

## [1.0.2] — 2026-05-13

### Changed

- Release tarballs now ship as `.tar.gz` (gzip) instead of `.tar.zst`
  (zstd) so they extract with stock `tar` on every distro.  The
  compression savings of zstd (~30% on a ~10 MB payload) weren't worth
  asking users on minimal systems to install a prerequisite first.

## [1.0.1] — 2026-05-13

Patch release that resolves installation failures on Arch and Ubuntu
24.04 surfaced by container-based testing of 1.0.0 on Arch, Debian
trixie, Ubuntu 24.04, and Fedora 44.  Debian and Fedora install
cleanly from the 1.0.0 tarball already.

### Fixed

- **Arch:** `install.sh` now installs `libxml2-legacy` instead of
  `libxml2`.  Arch bumped libxml2 to 2.15 in 2025 and the new package
  ships `libxml2.so.16`; the legacy SONAME (`.so.2`) that
  `sd_eloquence` links against is in the `libxml2-legacy` package.
- **Ubuntu noble:** the release tarball now bundles
  `libspeechd_module.so.0` alongside the `sd_eloquence` binary, and
  `sd_eloquence` is linked with `RPATH=$ORIGIN` so it finds the bundled
  copy.  Ubuntu's `libspeechd-dev` package does not ship this helper
  library (Debian does, in `libspeechd-module0`), and the CI build
  links against an upstream speech-dispatcher 0.12.0 we build from
  source.  Bundling avoids the distro-packaging variance entirely.

## [1.0.0] — 2026-05-13

First public release.

### macho2elf converter

- Converts Apple's Mach-O dylibs from `TextToSpeechKonaSupport.framework`
  to Linux ELF `.so` files that load via `dlopen()` and expose the
  standard ECI 6.1 C API.
- Handles every relocation kind present in the tvOS 18.2 dylibs across
  all 18 modules; full per-module audit catalog under
  `docs/macho2elf-audit/`.
- Supports x86_64 Linux (fully tested) and aarch64 Linux (build-verified;
  needs real arm64 hardware for runtime confirmation).
- Built on Python + LIEF.

### sd_eloquence speech-dispatcher module

- Native speech-dispatcher output module, rewritten from scratch against
  the official IBM ECI SDK documentation and the NVDA-IBMTTS-Driver
  reference. Licensed under GPL-2.0-or-later (the converter and the rest
  of the project remain MIT).
- SSML support: speak / mark / prosody / voice / break / say-as via
  libxml2 SAX parsing, translated into ECI control sequences.
- Anti-crash regex filters per language (en/es/fr/de/pt/global), ported
  from NVDA-IBMTTS-Driver's accumulated incident knowledge.
- 8 voice presets (Reed, Shelley, Sandy, Rocko, Flo, Grandma, Grandpa,
  Eddy — "Jacques" replaces Reed for French) transcribed verbatim from
  Apple's `KonaVoicePresets.plist`, so voices sound the same as on iOS /
  tvOS / macOS.
- 10 working languages end-to-end: en-US, en-GB, es-ES, es-MX, fr-FR,
  fr-CA, de-DE, it-IT, pt-BR, fi-FI. Every supported language is
  available on the fly through speech-dispatcher's standard `language=`
  setting — no per-language conf edits.
- Optional libsoxr resampling.
- Single synth thread with cooperative cancellation, mark events, pause
  and resume.

### Release tooling

- GitHub Actions workflow builds per-arch release tarballs (`linux-x86_64`,
  `linux-aarch64`) on each tag.
- `dist/install.sh` resolves runtime deps through the host package
  manager (apt / dnf / pacman / zypper) with confirmation, installs into
  standard FHS paths, and registers the module with speech-dispatcher's
  modulebindir without touching `speechd.conf`.
- `dist/uninstall.sh` mirrors the installer, with `--purge` for the
  config template.

### Known limitations

- **CJK languages (ja-JP, ko-KR, zh-CN, zh-TW) are gated.** The Apple
  dylibs convert and load cleanly, but the romanizer initialization path
  needs the modern 2-suffixed ECI API (`eciNewEx2` / `eciAddText2` /
  `eciRegisterSampleBuffer2`) rather than the legacy IBM-compatible API
  v1 uses. Background in `docs/cjk-investigation/` and the deferred 2-API
  spec in `docs/eci-2-api/`. Re-enabling CJK is v2 work.
- aarch64 Linux is build-verified only; real arm64 runtime testing is
  pending.
