# Apple Eloquence — Android TTS engine (experimental scaffold)

A buildable skeleton that exposes the converted Eloquence engine as an Android
`TextToSpeechService`, so it shows up in **Settings → Accessibility →
Text-to-speech** alongside Google's engine.

> **Status:** the native + converter path is validated (all 19 modules link
> against Bionic/NDK-libc++ with zero unresolved symbols; the JNI shim compiles
> clean with NDK r27c). The end-to-end app has **not yet been run on a device or
> emulator** — that's the next milestone. Treat this as a starting point.

## Layout

```
android/
  jni/eloquence_jni.c        Native bridge: dlopen eci.so, run ECI, return PCM
  jni/CMakeLists.txt         Builds libeloquence_jni.so (+ optional CJK atexit fix)
  stage-libs.sh              Convert vendor dylibs -> jniLibs/arm64-v8a/lib*.so
  app/
    build.gradle.kts         AGP/Kotlin/NDK config (arm64-v8a)
    src/main/
      AndroidManifest.xml    Declares the TTS_SERVICE
      java/.../EloquenceNative.kt        JNI declarations
      java/.../EloquenceTtsService.kt    The TextToSpeechService
      res/xml/tts_engine.xml             Engine descriptor
      jniLibs/arm64-v8a/                 Staged engine .so files (gitignored)
```

## How it fits together

1. **Convert + stage.** `stage-libs.sh` runs `macho2elf.py --os android` for each
   vendored dylib and drops the results into `jniLibs/arm64-v8a/` as
   `lib<name>.so`, plus the NDK's `libc++_shared.so`. Gradle installs everything
   under `jniLibs/` into the app's **read-only `nativeLibraryDir`** — the one
   place modern Android lets you `dlopen` executable code (writable app storage
   is blocked by W^X / SELinux).
2. **JNI bridge.** `libeloquence_jni.so` `dlopen`s `libeci.so`, resolves the ECI
   C API, and synthesizes to 11025 Hz mono S16 PCM via an ECI callback.
3. **Service.** `EloquenceTtsService` generates `eci.ini` at runtime (absolute
   `Path=` into `nativeLibraryDir`), maps Android locales to ECI dialects, and
   feeds PCM to the platform `SynthesisCallback`.

## Build

```bash
# 1. Convert + stage the engine libraries (needs the venv with lief + the NDK).
export ANDROID_NDK=/path/to/android-ndk-r27c
PATH="/path/to/venv/bin:$PATH" ./android/stage-libs.sh 24

# 2. Build the APK (Android SDK + the same NDK).
cd android
./gradlew assembleDebug        # or open in Android Studio
```

The NDK version in `app/build.gradle.kts` (`ndkVersion`) must use `std::__ndk1`
libc++ — **r27** does. If you bump the NDK and the libc++ inline namespace
changes, update the `St3__1 -> St6__ndk1` rewrite in `macho2elf.py`
(`rename_import`) accordingly.

## Install & test (emulator or device)

Use an **arm64-v8a** Android image (the engine is arm64-only today):

```bash
adb install android/app/build/outputs/apk/debug/app-debug.apk
# Then: Settings -> Accessibility -> Text-to-speech -> pick "Apple Eloquence",
# or drive it directly:
adb shell am start -a android.intent.action.VIEW   # (your own test activity), or
adb shell settings put secure tts_default_synth com.eloquence.tts
```

A quick way to hear it without a UI is the `TextToSpeech` API from a tiny test
activity, or `adb shell` an app that calls `speak()`.

## Known gaps / next steps

- **Not yet run on-device** — verify load + synthesis, then iterate.
- **arm64 only.** An `android-x86_64` target (for x86_64 emulators) is easy to
  add: x86_64 Android uses the SysV variadic ABI, so none of the arm64 shims are
  needed — just a config entry with the x86_64 NDK clang and 4 KB pages.
- **Streaming.** `nativeSynthesize` buffers the whole utterance; for long text,
  switch to chunked delivery (the ECI callback already arrives in chunks).
- **Voice/prosody mapping** is approximate (`onSynthesizeText`); tune against the
  IBM ECI ranges.
- **CJK** needs `Path_Rom=` (handled) and the `__cxa_atexit` override (bundled
  via `CMakeLists.txt`, which makes `libeloquence_jni.so` GPL-2.0-or-later).
