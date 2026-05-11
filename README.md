# apple-eloquence-elf

Convert Apple's bundled ETI Eloquence TTS engine (Mach-O dylibs from the
TextToSpeechKona framework) to Linux ELF shared objects. The genuine ETI
Eloquence 6.1 speech synthesizer, running natively on Linux x86_64 and arm64.

```
$ ./examples/speak ./prebuilt/x86_64/eci.so "Hello world."
eciVersion: '6.1.0.0'
PCM: 27907 samples (2.53s @ 11025Hz), peak amplitude 18103
Wrote /tmp/eci_out.s16
$ aplay -r 11025 -f S16_LE /tmp/eci_out.s16
```

## What this is

Apple ships ETI Eloquence as part of VoiceOver across macOS, iOS, iPadOS, and
tvOS. Inside `TextToSpeechKonaSupport.framework` they bundle a set of dylibs
that ARE the genuine ETI ECI 6.1 engine (the same C++ source tree ETI /
Speechworks shipped historically), compiled for Apple platforms. Crucially,
those dylibs depend only on `libSystem.B.dylib` and `libc++.1.dylib` — no
Apple-framework deps — which makes them tractable to retarget.

> Note on the name: the engine is **Eloquence**, originally from Eloquent
> Technologies, Inc. (ETI). Speechworks (later ScanSoft, later Nuance,
> later Microsoft) acquired ETI in 2003. IBM had its own ECI-licensed
> fork shipped as ViaVoice TTS / IBMTTS, but the mainline engine —
> including what Apple ships today — descends from the ETI tree.

This project provides `macho2elf.py`: a Python+LIEF converter that takes
those Mach-O dylibs and produces ELF `.so` files that load on Linux via
`dlopen()` and expose the standard ECI C API.

## Project status

| Architecture | Builds | Runtime tested | Languages |
|---|---|---|---|
| **x86_64 Linux** | ✅ | ✅ end-to-end speech | 8 of 19 modules verified (see below) |
| **aarch64 Linux** | ✅ | ⚠️ build-verified only; needs real arm64 hw | same 19 modules |

The shipped binaries are built from the **tvOS 18.2 Simulator Runtime**
which adds CJK + romanization language support over the older 16.4 release.

**Verified working language modules** (x86_64):
- `enu` US English, `eng` UK English
- `deu` German, `fra` French (FR), `frc` French (Canada), `esp` Spanish (Spain), `esm` Spanish (Mexico/LatAm), `ita` Italian, `fin` Finnish, `ptb` Portuguese (Brazil)
- `jpn` Japanese, `kor` Korean, `chs` Chinese Simplified, `cht` Chinese Traditional

**Romanization helper modules** (not standalone synthesizers, require integration as secondary modules):
- `jpnrom`, `korrom`, `chsrom`, `chtrom`

## Quick start (use the prebuilt binaries)

```bash
# 1. Build everything (macho2elf script, sd_eloquence module, example binaries)
cmake -B build
cmake --build build

# 2. Set up an eci.ini pointing at the language module
cd prebuilt/x86_64
cp ../../examples/eci.ini ./eci.ini
sed -i "s|^Path=.*$|Path=$(pwd)/enu.so|" eci.ini

# 3. Speak
../../build/examples/speak ./eci.so "Hello from Apple's Eloquence on Linux."
aplay -r 11025 -f S16_LE /tmp/eci_out.s16
```

## System install (any distro)

`cmake --install` puts everything in standard FHS paths:

```bash
# Build tools + speech-dispatcher module
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build

# Install (needs root for /usr and /etc paths)
sudo cmake --install build
```

Files placed:
- `/usr/bin/macho2elf` — converter CLI
- `/usr/lib/speech-dispatcher-modules/sd_eloquence` — speechd module binary
- `/etc/speech-dispatcher/modules/eloquence.conf` — module config template
- `/usr/share/doc/apple-eloquence-elf/` — README + docs

Runtime deps the install does NOT pull in (install via your distro):

| Component | Arch | Debian/Ubuntu | Fedora |
|---|---|---|---|
| LIEF (Python) | `python-lief` | `python3-lief` | `python3-lief` |
| C++ runtime | `libc++ libc++abi` | `libc++1 libc++abi1` | `libcxx libcxxabi` |
| Resampler (optional) | `libsoxr` | `libsoxr0` | `soxr` |
| llvm tools (for `llvm-lipo`) | `llvm` | `llvm` | `llvm` |
| aarch64 cross-build (optional) | `aarch64-linux-gnu-{gcc,binutils}` | `gcc-aarch64-linux-gnu` | `gcc-aarch64-linux-gnu` |

Then convert your own Apple dylibs (see `docs/02-conversion.md`) or grab pre-converted release artifacts from the [GitHub Releases page](../../releases) (built by CI on a macOS runner from the actual Apple SDK).

To try Japanese, Korean, Chinese, etc: change the `Path=` to `jpn.so`,
`kor.so`, `chs.so`, `cht.so` and so on. See `docs/03-integration.md` for
the complete API guide.

Required runtime libraries on x86_64 Linux:
- glibc (any reasonable version)
- libc++.so.1 + libc++abi.so.1 (Arch: `pacman -S libc++ libc++abi`; Debian: `apt install libc++1 libc++abi1`)

## Building from scratch (run the converter yourself)

If you want to rebuild from the Apple originals (or convert a different version):

```bash
# 1. Get Python deps
python3 -m venv venv && ./venv/bin/pip install lief

# 2. Convert. The input must already be a single-arch slice — use llvm-lipo
#    to extract from a universal binary:
llvm-lipo -extract x86_64 vendor/tvOS-18.2/eci.dylib -output /tmp/eci.x86_64
./venv/bin/python3 macho2elf/macho2elf.py /tmp/eci.x86_64 -o /tmp/eci.so
```

See `docs/02-conversion.md` for the full recipe.

## Repo layout

```
macho2elf/macho2elf.py    ~860 LOC Python+LIEF converter (this project's main code, MIT)
vendor/tvOS-18.2/         Unmodified Apple Mach-O dylibs from tvOS 18.2 Simulator Runtime
prebuilt/x86_64/          Pre-converted ELF .so files for Linux x86_64
prebuilt/aarch64/         Pre-converted ELF .so files for Linux aarch64
examples/speak.c          Full TTS C example using dlopen + the ECI callback API
examples/eci.ini          Minimal config (the engine has built-in voice/phoneme defaults)
sd_eloquence/             Native Speech Dispatcher module sources. Built by
                          the root CMakeLists; install drops the binary where
                          speechd auto-discovers it. Optional libsoxr resampling.
docs/                     Extraction, conversion, integration, internals, troubleshooting
tools/checksums.txt       SHA256 of every shipped binary + the source DMG
tools/verify.sh           Verify shipped binaries match expected checksums
```

## Speech Dispatcher integration

A native speech-dispatcher output module ships in `sd_eloquence/`, built as
part of the root CMake project. After `sudo cmake --install build`, edit
`/etc/speech-dispatcher/modules/eloquence.conf` to point `EciLibrary` at
your `eci.so` and `EciVoicePath` at a language module, then restart
speech-dispatcher. Anyone familiar with speech-dispatcher modules will
recognise the SSIP-on-stdio shape; the conf file documents the
Eloquence-specific knobs (sample rate, default voice/language, optional
libsoxr resampling).

## How it works (the very short version)

The converter walks each section of the Mach-O dylib, emits an assembly stub
that interleaves `.incbin` of the original code/data bytes with section-relative
labels for exports and `.quad` references for chained-fixup bindings, generates
a linker script that pins each section at its original Mach-O virtual address
(preserving all RIP-relative offsets unchanged), and runs `gcc -shared` to
produce a valid ELF DSO. Symbol prefixes get stripped (`_atoi` → `atoi`), a
handful of Darwin-specific symbols get renamed (`___error` → `__errno_location`,
`___stderrp` → `stderr`), and small stub C functions provide a few names that
have no direct Linux equivalent (`__maskrune`, `__stack_chk_guard`, the
`_DefaultRuneLocale` ctype placeholder).

Full details in `docs/04-internals.md`.

## Provenance and licensing

- **The converter** (`macho2elf/`) is original work, licensed under MIT.
  See `LICENSE`.
- **The shipped Mach-O dylibs** under `vendor/tvOS-18.2/` are unmodified Apple
  binaries extracted from the tvOS 18.2 Simulator Runtime IPSW. SHA256 checksums
  for both the source DMG and each extracted file are in `tools/checksums.txt`.
  These remain subject to Apple's SDK terms.
- **The converted `.so` files** under `prebuilt/` are produced by running
  `macho2elf.py` on those Apple binaries — they are derivative works of
  Apple's distribution.
- **`examples/eci.ini`** is a minimal hand-authored configuration that uses
  the engine's built-in defaults. It's structured around the ECI 6.1 SDK
  format and observed in the LevelStar Icon distribution.

If you intend to redistribute or productize this, the conservative path is
to download your own copy of a tvOS Simulator Runtime from Apple's developer
portal and build locally — see `docs/01-extraction.md`.

## Acknowledgements

- Eloquent Technologies, Inc. (ETI), and the subsequent Speechworks / ScanSoft /
  Nuance custodians of the source tree, for the original engine that we're all
  still enjoying decades later.
- IBM for the parallel ViaVoice TTS / IBMTTS fork, which contributed back to
  the mainline ECI codebase in places.
- LevelStar for keeping a working Linux ECI distribution alive (their Icon
  product). Their published `eci.ini` informed our minimal template.
- Agner Fog (`objconv`) and the LIEF project for binary-format tooling that
  made this tractable.
- Anthropic's Claude Code agent did most of the engineering pair-programming.
