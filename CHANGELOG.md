# Changelog

All notable changes to apple-eloquence-elf are recorded here.

The format loosely follows [Keep a Changelog](https://keepachangelog.com/),
and the project adheres to [Semantic Versioning](https://semver.org/).

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
