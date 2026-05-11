# 04 — How the converter works

This document walks through `macho2elf/macho2elf.py` for anyone who wants
to understand or modify it. Skip if you just want to use the tool.

## Why this works at all

The lucky alignment that makes this approach tractable:

| Concern | Apple x86_64 | Linux x86_64 | Match? |
|---|---|---|---|
| ABI (calling conv, stack, red zone) | System V AMD64 | System V AMD64 | ✅ Identical |
| Addressing model | PIC + RIP-relative | PIC + RIP-relative | ✅ Identical |
| C++ ABI | Itanium (mangling, vtable layout, exceptions) | Itanium | ✅ Identical |
| Standard C++ runtime | libc++ (`std::__1`) | libc++ (`std::__1`) | ✅ Identical IF you install libc++ |
| C runtime | Darwin libSystem | glibc | ⚠️ ~95% same names, a few renames |
| File format | Mach-O | ELF | ❌ Different — this is what we translate |

Because the executable code itself uses System V calling conventions and
RIP-relative addressing, the **bytes in `__TEXT,__text` are valid on Linux
x86_64 unchanged**. Same for arm64. We don't need to recompile or
binary-rewrite the code; we just need to put it in an ELF wrapper that
the Linux dynamic loader understands.

The same insight wouldn't work for **Windows x64** — MS ABI uses different
argument registers and has shadow space; the bytes would need real
translation at every API call boundary. Or for **32-bit x86** — Apple
hasn't shipped i386 in this era, and the calling convention differs
significantly between System V i386 and Apple's old i386 ABI.

## The strategy

1. **Parse the Mach-O** with LIEF — extract sections, segments, exports,
   imports, bindings, relocations, chained-fixup chains.

2. **Dump each section as a binary blob** to disk (e.g., `TEXT__text.bin`,
   `DATA__data.bin`).

3. **Emit a single assembly stub** that:
   - `.extern` declares every external symbol the dylib imports (with
     Darwin → Linux renaming applied: `___error` → `__errno_location`,
     etc.).
   - For each section, emits an event-interleaved stream of:
     - `.incbin "blob.bin", from, len` for raw byte chunks
     - `.globl <name>` + `.type <name>, @function` + `<name>:` labels
       at every export's offset within the section
     - `.quad <symbol_name>` patches at every chained-fixup binding offset
       (overrides the original 8 bytes)
     - `.quad <section_label> + <offset>` patches at every rebase site
       (these become `R_X86_64_RELATIVE` after linking)
   - Emits an `.init_array` section containing `.quad` references to each
     Mach-O `__init_offsets` initializer (so ld.so runs C++ static
     constructors at load time).
   - Emits a `.note.GNU-stack` section in the `:gnustack` phdr (so the
     loader doesn't request executable stack).

4. **Emit a linker script** that:
   - Pins each ELF section at the original Mach-O virtual address (so
     all the RIP-relative offsets in the original code work unchanged).
   - Places linker-auto-generated sections (`.dynsym`, `.dynstr`,
     `.rela.dyn`, `.dynamic`, etc.) far above the Mach-O image's address
     range so they don't collide.
   - Uses a separate `auxtext` PT_LOAD segment for the linker-auto
     `.plt`/`.text`/`.rodata` sections so they don't extend the main
     text segment's vaddr range into the data segment.

5. **Compile a tiny stubs.c** providing the few Darwin-specific symbols
   that have no direct Linux equivalent:
   - `_macho2elf_rune_locale_stub` — placeholder for Apple's
     `_DefaultRuneLocale` ctype-table global (engine only uses it via
     macros that we map to glibc's `isalpha`/etc.)
   - `__stack_chk_guard` — glibc keeps the canary in TLS (`fs:0x28`)
     and doesn't export the symbol, so we provide a fixed-value global.
   - `__maskrune` — Darwin's ctype bitmask lookup, stubbed via glibc's
     `isalpha`/`isdigit`/etc.

6. **Run `gcc -shared`** with the linker script to produce the final ELF.

## Section layout translation

Mach-O segments and sections don't map 1:1 to ELF. Some have analogs and
some are special:

| Mach-O | ELF equivalent | Notes |
|---|---|---|
| `__TEXT,__text` | `.text` (we use `.m2e_text`) | Code |
| `__TEXT,__stubs` | `.plt`-equivalent (we keep as `.m2e_stubs`) | Indirect-jump trampolines |
| `__TEXT,__cstring` | `.rodata.str1` | C string constants |
| `__TEXT,__const` | `.rodata` | Read-only data |
| `__TEXT,__gcc_except_tab` | `.gcc_except_table` | C++ exception LSDA (DWARF format, portable) |
| `__TEXT,__unwind_info` | (no ELF equivalent — Apple's compact unwind) | We just `.incbin` it; not used by glibc unwinder |
| `__TEXT,__init_offsets` | `.init_array` (regenerated) | Static constructor pointers |
| `__DATA_CONST,__got` | `.got` (we keep as `.m2e_got`) | External symbol GOT |
| `__DATA_CONST,__const` | `.data.rel.ro` | RW-then-RO data (vtables, typeinfo) |
| `__DATA,__data` | `.data` | RW data |
| `__DATA,__bss` | `.bss` | Zero-initialized |
| `__DATA,__common` | `.bss` | C "common" symbols |
| `__DATA,__got_weak` | `.got` (weak slots) | Weak external imports |
| `__DATA,__const_weak` | `.data.rel.ro` | Weak vtable/typeinfo |

We preserve the original Mach-O virtual address for each section so the
existing RIP-relative offsets in code (and the offsets baked into the
`__stubs` section pointing at `__got` slots) all resolve correctly. The
linker script's `. = vaddr` assignments do this.

## Symbol translation

Mach-O prepends an underscore to every C/C++ symbol. ELF does not. So
`_atoi` in Mach-O becomes `atoi` in ELF — just strip one leading
underscore.

A handful of Darwin libc functions don't exist on Linux with the same
name:

| Mach-O name (stripped) | Linux equivalent | Why |
|---|---|---|
| `__error` | `__errno_location` | Different errno-accessor function name |
| `__tolower` | `tolower` | Darwin has `__tolower(c)`; glibc only has `tolower(c)` |
| `__toupper` | `toupper` | Same |
| `__stderrp` | `stderr` | Darwin's stdio uses `FILE *__stderrp`; glibc uses `FILE *stderr` |
| `__stdoutp` | `stdout` | Same pattern |
| `__stdinp` | `stdin` | Same pattern |
| `_DefaultRuneLocale` | `_macho2elf_rune_locale_stub` (provided in stubs.c) | Darwin ctype table has no glibc equivalent |

C++ ABI symbols (Itanium mangling: `_ZN...`) work identically on both
platforms — Apple uses libc++ with `std::__1` inline namespace, which is
exactly what Linux libc++ provides too. Hence the strict libc++ linkage
on the Linux side (libstdc++ would NOT work — different namespace, different
mangling).

## Chained-fixup handling

Mach-O on iOS 13+ / macOS 11+ uses *chained fixups* instead of the older
`dyld_info` rebase/bind streams. The fixup data is encoded inline in the
data sections: each 8-byte slot in `__got`, `__const`, etc. contains a
bitfield with the target address (for rebases) or import ordinal (for
binds) plus a "next chain entry offset" field.

LIEF abstracts both formats and gives us:
- `binary.bindings` — iterator of binding sites (slot address + symbol
  reference), but only for the standard binding chains
- `binary.relocations` — comprehensive iterator covering ALL fixup sites
  including internal-symbol relocations that aren't in `binary.bindings`,
  each with `.address` (the slot) and `.target` (the resolved address)

Our converter walks both lists:
- For each `binding` (external symbol reference) → emit `.quad <symbol_name>`
  at the slot. The linker treats this as an undefined reference and
  generates an R_X86_64_GLOB_DAT (or _RELATIVE) at load time.
- For each `relocation` not in `bindings` → emit `.quad <section_label>
  + <offset_within_section>` at the slot. This compiles to R_X86_64_64,
  which the linker turns into R_X86_64_RELATIVE for shared library output.
  ld.so applies the load-time slide.

The result is that every chained-fixup slot in the original Mach-O gets
a corresponding ELF dynamic relocation that does the same thing.

## Why arm64 is harder

The strategy works for arm64 too — same ABI alignment, same Mach-O
structure. The converter is arch-aware (`ARCH_CONFIG` dict at top of
`macho2elf.py`) and emits the right `OUTPUT_FORMAT`, page size, and
toolchain commands.

But there are arm64-specific gotchas:
1. **Page alignment**: Apple's arm64 dylibs use 16KB segments
   (`-z max-page-size=0x4000`). x86_64 uses 4KB.
2. **Comment syntax**: arm64 GAS uses `//` for line comments (`#` is for
   immediate values like `mov x0, #4`). x86_64 GAS uses `#`. We use
   block `/* */` comments everywhere to sidestep this.
3. **libc++ availability**: aarch64 cross-toolchain sysroots don't ship
   libc++. We generate empty stub .so files at link time so DT_NEEDED
   gets recorded; ld.so resolves real symbols at load on the target.
4. **`--unresolved-symbols=ignore-all`**: for arm64 we tell the linker
   not to fail on unresolved C++ vtable/typeinfo references — they'll
   resolve at load against the target's libc++.

The arm64 output builds correctly but runtime testing under qemu-user
is unreliable (qemu's emulation of glibc's complex C++ init has known
issues). See `docs/05-troubleshooting.md`.

## Important subtleties

- **C++ static constructors**: Mach-O's `__init_offsets` lists offsets to
  initializer functions. We translate to ELF `.init_array` so ld.so runs
  them BEFORE any dlopen call returns. If we skipped this, every C++
  global with a non-trivial constructor would be in an unconstructed
  state, and the first method call would crash.

- **Stack canary**: glibc's `__stack_chk_guard` is internal-hidden. We
  must provide our own — see `stubs.c`. The value doesn't matter for
  security (we just want entry-canary == exit-canary), but Apple's
  convention is to make the high byte zero so string-overflow attacks
  hit a null terminator. We follow that pattern.

- **PHDRS overlap**: Mach-O packs sections tighter than ELF segments
  allow. Specifically, `__DATA_CONST` and `__DATA` are at adjacent
  virtual addresses with the same R-then-RO permission profile in
  Mach-O, but ELF wants distinct PT_LOAD segments per permission. We
  unify them into one rwdata PT_LOAD (matching ELF's `.data.rel.ro`
  semantics) to avoid segment-overlap errors.

- **`_DefaultRuneLocale` size**: our stub is an 8KB zero buffer. Apple's
  real struct has classification tables for full Unicode characters. The
  ETI Eloquence engine only seems to use the table via inlined `isXXX(c)`
  macros for ASCII chars, which on glibc go through the function-form
  (`isalpha(c)` etc.) instead — our `__maskrune` stub handles that.
  If something exotic uses the table directly on non-ASCII, we'd need a
  real classification table.

## Limitations / what won't work

- Any Mach-O dylib with Apple-framework dependencies (CoreFoundation,
  Foundation, AVFoundation, etc.) — those have no Linux equivalents.
- arm64e (pointer-authenticated) Mach-O — uses extra encoding bits in
  the chained-fixup format that our converter doesn't decode.
- Objective-C-heavy binaries — Apple's runtime is not on Linux.
- Anything using XPC, Mach ports, or Darwin-only syscalls.
- 32-bit Mach-O (i386 or armv7) — extensively tested only on 64-bit.

The ETI Eloquence dylibs happen to fit cleanly within these limits
because the engine was authored in portable C/C++ to ship across many
platforms (Mac, Windows, Linux, embedded RTOSes) — it's explicitly
designed to use only standard libc and libc++.
