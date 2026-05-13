# 03 — Integrating Eloquence into your application

Once you have `eci.so` and a matching language module (`enu.so` for US English,
etc.), the integration is the standard ECI 6.x C API. This document covers
the gotchas specific to the converted dylibs.

## Files you need at runtime

```
your_app/                                  Or wherever you run from
├── (your binary)
├── eci.so                                 Converted control library
├── enu.so                                  Converted English module (or other language)
└── eci.ini                                 Config file (see below)
```

`eci.ini` MUST be in the current working directory of your process. The
engine looks for it there. It also looks for an optional `eci.dbg` file
(O_RDWR) — if you create it, the engine writes verbose debug logs to it.

## Minimum eci.ini

The bundled `examples/eci.ini` is intentionally minimal:

```
[1.0]
Path=/absolute/path/to/your/enu.so
Version=6.1
```

The `Path=` line is the absolute path to the language module. Update it
to point at your installed module of choice (`enu.so`, `eng.so`,
`deu.so`, `fra.so`, etc. — see "Language modules available" below).

The engine has fully-tuned built-in voice and phoneme defaults that
produce intelligible speech without further configuration.

**Why minimal?** The tvOS 18.2 Eloquence build has internal config-table
buffers that overflow if you include too many `Voice<N>=` or `Phoneme<N>=`
tuning entries (such as those in LevelStar Icon's eci.ini). The overflow
silently corrupts a Klatt-hook callback's instance-data slot, and the
engine then segfaults during `eciAddText`. Stick to the minimal form and
the engine works cleanly.

If you need to override voice or phoneme parameters, do it at runtime
via `eciSetVoiceParam` calls — not in eci.ini.

**Section-name quirk:** the engine's iniReader also has a 10-byte buffer
for section names. Names longer than 7 chars (after brackets) crash the
engine with a stack canary smash. `[1.0]` is fine; `[LanguageIndependent]`
is not. Use short section names.

## Language modules available

The tvOS 18.2 set ships 19 language dylibs:

**Standalone synthesizers — v1 supported** (use as `Path=...so`):
- `enu.so` US English, `eng.so` UK English
- `deu.so` German
- `fra.so` French (FR), `frc.so` French (Canada)
- `esp.so` Spanish (Spain), `esm.so` Spanish (Mexico/LatAm)
- `ita.so` Italian, `fin.so` Finnish, `ptb.so` Portuguese (Brazil)

**Standalone synthesizers — v1 gated (CJK)**:
- `jpn.so` Japanese, `kor.so` Korean
- `chs.so` Chinese Simplified, `cht.so` Chinese Traditional

These dylibs convert and load, but `sd_eloquence` rejects them at
language-select time pending v2 work on the 2-suffixed ECI API. If
you're driving `eci.so` directly via dlopen (not via sd_eloquence),
they may work for short utterances but will crash on process exit —
see `docs/cjk-investigation/` for the full story.

**Romanization helper modules** (`*rom.so`): auxiliary transliterators
used internally by the engine when consuming already-romanized text.
Don't use these directly as `Path=` — they're not complete synthesizers.

(The older tvOS 16.4 set has 10 modules — same as 18.2 minus all the
CJK ones.)

## Required runtime dependencies

```
ldd ./eci.so
        linux-vdso.so.1
        libc.so.6 => /usr/lib/libc.so.6
        libm.so.6 => /usr/lib/libm.so.6
        libc++.so.1 => /usr/lib/libc++.so.1
        libc++abi.so.1 => /usr/lib/libc++abi.so.1
        libpthread.so.0 => /usr/lib/libpthread.so.0   (provided by libc on modern glibc)
        libdl.so.2 => /usr/lib/libdl.so.2             (provided by libc on modern glibc)
```

Most distros bundle libpthread/libdl into libc since glibc 2.34. The
critical one to ensure is **`libc++.so.1`** + **`libc++abi.so.1`** — Apple
compiled the engine against libc++ (not libstdc++), and the `std::__1`
namespace symbols are exclusively in libc++. On systems that only have
libstdc++, you need to install libc++ separately:

- **Arch**: `sudo pacman -S libc++ libc++abi`
- **Debian/Ubuntu**: `sudo apt install libc++1 libc++abi1`
- **Fedora**: `sudo dnf install libcxx libcxxabi`

## API surface

The ECI C API is documented in the historical Eloquence SDK (ETI / Speechworks).
The key entry
points exposed by `eci.so`:

| Function | Purpose |
|---|---|
| `void* eciNew()` | Create an engine instance with default language |
| `int eciDelete(void *h)` | Destroy an instance |
| `void eciVersion(char *buf)` | Write version string to buf (recommend 16+ bytes) |
| `int eciRegisterCallback(void *h, ECICallback cb, void *data)` | Register a callback for audio/phoneme/index messages |
| `int eciSetOutputBuffer(void *h, int size, void *buf)` | Tell engine where to write PCM. **Must be called AFTER eciRegisterCallback.** |
| `int eciAddText(void *h, const char *text)` | Queue text for synthesis |
| `int eciSynthesize(void *h)` | Trigger synthesis of queued text |
| `int eciSynchronize(void *h)` | Block until synthesis is complete |
| `int eciSetParam(void *h, int param, int value)` | Adjust engine parameters |
| `int eciSetVoiceParam(void *h, int voice, int param, int value)` | Per-voice parameter tuning |

There are many more; `nm -D --defined-only eci.so | grep ' T eci' | sort`
gives the complete list.

## The callback contract

```c
typedef enum {
    eciWaveformBuffer = 0,
    eciPhonemeBuffer  = 1,
    eciIndexReply     = 2,
    eciPhonemeIndexReply = 3
} ECIMessage;

typedef enum {
    eciDataNotProcessed = 0,
    eciDataProcessed    = 1,
    eciDataAbort        = 2
} ECICallbackReturn;

typedef ECICallbackReturn (*ECICallback)(
    void *hEngine, int msg, long lParam, void *pData
);
```

When the engine has PCM ready, it calls your callback with:
- `msg = eciWaveformBuffer`
- `lParam` = number of samples written into the buffer you set via
  `eciSetOutputBuffer`
- `pData` = the user-data pointer you passed to `eciRegisterCallback`

PCM is **16-bit signed mono at 11025 Hz** by default. The output buffer
gets reused — drain it (copy out, write to disk, push to audio device)
from inside the callback. Return `eciDataProcessed` to continue, or
`eciDataAbort` to stop early.

A complete working example is in `examples/speak.c`.

## Common parameters

Engine-wide params (used with `eciSetParam` / `eciGetParam`). The numbering
has deliberate gaps — slots 4, 6, 11 are unused in ECI 6.x and must not be
touched. The authoritative declarations live in `sd_eloquence/src/eci/eci.h`.

```c
#define eciSynthMode               0   /* 0=screen reader, 1=TTS general text */
#define eciInputType               1   /* 0=character, 1=phonetic, 2=TTS */
#define eciTextMode                2   /* 0=normal, 1=alphanumeric, 2=verbatim, 3=spell */
#define eciDictionary              3   /* 0=use dictionary, 1=skip */
#define eciSampleRate              5   /* 0=8 kHz, 1=11025 Hz, 2=22050 Hz */
#define eciWantPhonemeIndices      7
#define eciRealWorldUnits          8
#define eciLanguageDialect         9
#define eciNumberMode             10
#define eciWantWordIndex          12
#define eciNumDeviceBlocks        13
#define eciSizeDeviceBlocks       14
#define eciNumPrerollDeviceBlocks 15
#define eciSizePrerollDeviceBlocks 16
```

Apple's eci.dylib 6.1 only accepts `eciSampleRate` values 0 and 1 — passing
2 returns -1. Other ECI builds (Speechworks/IBMTTS era, voxin) accept all
three.

Per-voice params (used with `eciSetVoiceParam`):
```c
#define eciGender           0   /* 0=male, 1=female */
#define eciHeadSize         1
#define eciPitchBaseline    2
#define eciPitchFluctuation 3
#define eciRoughness        4
#define eciBreathiness      5
#define eciSpeed            6
#define eciVolume           7
```

## Common pitfalls

1. **`eciSetOutputBuffer` before `eciRegisterCallback`** — returns
   "NULL eciHandle or no callback" error and does nothing.

2. **eci.ini section names > 9 chars** — engine crashes with stack canary
   smash. Use `[1.0]` or other short names.

3. **Missing `libc++.so.1`** — dlopen fails with `cannot open shared object
   file`. Install libc++ from your distro.

4. **`Path=` pointing at non-existent file** — engine fails silently;
   `eciNew()` returns NULL. Always use absolute paths.

5. **Running from a directory without write permission** — engine can't
   create `eci.dbg`; not fatal but causes a noisy `openat` failure under
   strace.
