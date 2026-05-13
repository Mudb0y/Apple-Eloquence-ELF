# ECI 2-API reverse-engineering notes

For each `2`-suffixed function `sd_eloquence` wraps, a per-function doc here
records the inferred signature plus the disassembly evidence (Apple's call site
+ eci.so's function prologue) it's based on.

## Sources

- **Apple's framework:** `TextToSpeechKonaSupport`, a Mach-O 64-bit x86-64
  dylib shipped inside `TextToSpeechKonaSupport.framework`. Location on this
  host:
  `/home/stas/embedded-viavoice/Apple/extracted_18.2_kona/Library/Developer/CoreSimulator/Profiles/Runtimes/tvOS 18.2.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/TextToSpeechKonaSupport.framework/TextToSpeechKonaSupport`

- **ECI engine:** `/usr/lib/eloquence/eci.so` — the installed copy of the
  converted `vendor/tvOS-18.2/eci.dylib`.

## Workflow per function

### 1. Map Apple's import table

The framework uses `LC_DYLD_CHAINED_FIXUPS` (not the traditional indirect
symbol table). Parse with Python:

```python
import struct
with open(APPLE_FW, 'rb') as f:
    data = f.read()
# ... (see parse script in eciNewEx2.md notes)
```

Or check which symbols the framework imports:

```bash
llvm-nm "$APPLE_FW" | grep '^[[:space:]]*U' | sort
```

Note: the framework may call `_eciNew2` directly rather than the `eciNewExN`
wrapper for a given function.

### 2. Find the stub address for the target function

The `__stubs` section starts at 0x9c02 (6 bytes per stub). Each stub does
`jmp qword ptr [rip + offset]` pointing to a `__got` slot. The `__got` slots
are populated from the chained fixup import table in order.

Quick formula: if `_funcname` is import[N], its GOT slot is at
`0xe000 + N * 8` (specific to this binary — verify from section headers).
Its stub is the entry whose `jmp` target equals that GOT address.

### 3. Find every call site of the target function

```bash
APPLE_FW=".../TextToSpeechKonaSupport"
# Disassemble and search for call to the stub address
llvm-objdump -d --x86-asm-syntax=intel --no-show-raw-insn "$APPLE_FW" \
  | grep -B 8 '<stub_addr>'
```

The 8 instructions before each `call` show how arg registers are set up per
the System V AMD64 ABI:
- Integer/pointer args: `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`
- Float args: `xmm0` .. `xmm7`
- Return value: `rax` (integer/pointer) or `xmm0` (float)

32-bit register slice (`edi`, `esi`, ...) means int32 (e.g. an enum). 64-bit
(`rdi`, `rsi`, ...) means pointer or int64.

### 4. Read the function prologue in eci.so

```bash
llvm-objdump -d --x86-asm-syntax=intel --no-show-raw-insn /usr/lib/eloquence/eci.so \
  | awk '/<funcname>:/,/^[[:xdigit:]]+ <eci/' | head -60
```

Key signals:
- `mov r14d, edi` — function saves first arg as 32-bit: it's an `int`/`enum`
- `mov r14, rdi` — function saves first arg as 64-bit: it's a pointer
- Immediate overwrite of `rdi` (e.g. `lea rdi, [rip + ...]` in first few
  instructions before any save) — the first argument register is **not** used;
  function takes fewer args than the register suggests

### 5. Cross-reference against the legacy API

Compare `funcname2` prologue with `funcname` (no "2") prologue:
- Same arg saves in the same registers → same signature, different implementation
- Missing arg save in "2" version → "2" version takes fewer args or ignores
  the argument

### 6. Write the per-function doc

Use `eciNewEx2.md` as the template. Include:
- Inferred C signature with explanation of any divergence from the legacy version
- The Apple framework call site (8 instructions before the `call`)
- The eci.so function prologue (first 20-30 lines)
- The contrast with the legacy (`funcname` without "2") if applicable
- Summary of register evidence

## Important: eciNewEx2 divergence from spec hypothesis

The initial hypothesis was `ECIHand eciNewEx2(enum ECILanguageDialect dialect)`,
matching `eciNewEx`. The disassembly shows `eciNewEx2` takes **zero** arguments:
`edi` is immediately overwritten in the prologue without being saved. The
dialect is hardcoded to 0 inside the function. Apple's framework calls
`_eciNew2` directly (not `_eciNewEx2`) and passes the dialect explicitly.

This pattern should be verified for each subsequent function before assuming
the "2" version has the same signature as its legacy counterpart.

## Critical-path functions for the migration

(See `docs/superpowers/specs/2026-05-13-eci-2-api-switch-design.md` §5.3 for
the full list. As of 2026-05-13 this work is deferred to v2; see
`STATUS.md`.)
