# Changelog

All notable changes to apple-eloquence-elf are recorded here.

The format loosely follows [Keep a Changelog](https://keepachangelog.com/),
and the project adheres to [Semantic Versioning](https://semver.org/).

## [1.1.1] — 2026-05-13

### Fixed

- **SIGPIPE crash on `EloquenceResampleRate` enabled.** The module
  writes PCM to a pipe back to the speech-dispatcher daemon. When
  `EloquenceResampleRate` is set high (e.g. 48000), the data rate
  is 4-5x the pass-through rate; any backend stall closes the pipe,
  the next write hits `SIGPIPE`, the module process dies, and
  speech-dispatcher falls back to its next-preferred output module
  (typically espeak-ng).  Symptom was a brief burst of correctly
  resampled audio followed by an immediate failover. Fix: ignore
  `SIGPIPE` in `module_init` so a pipe stall becomes a benign
  `EPIPE` return on the write rather than a process kill.

## [1.1.0] — 2026-05-13

### Added

Voice-tuning overrides (each 0..100; unset keeps the preset's value):
  - `EloquenceHeadSize`
  - `EloquenceRoughness`
  - `EloquenceBreathiness`
  - `EloquencePitchBaseline`
  - `EloquencePitchFluctuation`

Punctuation, dictionary, and rate controls:
  - `EloquenceLoadAbbrDict` (default 0): opt-in abbreviation expansion.
  - `EloquenceRateBoost` (default 0): 1.6× speed multiplier on the
    SSML-driven rate.
  - `EloquencePauseMode` (default 2): punctuation-pause handling.
    `0` = engine's natural pauses; `1` = a short pause at utterance
    end only; `2` = short pauses at every punctuation site.

Pre-rendered audio previews of every libsoxr resampler preset ship at
`/usr/share/eloquence/resampler-previews/` — sixteen WAVs covering
per-axis sweeps for rate, quality, phase, and steep. `paplay` /
`aplay` one to audition a setting before committing to it in the conf.

### Removed

- `EloquenceSendParams`. Apple's Eloquence doesn't have the voice-
  param-reset bug NVDA's IBMTTS workaround addressed.

### Changed

- Install paths under `/usr/share/` standardize on `eloquence`:
  `/usr/share/eloquence/` and `/usr/share/doc/eloquence/` (was
  `apple-eloquence-elf`). `/usr/lib/eloquence/` and the conf path
  were already on this naming. The repo / release-tarball prefix
  remains `apple-eloquence-elf`.
- `eloquence.conf` rewrite: audio-rate keys grouped under a
  signal-flow header, dictionary docs name the real file basenames
  (`$LANG.{main,root,abbr}.dic`) the engine actually reads.

## [1.0.3] — 2026-05-13

### Added

- `eloquence.conf` documents five previously-undocumented working
  keys: `EloquenceUseDictionaries`, `EloquenceDictionaryDir`,
  `EloquencePhrasePrediction`, `EloquenceSendParams`,
  `EloquenceBackquoteTags`.

### Removed

- Three config keys that were parsed but never consulted:
  `EloquenceRateBoost`, `EloquencePauseMode`, `EloquenceCjkSegvGuard`.
  Setting them in `eloquence.conf` now logs an "ignored config"
  warning under `Debug 1`. `RateBoost` and `PauseMode` return as real
  working knobs in 1.1.0; `CjkSegvGuard` is dropped entirely.

### Changed

- Release tarballs ship as `.tar.gz` (was `.tar.zst`) so they
  extract with stock `tar` on every distro.

## [1.0.1] — 2026-05-13

Container-based testing of 1.0.0 on Arch, Debian trixie, Ubuntu 24.04,
and Fedora 44 surfaced two install failures.

### Fixed

- **Arch:** `install.sh` installs `libxml2-legacy` instead of
  `libxml2`. Arch's 2.15 bump ships `libxml2.so.16`; the `.so.2`
  SONAME `sd_eloquence` links against is in `libxml2-legacy`.
- **Ubuntu noble:** the release tarball bundles
  `libspeechd_module.so.0` alongside `sd_eloquence`, linked with
  `RPATH=$ORIGIN`. Ubuntu doesn't package that helper library as a
  shared object (Debian does, via `libspeechd-module0`); bundling
  sidesteps the distro variance entirely.

## [1.0.0] — 2026-05-13

First public release.

### macho2elf converter

- Converts Apple's Mach-O dylibs from `TextToSpeechKonaSupport.framework`
  to Linux ELF `.so` files that load via `dlopen()` and expose the
  standard ECI 6.1 C API.
- Handles every relocation kind in the tvOS 18.2 dylibs across all 18
  modules; full per-module audit catalog under `docs/macho2elf-audit/`.
- x86_64 Linux fully tested; aarch64 Linux build-verified.
- Python + LIEF.

### sd_eloquence speech-dispatcher module

- Native output module, rewritten from scratch against the IBM ECI SDK
  documentation and the NVDA-IBMTTS-Driver reference. GPL-2.0-or-later
  (the converter and the rest of the project remain MIT).
- SSML: speak / mark / prosody / voice / break / say-as.
- Anti-crash regex filters per language (en / es / fr / de / pt /
  global), ported from NVDA-IBMTTS-Driver.
- 8 voice presets (Reed, Shelley, Sandy, Rocko, Flo, Grandma, Grandpa,
  Eddy; Jacques replaces Reed in French) transcribed from Apple's
  `KonaVoicePresets.plist`.
- 10 working languages: en-US, en-GB, es-ES, es-MX, fr-FR, fr-CA,
  de-DE, it-IT, pt-BR, fi-FI.
- Optional libsoxr resampling; single synth thread with cancellation,
  mark events, pause and resume.

### Release tooling

- GitHub Actions workflow builds per-arch tarballs on each tag.
- `dist/install.sh` resolves runtime deps via the host package manager
  (apt / dnf / pacman / zypper), installs into standard FHS paths, and
  registers the module with speech-dispatcher's `modulebindir`.

### Known limitations

- CJK (ja-JP, ko-KR, zh-CN, zh-TW) is gated. The dylibs convert and
  load, but the romanizer init path needs the modern 2-suffixed ECI
  API rather than the legacy one v1 uses. Re-enabling CJK is v2 work;
  background in `docs/cjk-investigation/` and `docs/eci-2-api/`.
- aarch64 runtime is not yet validated on real hardware.
