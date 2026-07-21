# Changelog

All notable changes to apple-eloquence-elf are recorded here.

The format loosely follows [Keep a Changelog](https://keepachangelog.com/),
and the project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- **Experimental `android-x86_64` converter target** (`--os android` on an
  x86_64 slice). Simpler than arm64 — x86_64 Android uses the SysV ABI, so no
  variadic/`__chkstk` shims; only the Bionic bits apply (libc++ `__ndk1`
  rewrite, `bzero`/`ftime` stubs, `__errno`, `libunwind`). All 19 modules build
  and are **symbol-complete**. Off by default in the APK (`abiFilters` stays
  arm64-v8a) — see Known issue.

### Verified

- **On-device/emulator testing of the Android build.** Brought up a
  KVM-accelerated **x86_64 Android 14 emulator**; the **APK installs and
  registers as a selectable system TTS engine**. Separately, ran the converted
  **arm64** engine under a real Bionic `linker64` (qemu-user): it loads and
  synthesizes audio byte-near-identical to Linux. (This is what surfaced the
  1.2.3 phdr fix.)

### Known issue

- **`android-x86_64`: `eciNew` crashes at runtime** (NULL function-pointer call
  — an engine object is reached uninitialised, an init step skipped). Unique to
  the x86_64+Bionic combination; arm64+Bionic and x86_64+glibc both synthesize
  correctly. Not a symbol/relocation problem (runtime resolution is clean). Root
  cause not yet pinned (needs single-stepping the stripped engine). arm64
  remains the validated, shipped target.

### Fixed

- **Long utterances were truncated once a job exceeded 256 frames ("text past a
  certain amount is eaten on one huge chunk").** A `synth_job` held a fixed
  `FRAMES_CAP = 256` frames and `synth_job_push_frame` returned NULL past that,
  silently dropping every later frame — text included. speech-dispatcher emits
  word-level `<mark>`s and each marked word costs two frames (mark + text), so a
  long read hit the cap at ~128 words and the rest went unspoken. The frame
  array now grows on demand (`sd_eloquence/src/synth/job.c`); the initial cap is
  just a hint. Verified against the engine: a 400-marked-word utterance that
  previously produced 128 marks and 208 s of audio now speaks all 400 words
  (818 s, matching the mark-free baseline). Plain text without marks was never
  affected (one frame regardless of length).
- **Index marks past 256 were dropped for the same reason** — a hard
  `MARKS_MAX = 256` global table (`sd_eloquence/src/synth/marks.c`). The table
  now grows on demand; since every access is under its lock and mark names are
  separately heap-allocated, reallocating the entry array never invalidates a
  name already returned by `marks_resolve`. The 400-mark test now reports all
  400 marks. This restores caret/word tracking on long reads.

## [1.2.3] — 2026-06-09

### Fixed

- **Converted `.so` files now load under Android's Bionic linker** (and are more
  standard ELF generally). The ELF header + program-header table used to sit
  *before* the first `PT_LOAD` segment; glibc's loader tolerates that, but
  Bionic refuses with *"can't find loaded phdr"* and won't `dlopen` the library.
  The linker script now puts `FILEHDR PHDRS` in the first `PT_LOAD` (+ a
  `PT_PHDR`), placing the headers inside a loaded segment — there's always room
  in the gap Mach-O leaves below `__text`. Verified by running the converted
  engine under a real Bionic `linker64` (via qemu-user): it loads and
  synthesizes audio **byte-near-identical to the Linux build** (same sample
  count; ≤3/19000 deviation from FP rounding). Linux x86_64/arm64 unaffected.

### Added

- **`android/` — experimental Android TextToSpeechService scaffold.** A
  buildable skeleton that exposes the converted engine as a system TTS engine:
  a JNI bridge (`eloquence_jni.c`, compiles clean against NDK r27c), a Kotlin
  `EloquenceTtsService` + `EloquenceNative`, manifest/Gradle/CMake, and
  `stage-libs.sh` which converts all 19 modules with `--os android` and lays
  them out under `jniLibs/arm64-v8a/`. Validated end-to-end at build level
  (staging converts all 19; JNI links); **not yet run on a device/emulator** —
  that and the prosody/voice tuning are the next steps. Does not touch the
  shipped Linux artifacts. See `android/README.md`.

## [1.2.2] — 2026-06-09

### Added

- **Android (`--os android`) is now symbol-complete: all 19 modules link with
  zero unresolved imports against the NDK's Bionic + libc++.** A full
  import-vs-Bionic-export audit surfaced four real gaps beyond the 1.2.1
  prototype, all now closed:
  - **libc++ inline-namespace mismatch (the big one).** Apple built the engine
    against `std::__1`; the NDK's libc++ renames its inline namespace to
    `std::__ndk1`, so the engine's 26 iostream/locale/string imports (mangled
    `_ZNSt3__1…`) didn't resolve. The ABI is otherwise identical and
    substitution refs are positional, so `rename_import` rewrites
    `St3__1` → `St6__ndk1` on Android targets — binding functions, vtables,
    VTTs, type-infos, and facet ids straight to `libc++_shared.so`.
  - **`_Unwind_Resume`** — Bionic only exports it from libc.so at API 34+; the
    Android link now pulls the NDK's static `libunwind` so the `.so` is
    self-contained on older devices.
  - **`bzero` / `ftime`** — dropped from Bionic (inlined / obsolete); provided
    as small `BIONIC_COMPAT_STUBS` in `stubs.c` (Android-gated).

### Fixed

- **Linker script now catches function-sections (`.text.*`,
  `.gcc_except_table.*`, `.bss.*`).** `libunwind`/`clang_rt` are built with
  `-ffunction-sections`, so they contribute `.text._Unwind_RaiseException` &c.;
  without the broad wildcards lld placed them as orphans overlapping the pinned
  `.m2e_*` sections. Harmless for the existing glibc/bfd builds (verified
  byte-identical synthesis on Linux arm64 + x86_64), required for Android.

Android remains **not device-tested and not in CI** — symbol resolution is now
proven, but on-device runtime (Bionic linker, file sandboxing) is the next
step, along with the JNI / `TextToSpeechService` integration.

## [1.2.1] — 2026-06-09

### Fixed

- **arm64 variadic fix was incomplete — fortified and `va_list`-taking libc
  calls still passed garbage.** 1.2.0 bridged the plain variadic functions
  (`sprintf`/`printf`/`fprintf`/`sscanf`) but missed two related cases the
  engine also imports: the FORTIFY variant `__sprintf_chk` (variadic), and
  functions that *take* a `va_list` — `vfprintf` and `__vsprintf_chk`. On
  Apple arm64 a `va_list` is a bare `char*` to the stacked args, whereas
  AAPCS64's is a 5-field struct, so a forwarded list is misread just like a
  register/stack mismatch. Added the `_chk` variadic trampoline plus a second
  shim family (`M2E_VL_SHIM`) that wraps an incoming Apple `va_list` into an
  AAPCS64 struct. All three new shims are unit-tested (integer/string/pointer
  args round-trip correctly). These paths weren't exercised by the simple
  smoke phrases, so 1.2.0 audio was unaffected — but they were latent crashes.
- **Orphan `.data` from `stubs.o` is now explicitly placed in the linker
  script.** It holds `__stack_chk_guard`; without explicit placement the LLVM
  linker (lld) assigns it an address that overlaps the pinned `.m2e_*`
  sections. GNU bfd happened to place it safely, so Linux builds were fine,
  but being explicit fixes both linkers (and is required for the Android path,
  which uses lld).

### Added

- **Experimental `--os android` target (arm64/Bionic).** `macho2elf.py` can now
  emit Android-arm64 `.so` files: same CPU/ABI fixes as Linux arm64 (they all
  key off the `arm64` suffix), plus Bionic specifics — NDK clang, unversioned
  imports, `libc.so`/`libm.so`/`libdl.so`/`libc++_shared.so` `DT_NEEDED`, and
  `__errno_location` → `__errno`. Verified to build a correct Bionic ELF
  against NDK r27c (zero `@GLIBC` symbols, correct `NEEDED`). **Not yet
  device-tested and not built in CI** — the converted libs are usable from an
  Android app via JNI, but the integration layer (a `TextToSpeechService`) is
  out of scope here. See `ARCH_CONFIG["android-arm64"]`.

## [1.2.0] — 2026-06-08

### Added

- **aarch64/arm64 is a shipped, working target again.** Synthesis on real
  arm64 used to segfault; three distinct converter/runtime gaps were the
  cause, all fixed here. arm64 output now synthesizes the same waveform as
  x86_64 (verified end-to-end under qemu — identical sample counts, max
  deviation ~0.01% of peak, i.e. sub-LSB floating-point rounding). The
  aarch64 entry is restored to the release matrix and resampler previews now
  render on both arches.

### Fixed

- **arm64 high8 tagged-pointer rebases were silently dropped.** Apple's arm64
  chained-fixup rebases can carry a `high8` tag in the top byte of the pointer
  (a top-byte-ignore tagged pointer); LIEF surfaces it in bits 56..63 of
  `r.target`. The converter mapped the *tagged* value to a section, found it
  out of range, and skipped the rebase — leaving raw chained-fixup bytes in the
  slot (garbage pointers, e.g. `eciVersion` returned addresses instead of
  `6.1.0.0`). On eci alone, 30 rebases were dropped. The tag is now stripped
  for section lookup and folded back into the emitted addend so the pointer
  keeps its tag. x86_64 never sets high8, so it was unaffected. All 19 modules
  now convert with **0 dropped rebases** (`tools/audit_relocs.py` confirms
  ground-truth == converter), and `tools/dump_chained_fixups.py` uses the
  correct 36-bit target mask.
- **arm64 variadic libc calls passed garbage.** Apple's arm64 ABI passes *all*
  variadic arguments on the stack; Linux AAPCS64 passes the first integer/FP
  variadic args in `x2..x7`/`v0..v7`. So every `sprintf`/`printf`/`fprintf`/
  `sscanf` call from the engine laid its args where glibc never reads them. The
  converter now redirects those imports (arm64 only) to tiny asm trampolines in
  `stubs.c` that rebuild a stack-only `va_list` (`__gr_offs = __vr_offs = 0`)
  and forward to the `v*` variant — correct for integer, pointer, and FP args.
- **arm64 `__chkstk_darwin` was unresolved.** Apple emits this stack-probe in
  large-frame prologues (arm64 only). The converted language modules failed to
  load (`undefined symbol: __chkstk_darwin`). A register-preserving no-op stub
  (Linux grows the thread stack on demand) is now provided in `stubs.c`.
- **arm64 `.so` files carried no libc++ dependency.** The empty link-time stub
  libs were dropped by `--as-needed`, so the converted `.so` had no
  `DT_NEEDED` for libc++ — its C++ symbols (`operator new`, `__cxa_*`,
  `_Unwind_Resume` via libc++abi → libgcc_s) were unresolved and `dlopen`
  failed without a manual preload. The stubs are now linked under
  `--no-as-needed` so the soname is recorded and ld.so pulls the real libc++
  chain at load time. (x86_64 already linked the real libc++.)

## [1.1.4] — 2026-06-08

### Fixed

- **Converted `.so` files were ~7x larger than they needed to be.**
  The generated linker script assigned `.eh_frame`,
  `.eh_frame_hdr`, and `.bss` to the `rwdata` `PT_LOAD` segment but
  emitted them *after* the big `auxtext` vaddr push (`. = ALIGN(0x100000);
  . = . + 0x100000;`).  Because `.eh_frame`/`.eh_frame_hdr` are
  `PROGBITS`, the RW segment's file image was forced to span from
  `~0x21000` all the way to `~0x200000`, so the linker wrote ~1.8 MB
  of zero padding into every output `.so`.  These sections now sit
  contiguous with the dynamic tables, *before* the push; the
  `auxtext` segment is a separate `PT_LOAD`, so its high vaddr costs
  nothing on disk.  `eci.so` drops 2.0 MB -> 308 KB; `enu.so` drops
  4.3 MB -> 2.3 MB.  No symbols are lost and exports are unchanged.

### Changed

- **Output `.so` files are now stripped by default** (`-Wl,-s` at
  link time).  Engine exports live in `.dynsym`; the non-dynamic
  `.symtab` of local `.m2e_*` labels is dead weight at runtime (tens
  of KB to ~100 KB per module).  Pass `--no-strip` to `macho2elf.py`
  to keep it.  Combined with the `.eh_frame` fix, `eci.so` is 265 KB
  (8.1x smaller) -- smaller than the source Mach-O slice.

## [1.1.3] — 2026-05-14

### Fixed

- **Orca rate/pitch/volume sliders had no effect.** The module
  tracked rate/pitch/volume per voice slot and used `INT_MIN`
  sentinels (= "use preset default") whenever `SET VOICE_TYPE`,
  `SET LANGUAGE`, or `SET SYNTHESIS_VOICE` arrived from
  speech-dispatcher.  Since speech-dispatcher sends those commands
  alongside `SET RATE` in arbitrary order per utterance, any rate
  that was just set got wiped before the speak fired -- every
  single time.  Rate/pitch/volume are now session-wide globals (per
  the SSIP protocol's intent); every voice activation re-applies
  the current session values so sliders persist across voice and
  language changes.

### Added

- **`EloquenceUtteranceTailMs`** (default 25, range 0..200).  The
  trailing-silence pad that absorbs the pulse/alsa stream-drain
  trim at end-of-utterance is now tunable.  Lower values feel
  snappier when Orca chains utterances back-to-back; higher values
  fully protect the speech but add an audible gap.  `0` disables
  the pad entirely.

### Changed

- **Release tarballs are x86_64-only.**  aarch64 binaries built
  successfully but synthesis segfaults on real arm64 hardware
  (deeper converter and C++ runtime gaps).  The matrix entry is
  dropped from `release.yml` until those are fixed; the README's
  project-status table reflects this.
- Per-asset `*.sha256` files are no longer generated.  The GitHub
  Releases page already shows SHA256 next to every downloaded
  asset, so the external files were redundant noise.

## [1.1.2] — 2026-05-13

Patch release that supersedes [1.1.1] -- if you're on 1.1.1 you want
this one.

### Fixed

- **Word cutoff regression in 1.1.1.** The SIGPIPE handler added in
  1.1.1 caused a slight cutoff at the end of utterances in
  pass-through mode (the SIG_IGN disposition was being inherited
  into engine threads that expected pipe-closed errors to surface
  normally).  The SIGPIPE handler is fully reverted -- it didn't fix
  the resampler crash it was intended for anyway (see next item).
- **Resampler SEGSEGV at high `EloquenceResampleRate`.** Real cause
  was a libsoxr state-machine misuse: `resampler_flush` ran
  `soxr_process(NULL, ...)` to drain the polyphase tail but never
  called `soxr_clear()` afterwards; the next utterance's first
  `soxr_process()` then dereferenced stale libsoxr state and
  segfaulted inside `soxr_process` (verified via core-dump stack).
  `audio_sink_flush` now loops on `resampler_flush` until libsoxr
  reports 0 samples drained (libsoxr's documented drain pattern,
  needed for long-tailed filters like very-high-quality + linear
  phase) and then calls a new `resampler_clear()` to reset for the
  next stream.
- **Slight cutoff at end of every utterance** (pre-existing; not a
  1.1.1 regression).  The pulse / alsa stream-drain trims the last
  few ms of audio at end-of-stream.  Each utterance now ends with
  ~100ms of trailing silence at the engine's native rate; the
  backend trims silence instead of speech.  Cancel paths skip the
  pad so stop is still snappy.

### Changed

- Release workflow now extracts the per-version section out of
  `CHANGELOG.md` and uses it as the GitHub Release body, instead of
  GitHub's auto-generated "What's Changed" list.  Forces the
  CHANGELOG to be updated before any tag push -- the release build
  fails if the section is missing.

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
