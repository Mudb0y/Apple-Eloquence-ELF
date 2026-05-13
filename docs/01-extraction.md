# 01 — Extracting Apple's TextToSpeechKona framework

The shipped `vendor/tvOS-18.2/` directory contains exactly the dylibs you'd
get by performing the steps below against Apple's tvOS 18.2 Simulator Runtime.
This document records the recipe so you can verify the provenance, repeat
the extraction for a different tvOS/iOS/macOS version, or use your own
ADC-licensed copy of the SDK.

## What's in the framework

Apple bundles ETI Eloquence inside the private framework
`TextToSpeechKonaSupport.framework`. Inside that framework there's a
`Frameworks/` subdirectory containing the actual Eloquence dylibs:

```
TextToSpeechKonaSupport.framework/
├── TextToSpeechKonaSupport        Apple's Obj-C wrapper, NOT what we want
└── Frameworks/
    ├── eci.dylib                   Control library (ECI API surface)
    ├── enu.dylib                   US English synthesis
    ├── eng.dylib                   UK English synthesis
    ├── deu.dylib                   German
    ├── fra.dylib                   French (FR)
    ├── frc.dylib                   French (Canada)
    ├── esp.dylib                   Spanish (Spain)
    ├── esm.dylib                   Spanish (Mexico/LatAm)
    ├── ita.dylib                   Italian
    ├── fin.dylib                   Finnish
    ├── ptb.dylib                   Portuguese (Brazil)
    ├── jpn.dylib                   Japanese                       (18.2+)
    ├── jpnrom.dylib                Japanese romanization helper   (18.2+)
    ├── kor.dylib                   Korean                         (18.2+)
    ├── korrom.dylib                Korean romanization helper     (18.2+)
    ├── chs.dylib                   Chinese Simplified             (18.2+)
    ├── chsrom.dylib                CHS romanization helper        (18.2+)
    ├── cht.dylib                   Chinese Traditional            (18.2+)
    └── chtrom.dylib                CHT romanization helper        (18.2+)
```

Each dylib is a fat (universal) Mach-O containing both `x86_64` and `arm64`
slices. Each has only two dependencies: `libSystem.B.dylib` (libc) and
`libc++.1.dylib` (C++ stdlib).

## Where Apple ships it

The Eloquence framework appears across multiple Apple platforms:
- macOS 13 Ventura and later, in VoiceOver
- iOS 16+, iPadOS 16+, tvOS 16+ — same framework (CJK modules added in
  18.x / Ventura 14)
- Simulator Runtimes (Xcode downloads) — convenient because they ship
  the framework as plain files on disk rather than packed into
  `dyld_shared_cache`

Tip: the **tvOS 18.2 Simulator Runtime** is ideal because it includes the
full 19-module set (CJK + romanization variants) and is one of the last
ones Apple shipped with x86_64 slices.

## Recipe — tvOS 18.2 Simulator Runtime

This is the version `vendor/tvOS-18.2/` was extracted from.

### 1. Download the runtime

Sign in at <https://developer.apple.com/download/all/> with a free Apple
Developer account and search for "tvOS 18.2 Simulator Runtime". You'll
get a ~4.5 GB DMG file:

```
tvOS_18.2_Simulator_Runtime.dmg   (sha256: see tools/checksums.txt)
```

Alternatives: `ipsw download xcode --sim` from a Linux box
(blacktop/ipsw, provide your ADC auth cookie).

### 2. Extract with 7z (two passes)

Newer Apple Simulator Runtimes come packaged like a restore IPSW: the
outer DMG contains a `Restore/<long-id>.dmg` which is the actual rootfs.
7-Zip 22.00+ reads both layers; install a fresh 7zip if needed
(Arch: `sudo pacman -S p7zip`).

```
mkdir -p extracted && cd extracted
7z x -y ../tvOS_18.2_Simulator_Runtime.dmg
# Result: ./Restore/<numeric-id>.dmg + BuildManifest.plist + Firmware/
```

### 3. Pull the framework directly

You don't need to fully extract the inner DMG — `7z x` can target a path:

```
INNER=Restore/*.dmg
FRAMEWORK="Library/Developer/CoreSimulator/Profiles/Runtimes/tvOS 18.2.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/TextToSpeechKonaSupport.framework/*"

7z x -y "$INNER" "$FRAMEWORK"
```

Then copy the `Frameworks/` subdir to wherever you want:

```
cp -r "Library/Developer/CoreSimulator/Profiles/Runtimes/tvOS 18.2.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/TextToSpeechKonaSupport.framework/Frameworks/"*.dylib  vendor/tvOS-18.2/
```

That's it — 19 fat Mach-O files, ~52 MB total.

## Verifying the extraction

```
./tools/verify.sh
```

This compares the SHA256 of every file under `vendor/` and `prebuilt/`
against the expected values in `tools/checksums.txt`. If your extraction
matches ours, every line will say `OK`.

## tvOS 16.4

The tvOS 16.4 Simulator Runtime ships the same framework in a simpler
DMG layout (single APFS volume, `7z x` once) and tolerates the older
LevelStar-style eci.ini with Voice/Phoneme tuning entries — see
`docs/05-troubleshooting.md`. Conversion is identical to 18.2.
