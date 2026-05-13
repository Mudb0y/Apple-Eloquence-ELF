# macho2elf relocation audit — design

**Status:** approved 2026-05-13
**Owner:** Stas Przecinek
**Scope:** Audit every Mach-O relocation/fixup kind that appears in `vendor/tvOS-18.2/*.dylib`, fix the macho2elf converter's handling of each, re-convert all 18 modules, and remove the runtime workarounds in `sd_eloquence/` that exist only because of converter bugs.

---

## 1. Motivation

CJK Phase 0-2 turned up two distinct converter-side bugs in the chsrom/chtrom/jpnrom Mach-O→ELF translation:

1. **`__cxa_atexit` destructor pointers** are passed values that are unslid Mach-O VAs or pointers into freed-heap regions. libc dereferences them at process exit → SIGSEGV. We currently work around this with `sd_eloquence/src/cjk_atexit_override.c` (an in-module `__cxa_atexit` override that suppresses registrations originating from the romanizer .so's).

2. **Function-pointer-table values in chsrom's data sections** resolve at runtime to `.m2e_text_start + 0` (the section base) instead of `.m2e_text_start + <intended function offset>`. Apple's eci.so dereferences this during synthesis (`reset_sent_vars()` called from `process_input()` called from the SynthThread's `addTextToEngine()`), jumps into the section header, and SIGSEGVs. No module-side workaround possible — the dereference is inside closed-source engine code.

Both bugs share a likely root cause: certain Mach-O relocation/fixup kinds aren't being translated by `macho2elf/macho2elf.py` with the correct target offset. The pointer value emitted to ELF is missing the offset within the target section.

Policy decision: converter bugs belong in the converter. Workarounds in `sd_eloquence/` are paid technical debt. This project closes that debt at the source.

## 2. Scope

**In scope:**

- Catalog every distinct relocation/fixup kind that appears in `vendor/tvOS-18.2/*.dylib`
- Cross-reference LIEF's output against a hand-rolled chained-fixup walker + `llvm-otool` ground truth
- For each kind, document how `macho2elf.py` currently handles it and whether that handling is correct
- Fix every gap; re-convert all 18 modules
- Validate all 14 working dialects via isolated synth probe + speech-dispatcher smoke + Orca manual gate
- Remove `sd_eloquence/src/cjk_atexit_override.c` and re-ungate CJK in `module_init`

**Out of scope:**

- Per-function probe harness for engine internals (Approach C from brainstorming — too expensive)
- Switching to a custom LIEF replacement (we use LIEF where it works, our own parser only for chained fixups)
- Filing LIEF bugs upstream (we may, but it's not a deliverable here)
- The CJK-language anti-crash regex filters in `sd_eloquence/src/filters/` — those are unrelated to converter bugs
- Generally improving macho2elf beyond what the audit identifies

## 3. Success criteria

After the audit + fix + re-conversion + cleanup:

1. `examples/cjk_probe` (extended to all 14 dialects) produces ≥ 5000 PCM samples AND exits with code 0 for every dialect.
2. `dist/smoke.sh` (extended with CJK lines) survives the full sequence; the module process keeps the same PID throughout.
3. Orca manual smoke: switching between all 14 voices, scrolling rapidly through Settings, scrolling lines in a terminal — no skipped utterances, no module restarts.
4. `sd_eloquence/src/cjk_atexit_override.c` is deleted; CJK is `LANG_AVAILABLE` again; `-rdynamic` is dropped from CMake; 6/6 unit tests still pass.
5. `docs/macho2elf-audit/relocation-catalog.md` exists, every relocation kind has a `✅` row, and every `diff-converter.txt` is empty.

## 4. High-level architecture

Four artifacts and one cleanup:

1. **Per-module audit dumps** in `docs/macho2elf-audit/<filename>/` (4 files per dylib × 18 dylibs)
2. **Audit tool** `tools/audit_relocs.py` (≈ 400 LOC) — combines LIEF, `llvm-otool`, and our hand-rolled chained-fixup walker
3. **Patched `macho2elf/macho2elf.py`** with focused commits per relocation kind
4. **`sd_eloquence/` cleanup** — drop the workaround + re-ungate CJK

Workflow:

```
                  ┌─────────────────────┐
  vendor/*.dylib  │  audit_relocs.py    │   docs/macho2elf-audit/
        ─────────▶│  (our parser +      │──▶ <filename>/
                  │   LIEF + otool +    │      {ground-truth, lief,
                  │   converter dump)   │       converter, diff-lief,
                  │                     │       diff-converter, summary}
                  └─────────────────────┘
                            │
                            ▼
                  ┌─────────────────────┐
                  │ catalog assembly    │   docs/macho2elf-audit/
                  │ (manual: spec doc)  │──▶ relocation-catalog.md
                  └─────────────────────┘
                            │
                            ▼
                  ┌─────────────────────┐
                  │  fix macho2elf.py   │   macho2elf/macho2elf.py
                  │  (one commit or     │   (patched)
                  │   small series)     │
                  └─────────────────────┘
                            │
                            ▼
                  ┌─────────────────────┐
                  │  re-convert all 18  │   prebuilt/{x86_64,aarch64}/*.so
                  │  modules            │   (re-generated)
                  └─────────────────────┘
                            │
                            ▼
                  ┌─────────────────────┐
                  │  validate           │   examples/cjk_probe ×14
                  │  (probe + Orca      │   dist/smoke.sh
                  │   smoke)            │   Orca manual smoke
                  └─────────────────────┘
                            │
                            ▼
                  ┌─────────────────────┐
                  │  cleanup sd_eloq:   │   remove cjk_atexit_override.c
                  │  drop workaround    │   re-ungate CJK in module.c
                  └─────────────────────┘
```

License: all converter-side changes are MIT under `macho2elf/` and `tools/`. The `sd_eloquence/` cleanup is GPLv2 (unchanged license scope).

## 5. Audit data collection

For each `.dylib` in `vendor/tvOS-18.2/` (18 files), produce four artifacts in `docs/macho2elf-audit/<filename>/`:

### 5.1 `ground-truth.txt`

Combined output from Apple's tools and our hand-rolled parser:

```bash
llvm-otool -lv <dylib>                        # load commands, segments, sections
llvm-otool -j <dylib>                         # full disassembly w/ rip-relative resolved
llvm-otool -rv <dylib>                        # classic external relocations table
python3 tools/dump_chained_fixups.py <dylib>  # NEW (~150 LOC)
```

`dump_chained_fixups.py` walks the `LC_DYLD_CHAINED_FIXUPS` load command's chains directly. Per-fixup output:

```
# fmt: site (segment + page + offset)  kind  raw_value  decoded_target
[__DATA_CONST,__got @ 0x00b8000 + 0x008]  bind     0x80000007  -> __cxa_atexit  (libSystem.dylib)
[__DATA_CONST,__got @ 0x00b8000 + 0x010]  rebase   0x00069aa0  -> .__text + 0x68a8 (reset_sent_vars)
[__DATA,__data    @ 0x00b9000 + 0x100]    rebase   0x000b3e70  -> .__const + 0x000  (vtable_for_Lexicon)
```

### 5.2 `lief.txt`

What LIEF reports for the same dylib via `binary.relocations`, `binary.bindings`, `binary.dyld_chained_fixups`. Same per-record format as `ground-truth.txt`.

### 5.3 `converter.txt`

What `macho2elf.py` *would emit* for each relocation site in this dylib. Same per-record format as the previous two. Produced by importing macho2elf's relocation-emission code as a module and capturing its output without invoking `gcc -shared` at the end — same data the assembly stub gets, just textualized.

### 5.4 `diff-lief.txt` and `diff-converter.txt`

Two diffs:

- `diff-lief.txt` = `diff -u ground-truth.txt lief.txt` — measures LIEF accuracy against Apple's tools + our walker. Any non-empty diff is a LIEF gap; we either route around it in the converter (read load commands directly) or file upstream.
- `diff-converter.txt` = `diff -u ground-truth.txt converter.txt` — measures the converter's accuracy against ground truth. **This is what shrinks as we fix bugs.** When all 18 `diff-converter.txt` files are empty, the converter handles every relocation kind correctly.

### 5.5 `summary.json`

Counts per relocation kind, suitable as machine-readable input to catalog assembly:

```json
{
  "module": "chs.dylib",
  "kinds": {
    "chained_rebase_target_in_text": 1234,
    "chained_rebase_target_in_text_with_nonzero_offset": 18,
    "chained_bind_libsystem": 56,
    "classic_external_cxa_atexit": 1,
    "...": "..."
  },
  "lief_diffs": [
    { "site": "...", "ground_truth": "...", "lief": "..." }
  ]
}
```

### 5.6 `audit_relocs.py`

Wraps the above into one CLI:

```bash
python3 tools/audit_relocs.py vendor/tvOS-18.2/chs.dylib \
        --workdir docs/macho2elf-audit/chs.dylib
```

Run across all 18 modules via `tools/reconvert_all.sh` (which also drives the re-conversion step in §7).

## 6. Catalog assembly + fix workflow

`docs/macho2elf-audit/relocation-catalog.md` is a single document listing every distinct relocation/fixup kind seen across the 18 modules. Schema:

| Kind | Where it appears | Source format | Current handling | Correct handling | Status |
|------|------------------|---------------|------------------|------------------|--------|
| chained rebase, target in `__TEXT,__text` (any offset) | every dylib | 32-bit page-relative offset in chained-fixup chain | `binary.relocations` loop, `tgt = r.target`, emitted as `.quad .m2e_text_start + tgt_off` | Same — verified correct | ✅ |
| chained rebase, target in `__TEXT,__text` with non-zero function offset | chs, cht, jpn (the smoking gun) | same | LIEF reports `r.target = .__text + 0` instead of `.__text + <intended offset>` — converter emits `.quad .m2e_text_start + 0` | Parse the chained-fixup record directly to get the real target offset | ❌ Bug A |
| classic external reloc, `__cxa_atexit` callsite | C++ static-dtor TUs in chs/cht/jpn | classic relocation table (`LC_DYSYMTAB`) | Bindings loop matches by symbol name; emits a PLT reference | (illustrative — actual handling status filled in once audit_relocs.py runs against the dylibs) | ❌ Bug B (presumed) |
| (rows above are illustrative; full catalog assembled from `summary.json` files in §5.4) | | | | | |

The catalog is generated semi-manually: `audit_relocs.py` produces `summary.json` listing kind counts; we cross-walk those into the catalog by hand, grouping equivalent kinds. The manual review is the value — it's what guarantees every kind has been considered.

### 6.1 Fix workflow

1. From `❌ Bug` rows, identify underlying root causes. Common patterns we expect:
   - LIEF abstraction lossiness (LIEF reports a target without the right offset)
   - `macho2elf.py`'s `r.target` interpretation incorrect for some kinds
   - `macho2elf.py` dropping entire kinds without emitting a rebase

2. For each bug, write a focused commit to `macho2elf.py`. Each commit names the kind, fixes or adds a handler, references the catalog row anchor.

3. After each commit, re-run `audit_relocs.py` against affected modules. `diff-converter.txt` shrinks (this is the "converter vs ground truth" diff from §5.4; the `diff-lief.txt` is separate and may stay non-empty if LIEF has gaps we work around). When all 18 `diff-converter.txt` files are empty, the catalog is fully closed.

4. Re-convert every `.so` from `vendor/tvOS-18.2/`. Spot-check Latin-module bytes against pre-fix counterparts to confirm no regressions (only the bytes called out by Bug rows should change).

### 6.2 Heuristics for the chs.dylib smoking gun

Crash trace: thread in `reset_sent_vars` (at `chs.so + 0x69aa`) called something at `chs.so + 0xfb0` (literally `.m2e_text_start`). Disassemble `reset_sent_vars`:

```bash
llvm-otool -j vendor/tvOS-18.2/chs.dylib | \
  awk '/_reset_sent_vars/,/^_[a-z]/' | \
  grep -E 'callq|jmpq|leaq'
```

Each indirect call/jump's operand source is the bug site. Typically `[rip + N]` loading from a function-pointer table in `__DATA_CONST,__data_const`. That table slot has the correct value in Mach-O; the audit identifies what relocation entry covers it and why the converter loses the offset.

## 7. Re-conversion + validation

### 7.1 Re-conversion

After `macho2elf.py` is patched, re-run every module under `vendor/tvOS-18.2/` through the converter via `tools/reconvert_all.sh`:

```bash
for arch in x86_64 aarch64; do
  for dylib in vendor/tvOS-18.2/*.dylib; do
    out=prebuilt/$arch/$(basename "$dylib" .dylib).so
    llvm-lipo -extract $arch "$dylib" -output /tmp/slice
    ./venv/bin/python3 macho2elf/macho2elf.py /tmp/slice -o "$out"
  done
done
```

Same pattern the existing CI workflow (`.github/workflows/release.yml`) uses; the wrapper script just codifies the loop for local dev.

### 7.2 Four validation tiers

**Tier 1 — bytes diff.** Diff each freshly converted `.so` against its pre-fix counterpart. A successful fix should modify only the bytes covered by Bug rows; Latin-module bytes nearly identical (a few new rebases at most). Catches accidental breakage of working modules.

**Tier 2 — isolated synth probe per module.** Extend `examples/cjk_probe.c` into `examples/probe_module.c`: takes eci.so path + dialect + probe phrase; `eciNewEx` + callback + `eciAddText` + `Synchronize`; verifies PCM was produced (count > 5000) and exit code 0. Run for all 14 working dialects:

```
0x00010000 0x00010001                       # en-US, en-GB
0x00020000 0x00020001                       # es-ES, es-MX
0x00030000 0x00030001                       # fr-FR, fr-CA
0x00040000                                  # de-DE
0x00050000                                  # it-IT
0x00060000 0x00060001                       # zh-CN, zh-TW
0x00070000                                  # pt-BR
0x00080000                                  # ja-JP
0x00090000                                  # fi-FI
0x000A0000                                  # ko-KR
```

Catches both the runtime crash class (chs.so/cht.so/jpn.so exiting with 139) and the `__cxa_atexit` destructor class (exit 139 even after audio).

**Tier 3 — speech-dispatcher smoke.** Run `dist/smoke.sh` extended with CJK lines from `cjk_probe.c`. The module must survive the full sequence with the same PID throughout.

**Tier 4 — Orca manual.** Owner runs Orca with each language selected, scrolls through Settings, types in a terminal, switches voices rapidly. No crashes, no skipped utterances, clean audio.

Tiers 1–3 are scriptable. Tier 4 is human.

### 7.3 Failure handling

If a freshly converted `.so` regresses (e.g., en-US starts producing weird audio), tier 1 catches it immediately. Bisect: which Bug row's fix touched those bytes? Revisit that handler.

## 8. sd_eloquence cleanup

Once the converter is fixed and tiers 1–3 pass for all 18 modules, the sd_eloquence-side workarounds become redundant. One commit on `feat/sd-eloquence-rewrite`:

1. **Remove `sd_eloquence/src/cjk_atexit_override.c`.** Drop from `CMakeLists.txt`'s sd_eloquence source list. Drop `-rdynamic` from `target_link_options(sd_eloquence …)` and from the parallel `cjk_probe` block.

2. **Re-ungate CJK in `module_init`.** Replace the language-state loop with the unconditional `LANG_AVAILABLE` form from before the §6 re-gate.

3. **Retire `tools/cjk_atexit_shim.c`.** Recommendation: delete. Stays as git history if needed for debugging.

Validation after cleanup: rerun all four tiers from §7. Tier 2 (isolated synth probe) is the critical one — must pass without the override compiled in.

Commit message template:

```
sd_eloquence: drop CJK workarounds now that macho2elf handles their
relocations correctly

The in-module __cxa_atexit override and the CJK LANG_DISABLED gate
were both reactions to converter bugs in chsrom/chtrom/jpnrom. Both
are fixed at the converter level in <commit SHAs>; the workarounds
are now redundant.
```

## 9. File structure, deps, error handling, testing

### 9.1 New files / directories

```
docs/macho2elf-audit/
├── relocation-catalog.md              # written manually from the dumps
├── chs.dylib/
│   ├── ground-truth.txt
│   ├── lief.txt
│   ├── converter.txt
│   ├── diff-lief.txt
│   ├── diff-converter.txt
│   └── summary.json
├── cht.dylib/    (same 6)
├── enu.dylib/    (same 6)
└── ... (18 modules total)

tools/
├── audit_relocs.py                    # ~400 LOC; CLI wrapping LIEF + otool + our parser
├── dump_chained_fixups.py             # ~150 LOC; raw chained-fixup walker
├── reconvert_all.sh                   # driver for vendor/ → prebuilt/
└── tests/
    ├── test_chained_fixups.py         # unit test for dump_chained_fixups
    └── audit-enu.expected.txt         # self-check baseline for audit_relocs
```

### 9.2 Modified files

```
macho2elf/macho2elf.py                  # patched in focused commits
sd_eloquence/src/module.c               # CJK re-ungated
sd_eloquence/src/cjk_atexit_override.c  # DELETED
CMakeLists.txt                          # drop override + -rdynamic
.github/workflows/release.yml           # may need a 'reconvert' step in CI
```

### 9.3 Dependencies

- `python3-lief` — already used; no version bump expected
- `llvm` for `llvm-otool` / `llvm-objdump` — already documented dep
- No new Python packages

### 9.4 Error handling

- `audit_relocs.py`: tolerates partial input; logs and continues; exit 0 on success, 1 on unrecoverable error
- `dump_chained_fixups.py`: strict on a documented binary format; nonzero exit on failure with hex dump of failure site
- Patched `macho2elf.py`: keeps existing `rebases_emitted`/`rebases_dropped` counters and `WARNING` comments. Bar: every kind documented as `❌ Bug` in the catalog must be handled.

### 9.5 Testing

- `dump_chained_fixups.py`: unit test (`tools/tests/test_chained_fixups.py`) with a hand-constructed minimal Mach-O fragment containing one known chain
- `audit_relocs.py`: `--self-check` mode that runs against `vendor/tvOS-18.2/enu.dylib` (a known-good Latin module) and verifies output is byte-identical to `tools/tests/audit-enu.expected.txt`
- `macho2elf.py` itself: no new unit tests (out of scope). Validation is via output behavior (tiers 1–4)

### 9.6 CI hookup

- Tier 1 (converter accuracy): re-run audit_relocs.py against every dylib. `diff-converter.txt` must be empty for every module. Add as a new CI job. `diff-lief.txt` is informational (LIEF-vs-truth) and may be non-empty as long as the converter still emits correct output via its own parser fallback.
- Tier 2 (isolated synth probe): needs eci.so at runtime, which CI doesn't have. Stays manual.
- Tiers 3–4: manual / release-time.

## 10. Open risks

- **LIEF gaps that don't show up in our test dylibs.** The audit only covers what's in `vendor/tvOS-18.2/`. If Apple ships a different build later with a relocation kind we never saw, the converter could break silently. Mitigation: catalog format makes it easy to extend.
- **Re-conversion regresses Latin modules.** Tier 1 (bytes diff) is meant to catch this immediately. If it does happen, bisect within the fix series.
- **The smoking-gun bug is harder than a relocation-handler fix.** If chsrom's bad pointer turns out to be from a Mach-O fixup kind LIEF doesn't expose at all (e.g., something exotic in `LC_DYLD_CHAINED_FIXUPS`), the fix could require a more invasive parser change. Mitigation: §5.1's hand-rolled walker is exactly the escape hatch.
- **Converter changes break aarch64 specifically.** The current converter mostly assumes x86_64; aarch64 may have additional fixup kinds. Tier 1 should compare both arch outputs.

## 11. Implementation order (preview)

The writing-plans phase will produce a detailed multi-step plan. High-level ordering:

1. Build `dump_chained_fixups.py` + its unit test
2. Build `audit_relocs.py` driver + `--self-check`
3. Run audit against all 18 modules; commit the `docs/macho2elf-audit/` artifacts
4. Assemble `relocation-catalog.md`
5. Land focused fix commits in `macho2elf.py`, one per `❌ Bug` row
6. Re-convert all modules via `tools/reconvert_all.sh`
7. Extend `cjk_probe` to `probe_module` covering 14 dialects; validate tier 2
8. Extend `dist/smoke.sh`; validate tier 3
9. Manual Orca validation (owner)
10. `sd_eloquence/` cleanup commit
11. Final review + PR merge
