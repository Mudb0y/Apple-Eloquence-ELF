# CJK Phase 3 — Apple framework API divergence

Investigation pursuing the user's suggestion to look at the original
`TextToSpeechKonaSupport.framework` directory. Turned up a finding that
materially changes the picture from the spec's "macho2elf converter bug"
hypothesis.

## Source

`/home/stas/embedded-viavoice/Apple/extracted_18.2_kona/Library/Developer/CoreSimulator/Profiles/Runtimes/tvOS 18.2.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/TextToSpeechKonaSupport.framework/`

Contents inventory:

- `TextToSpeechKonaSupport` — the framework's main binary (134K). Hosts `AXKonaSpeechEngine` and related ObjC classes that drive eci.dylib.
- `PlugIns/KonaSynthesizer.appex/KonaSynthesizer` — the AVSpeechSynthesisProvider plugin entry-point (60K). Exposes the engine to AVFoundation / VoiceOver.
- `Frameworks/{enu,eng,esp,esm,fra,frc,deu,ita,chs,cht,jpn,kor,fin,ptb,chsrom,chtrom,jpnrom,korrom}.dylib` — the 18 language modules we already vendor.
- `Frameworks/eci.dylib` — the engine core we already vendor.
- `Dictionaries/{en-GB,en-US}/{system,community}/{root,main,abbrv}.kdict` — dictionaries in Apple's `.kdict` binary format (NOT the IBM `.dic` format sd_eloquence expects). NO CJK dictionaries shipped here.
- `KonaVoicePresets.plist` — the 8-voice preset table (Reed/Shelley/Sandy/...) we already transcribed.
- `KonaEncodingReplacements.plist` — binary plist; per-language encoding replacement map. We don't currently use this.
- `chtvoice.sapi.txt` — sample SAPI-tagged Traditional Chinese script. Not load-bearing; just a test/demo file.
- `Info.plist`, `_CodeSignature/` — bundle metadata.

## API divergence (the main finding)

Apple's framework uses the **`2`-suffixed modern ECI API**, NOT the
legacy IBM-compatible API that sd_eloquence currently uses.

Cross-referencing `strings TextToSpeechKonaSupport | grep eci...` against
the symbols `nm -D /usr/lib/eloquence/eci.so | grep ' T eci'` provides:

| What we use (legacy) | What Apple uses (modern `2`) |
|---|---|
| `eciNewEx` | `eciNew2` |
| `eciAddText` | `eciAddText2` |
| `eciDelete` | `eciDelete2` |
| `eciSetParam` | `eciSetParam2` |
| `eciRegisterCallback` | `eciRegisterCallback2` |
| `eciSetOutputBuffer` | `eciRegisterSampleBuffer2` (takes additional format arg) |
| `eciSynchronize` | `eciSynchronize2` |
| `eciCopyVoice` | `eciSetStandardVoice2` |
| `eciInsertIndex` | `eciInsertIndex2` |
| `eciSynthesize` | `eciSynthesize2` |
| `eciNewDict` / `eciLoadDict` / `eciDeleteDict` / `eciSetDict` | `eciNewDict2` / `eciLoadDictVolume2` / `eciDeleteDict2` / `eciActivateDict2` |
| (no equivalent — disk `eci.ini`) | `eciSetIniContent` (in-memory eci.ini as a string) |
| (no equivalent) | `eciRegisterKlattHooks2` (synthesis callbacks for Klatt parameters) |
| (no equivalent) | `eciNewAudioFormat2` / `eciDeleteAudioFormat2` |

Both API surfaces are exported from `eci.so` (the engine binary contains
both). They appear to be **different code paths internally** that do
different amounts of state initialization on the language module.

## Why this matters for CJK

The user-reported runtime crash (`reset_sent_vars` in chs.so, on first
AddText after a switch to a Chinese dialect) is a symptom of
**uninitialized internal state** in the language module. The chs.so /
cht.so / jpn.so / kor.so language modules were built against the
**modern `2` API** that Apple's framework uses. The `eciNewEx2` /
`eciAddText2` codepath presumably initializes that internal state.
`eciNewEx` (legacy, what we call) does NOT — leaving CJK modules in a
half-initialized state where the first AddText dereferences a null/stale
function-pointer table.

This means the full macho2elf relocation audit was correct **in its own
terms** — every relocation in vendor/tvOS-18.2/*.dylib gets correctly
translated to ELF by the converter. The runtime crash isn't a converter
bug. It's an **API-usage bug in sd_eloquence**.

## What we'd need to change

To use the `2` API, sd_eloquence would need:

1. Public signatures for the `2`-suffixed functions. Not in the IBM SDK
   header we vendor. They'd have to be inferred by:
   - Disassembling Apple's framework call sites (we have all 18 Mach-O
     binaries to disassemble).
   - Possibly cross-referencing with leaked or older Apple SDK headers
     that included these names.

2. A rewrite of `sd_eloquence/src/eci/runtime.h`'s `EciApi` struct to
   load the `2` variants via `dlsym`.

3. A rewrite of `sd_eloquence/src/eci/engine.c` to call the new
   functions. Notable: `eciRegisterSampleBuffer2` takes a format
   parameter (not just a size), so the audio buffer registration code
   needs to construct an `ECIsampleFormat` struct. We'd need to RE that
   struct's layout.

4. Probably: implement an `eciSetIniContent` content builder that
   replaces our disk-based eci.ini. Or keep disk eci.ini for now and
   only adopt `eciSetIniContent` if needed for CJK.

5. `eciNewAudioFormat2` / `eciDeleteAudioFormat2` for managing the
   format struct lifetime.

This is substantively more work than fixing a relocation handler — it's
re-doing the engine wrapper layer in eci/engine.c. But it's the
**correct** fix.

## What macho2elf-audit's findings are good for anyway

Even though the converter wasn't the source of the CJK crash, the audit
is genuinely valuable:

1. It built confidence that the converter handles every relocation kind
   in vendor/tvOS-18.2/ correctly. Latin-script modules don't have
   latent bugs hiding.

2. The chained-fixup walker is durable infrastructure. If a future Apple
   build ships with a relocation kind LIEF doesn't surface, the audit
   will catch it.

3. `audit_relocs.py`'s baseline lets CI catch future regressions in
   macho2elf.py.

The `relocation-catalog.md` deliverable from the audit plan (Phase C) is
still worth assembling — it's the durable documentation. But the fix
loop in Phase D doesn't apply because there are no Bug rows.

## Outcome

As of 2026-05-13, this finding deferred CJK support to v2. v1 ships
Latin-only; the wholesale 2-API switch is parked in
`docs/eci-2-api/` until/unless v2 work resumes.
