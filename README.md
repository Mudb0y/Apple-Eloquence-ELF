# apple-eloquence-elf

Convert Apple's bundled ETI Eloquence TTS engine (Mach-O dylibs from
the TextToSpeechKona framework) to Linux ELF shared objects, and ship
them behind a native speech-dispatcher module.

> [!IMPORTANT]
> **AI-assisted development.** Most of this project was implemented in
> pair-programming sessions with Anthropic's Claude. Every change is
> human-reviewed and validated through end-to-end speech testing with
> Orca + speech-dispatcher and container-based install testing on
> Arch, Debian, Ubuntu, and Fedora — but the implementation work
> itself is largely AI-generated. The `Co-Authored-By` trailer on
> each commit makes the split visible. Decide whether you're
> comfortable with that posture before depending on it.

```
$ ./examples/speak ./prebuilt/x86_64/eci.so "Hello world."
eciVersion: '6.1.0.0'
PCM: 27907 samples (2.53s @ 11025Hz), peak amplitude 18103
Wrote /tmp/eci_out.s16
$ aplay -r 11025 -f S16_LE /tmp/eci_out.s16
```

## What this is

Apple ships ETI Eloquence as part of VoiceOver across macOS, iOS,
iPadOS, and tvOS. Inside `TextToSpeechKonaSupport.framework` they
bundle dylibs that are the ETI ECI 6.1 engine compiled for Apple
platforms. The dylibs depend only on `libSystem.B.dylib` and
`libc++.1.dylib`, which makes them tractable to retarget.

> Eloquence originated at Eloquent Technologies, Inc. (ETI).
> SpeechWorks acquired ETI in 2000; ScanSoft acquired SpeechWorks in
> 2003 and renamed itself Nuance Communications in 2005. Nuance spun
> off its automotive AI division as Cerence in 2019, taking the
> Eloquence / Vocalizer TTS stacks with it. Microsoft's 2022
> acquisition of Nuance did not include those TTS engines, so
> Eloquence's current owner is Cerence, not Microsoft. IBM had its
> own ECI-licensed fork shipped as ViaVoice / IBMTTS; the mainline
> engine — including what Apple ships — descends from the ETI tree.

`macho2elf.py` is the converter: a Python + LIEF tool that produces
ELF `.so` files exposing the standard ECI C API.

## Project status

| Architecture | Builds | Runtime |
|---|---|---|
| x86_64 Linux | ✅ | ✅ tested end-to-end with sd_eloquence + Orca |
| aarch64 Linux | ✅ | ⚠️ build-verified only |

Shipped binaries are built from the tvOS 18.2 Simulator Runtime.

Working languages on x86_64: en-US, en-GB, es-ES, es-MX, fr-FR, fr-CA,
de-DE, it-IT, pt-BR, fi-FI. CJK (ja-JP, ko-KR, zh-CN, zh-TW) is gated
for v1 — the romanizer init path needs the modern 2-suffixed ECI API
rather than the legacy one v1 uses. Deferred to v2; background in
`docs/cjk-investigation/` and `docs/eci-2-api/`.

## Install (from the release tarball)

```bash
tar -xzf apple-eloquence-elf-*-linux-x86_64.tar.gz
cd apple-eloquence-elf-*-linux-x86_64
sudo ./install.sh
spd-say -o eloquence "Hello from Eloquence."
```

`install.sh` detects the host distro family (Debian / Ubuntu /
Fedora / Arch / openSUSE) and uses the system package manager to
install every runtime dependency (speech-dispatcher, libc++, libsoxr,
libxml2, pcre2) before dropping files into `/usr/lib/eloquence/`,
the speech-dispatcher modulebindir, and
`/etc/speech-dispatcher/modules/eloquence.conf`.

Configure the module by editing
`/etc/speech-dispatcher/modules/eloquence.conf` and restarting
speech-dispatcher. Audio previews of every libsoxr resampler preset
ship at `/usr/share/eloquence/resampler-previews/`.

Uninstall: `sudo ./uninstall.sh` (add `--purge` to also remove the
conf template).

## Build from source

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

`cmake --install` does **not** touch the system package manager.
Install build and runtime deps yourself: `cmake`, `gcc`, `llvm` (for
`llvm-lipo`), `python3` with the `lief` package, plus dev packages
for speech-dispatcher, libc++ / libc++abi, libsoxr, libxml2, and
libpcre2-8. CMake will name any missing ones at configure time.

Files placed:
- `/usr/bin/macho2elf` — converter CLI
- `<speechd modulebindir>/sd_eloquence` — module binary
- `/etc/speech-dispatcher/modules/eloquence.conf` — config template
- `/usr/share/doc/eloquence/` — README + docs

## Convert your own dylibs

```bash
python3 -m venv venv && ./venv/bin/pip install lief
llvm-lipo -extract x86_64 vendor/tvOS-18.2/eci.dylib -output /tmp/eci.x86_64
./venv/bin/python3 macho2elf/macho2elf.py /tmp/eci.x86_64 -o /tmp/eci.so
```

Full recipe (including extraction from a tvOS Simulator Runtime DMG):
`docs/01-extraction.md` and `docs/02-conversion.md`.

## Speech-dispatcher module

`sd_eloquence/` is a native speech-dispatcher output module. It
exposes the eight Apple voice presets (Reed, Shelley, Sandy, Rocko,
Flo, Grandma, Grandpa, Eddy; Jacques replaces Reed in French),
transcribed verbatim from `KonaVoicePresets.plist`. Every supported
language is selectable on the fly via speech-dispatcher's `language=`
parameter; no per-language conf edits.

Configuration reference: `docs/03-integration.md` plus the comments
in `eloquence.conf` itself.

## How it works

The converter walks each section of the Mach-O dylib, emits an
assembly stub that interleaves `.incbin` of the original code/data
with section-relative labels for exports and `.quad` references for
chained-fixup bindings, generates a linker script that pins each
section at its original Mach-O virtual address (preserving
RIP-relative offsets), and runs `gcc -shared`. Symbol prefixes are
stripped (`_atoi` → `atoi`), Darwin-specific symbols are renamed
(`___error` → `__errno_location`, `___stderrp` → `stderr`), and
small stub C functions cover names with no direct Linux equivalent
(`__maskrune`, `__stack_chk_guard`, `_DefaultRuneLocale`).

Details: `docs/04-internals.md`.

## Repo layout

```
macho2elf/macho2elf.py    Python + LIEF converter (MIT)
sd_eloquence/             speech-dispatcher module (GPL-2.0-or-later)
vendor/tvOS-18.2/         Unmodified Apple Mach-O dylibs
prebuilt/{x86_64,aarch64} Converted ELFs (gitignored)
examples/                 dlopen-based TTS sample (speak.c)
dist/                     install.sh / uninstall.sh / smoke.sh
tools/                    Converter audit + checksum tooling
docs/                     Extraction / conversion / integration / internals / troubleshooting
.github/workflows/        CI: convert vendor/ -> ELFs, package tarballs
```

## Licensing

- **`macho2elf/`** is MIT. See `LICENSE`.
- **`sd_eloquence/`** is GPL-2.0-or-later. It incorporates anti-crash
  regex tables and dictionary-loading patterns from the
  [NVDA-IBMTTS-Driver](https://github.com/davidacm/NVDA-IBMTTS-Driver)
  (Copyright (C) 2009-2026 David CM, GPL-2.0). Full text in
  `sd_eloquence/LICENSE.GPL`.
- **`vendor/tvOS-18.2/`** are unmodified Apple binaries from the tvOS
  18.2 Simulator Runtime IPSW, subject to Apple's SDK terms. SHA256
  checksums in `tools/checksums.txt`.
- **Converted `.so` files** are derivative works of Apple's
  distribution. Not committed; built locally or downloaded from the
  release tarball.

To redistribute or productize, the conservative path is to download
your own tvOS Simulator Runtime from Apple's developer portal and
convert locally; see `docs/01-extraction.md`.

## Acknowledgements

- ETI / SpeechWorks / ScanSoft / Nuance / Cerence — successive
  custodians of the engine.
- IBM ViaVoice TTS / IBMTTS, whose contributions to the mainline ECI
  codebase persist in this build.
- LevelStar, for keeping a working Linux ECI distribution alive in
  their Icon product; their `eci.ini` informed our minimal template.
- Agner Fog (`objconv`) and the LIEF project, for the binary-format
  tooling that made this tractable.
- Anthropic's Claude Code did most of the engineering pair-programming.
