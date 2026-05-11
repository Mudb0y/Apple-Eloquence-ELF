# sd_eloquence

A native Speech Dispatcher output module for **ETI Eloquence** (ECI 6.x).
Streams PCM directly to speech-dispatcher's audio server — no `sd_generic`
shell-wrapper indirection, no paplay child processes, clean STOP/PAUSE
interruption, full SSIP parameter mapping. Optional libsoxr-based output
resampling lets you replicate Apple's "Higher sample rate" VoiceOver
toggle (which is just AVAudioConverter under the hood).

Ships as part of the parent [apple-eloquence-elf](../README.md) project
but is self-contained — works with any libc-only ECI 6.x build:
- **Apple TextToSpeechKona dylibs** (converted via the parent repo's `macho2elf`)
- **Speechworks / IBMTTS** Linux distributions (2000-2006 era)
- **LevelStar Icon** bundled distribution
- **voxin** runtime (if you point `EciLibrary` at theirs)

Fresh implementation, written against the published pyibmtts SDK
type/constant definitions and cross-validated against Apple's runtime
behaviour. Deliberately does not reuse code from `viavoice-spd`, voxin's
module, or other existing Eloquence-based modules.

## Quick start

```bash
# 1. Install dependencies
sudo pacman -S speech-dispatcher cmake gcc        # Arch
# sudo apt install speech-dispatcher libspeechd-dev cmake gcc   # Debian

# 2. Build
cmake -B build -S .
cmake --build build

# 3. Install (binary + sample config)
sudo cmake --install build

# 4. Edit /etc/speech-dispatcher/modules/eloquence.conf:
#       EciLibrary       /path/to/eci.so
#       EciVoicePath     /path/to/enu.so

# 5. Register the module in /etc/speech-dispatcher/speechd.conf:
#       AddModule "eloquence" "sd_eloquence" "eloquence.conf"
#       DefaultModule eloquence

# 6. Restart speech-dispatcher and test
systemctl --user restart speech-dispatcher
spd-say -o eloquence "Hello from ETI Eloquence."
```

## What the module does

The module is a process speech-dispatcher launches as a child. It speaks the
SSIP protocol on stdin/stdout, manages an ECI engine instance, and forwards
the engine's PCM output to speech-dispatcher's audio server as it gets
synthesized.

```
                   ┌──────────────────┐
   Orca / spd-say │ speech-dispatcher │   stdin/stdout SSIP
            ───►  │       daemon      │ ◄────────────────► sd_eloquence
                   └──────────────────┘                      │
                          │                                   │ dlopen
                          │ audio                             ▼
                          ▼                              ┌─────────┐
                  PulseAudio / PipeWire                  │ eci.so  │
                                                          │ enu.so  │
                                                          └─────────┘
```

## Configuration

`eloquence.conf` (installed to `/etc/speech-dispatcher/modules/`):

| Key | Meaning |
|---|---|
| `Debug` | 0 = silent (default), 1 = verbose stderr |
| `EciLibrary` | Absolute path to `eci.so` (the engine control library) |
| `EciVoicePath` | Absolute path to a language module (e.g. `enu.so`) |
| `EloquenceSampleRate` | 0 = 8 kHz, 1 = 11025 Hz (default), 2 = 22050 Hz¹ |
| `EloquenceDefaultVoice` | 0-7 = preset voice slot (Wade, Flo, Bobbie, Reed, Brian, Dawn, Grandma, Grandpa) |
| `EloquenceDefaultLanguage` | ECI dialect code (decimal or `0x...` hex; see `src/eci.h`) |

¹ Apple's eci.dylib doesn't support 22 kHz; the module falls back to 11025
on rejection. Other ECI builds (Speechworks/IBMTTS era) accept all three.

## SSIP → ECI parameter mapping

| SSIP parameter | ECI mapping |
|---|---|
| `rate` (-100..+100) | `eciSpeed` (0..100) via linear remap |
| `pitch` (-100..+100) | `eciPitchBaseline` (0..100) via linear remap |
| `volume` (-100..+100) | `eciVolume` (0..100) via linear remap |
| `voice_type` MALE1..FEMALE3 | preset slot 0..6 |
| `synthesis_voice` "Wade", "Flo", ... | preset slot by name match |
| `language` "en", "de", ... | `eciLanguageDialect` lookup |
| `punctuation_mode` none/some/most/all | `eciTextMode` 1/0/0/3 |
| msgtype `SPD_MSGTYPE_CHAR` / `KEY` | `eciTextMode` 3 (spell mode) |

## Architecture

The TTS-specific code is in `src/sd_eloquence.c` (~340 LOC). speech-dispatcher's
shipped `libspeechd_module.so` provides `main()`, the protocol loop, and
audio-server plumbing — we only implement the callbacks declared in
`<speech-dispatcher/spd_module_main.h>`:

| Callback | Behaviour |
|---|---|
| `module_config` | Parses `eloquence.conf` |
| `module_init` | Writes a minimal eci.ini, `dlopen`s eci.so, calls `eciNew`, sets sample rate, registers audio callback |
| `module_list_voices` | Builds an SPDVoice* array across the 14 supported languages × 8 voices |
| `module_speak_sync` | Stops any in-flight synthesis, queues text via `eciAddText`, fires `eciSynthesize` + `eciSynchronize` |
| `module_stop` | Sets stop flag (callback returns `eciDataAbort`) + calls `eciStop` |
| `module_pause` | Calls `eciPause(h, 1)` |
| `module_close` | `eciDelete` + leave eci.so mapped until process exit (Apple's eci.dylib's C++ destructors run badly on `dlclose`) |
| `module_loop` | Just `module_process(STDIN_FILENO, 1)` — runs until QUIT |

`src/eci_runtime.c` is a thin `dlopen` wrapper that resolves every ECI
function we need from `eci.so` into a single struct of function pointers.
The module never link-time-depends on eci.so so users can point at any
build at runtime.

`src/eci.h` declares the ECI 6.x C API with the canonical parameter
numbering from the pyibmtts SDK (param 5 = `eciSampleRate`, not 3 — see
the comment block at top of the header). Cross-checked against Apple's
eci.dylib runtime behaviour.

## Testing without speech-dispatcher

You can drive the module manually for debugging:

```
cat > /tmp/transcript <<EOF
INIT
SET
rate=0
pitch=0
volume=0
.
SPEAK
Hello world.
.
QUIT
EOF

./build/sd_eloquence /etc/speech-dispatcher/modules/eloquence.conf < /tmp/transcript
```

The module will respond with SSIP status lines and emit `705-AUDIO` blocks
containing PCM. Useful for verifying engine init without involving the
speech-dispatcher daemon.

## Provenance

- **The module code** (`src/sd_eloquence.c`, `src/eci_runtime.c`, `src/eci.h`,
  `src/spd_audio.h`) is original work, MIT-licensed.
- **The ECI API constants** in `src/eci.h` are the type/enum declarations
  from the pyibmtts 0.x SDK (BSD-licensed by IBM, 2006-2007), refactored
  from the Pyrex `.pxi` files into a C header.
- **`libspeechd_module.so`** (the protocol loop runtime we link against)
  is shipped by speech-dispatcher (GPL-2.0+) — we use its public API only,
  no derivative-work concerns at the module API boundary.

## See also

- [apple-eloquence-elf](../apple-eloquence-elf) — converts Apple's bundled
  ETI Eloquence Mach-O dylibs to Linux ELF .so files, which this module
  loads via `EciLibrary`.
- [pyibmtts](https://sourceforge.net/projects/ibmtts-sdk/) — the original
  IBM SDK we cribbed the API definitions from.
- [Speech Dispatcher docs](https://htmlpreview.github.io/?https://github.com/brailcom/speechd/blob/master/doc/speech-dispatcher.html)
