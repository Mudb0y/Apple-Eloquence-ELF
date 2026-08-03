# 06 — Hybrid builds (`--substitute`)

A hybrid build is one where chosen functions run *your* C while everything
around them is still the original machine code, so the whole engine works and
its output can be compared against the genuine article. It is the mechanism
that makes incremental decompilation possible: convert one function, prove the
audio is unchanged, commit, repeat.

## Usage

```
python3 macho2elf/macho2elf.py slice.x86_64 -o out/eci.so \
    --substitute subs.tsv \
    --extra-object impl.o
```

`subs.tsv` has one function per line:

```
eciAddText	0x2c57	883
```

Name, address, size — tab- or space-separated, `#` comments allowed. Sizes are
required because the converter has no function-boundary information of its own:
Mach-O exports give addresses but not extents. Derive them from
`LC_FUNCTION_STARTS`.

For each listed function `foo`, supply a C function `reloq_foo` with the same
signature, compiled into an object passed via `--extra-object` (repeatable).
`--substitute` without `--extra-object` is refused, since the link would only
fail later with undefined symbols.

The name column is **not** looked up in the dylib: the address places the
trampoline, and the name only forms `reloq_<name>` and the slot label. So the
caller may put any label it likes there, and a caller substituting several
same-named functions must, because otherwise every one of those addresses
resolves to a single `reloq_` definition and all but one silently receives
another's body. Reloquence hit this with four distinct `freeDeltaStackTo` bodies
and three `clearDeltaStackBack` in one module -- the latter genuinely not
interchangeable, each tail-jumping to a different `freeDeltaStackTo` -- and
settled on `<name>_<address in hex>` for any name duplicated in the module (its
`docs/decisions.md` amendment 12.27). Nothing here had to change for that, which
is why it is worth recording: the freedom is deliberate, so do not "fix" this
column by resolving it against the symbol table.

## How it works, and why not the obvious way

The obvious approach — omit the function's bytes and let the linker point calls
at a replacement — cannot work. Call sites are baked into the original
instruction stream as fixed displacements, and every RIP-relative reference in
the image assumes nothing moves. Layout must be preserved exactly.

So the function is overwritten **in place** with a 6-byte indirect jump, and the
remaining bytes are filled with `0xCC` so any accidental fallthrough traps
loudly rather than executing leftovers:

```
ff 25 <rel32>     jmp *slot(%rip)
cc cc cc ...      int3 padding
```

The jump is indirect through an 8-byte slot in a `.reloq_slots` section holding
the replacement's address, rather than a direct 5-byte `jmp rel32`, so the
replacement can live wherever the linker puts it without range concerns.

The original symbol keeps its address and its name, so baked call sites,
function pointers and vtable entries all reach the replacement unchanged.

## Naming what the dylib keeps private: `--expose`

A replacement usually needs to reference something the dylib does not export. A
ported C++ constructor must store the same vtable pointer the original stored,
but vtables are local symbols — in `eci.dylib` only `_ZTI10ETImessage` is
exported, not `_ZTV10ETImessage`. 98 of that module's 839 functions are also
unexported. Without a way to name an internal address, faithfully porting
anything that touches internal data is impossible.

    --expose exposures.tsv

with one `symbol<TAB>address[<TAB>type]` per line; `type` defaults to `@object`,
use `@function` for code. This emits a global symbol at that address and changes
nothing else — a symbol is metadata, the bytes are untouched.

The replacement then declares it `extern` and uses it. For the case above,
exposing the vtable object at its address lets the constructor store
`address + 0x10`, matching the Itanium convention of pointing past
offset-to-top and typeinfo.

## Constraints

- A function must be at least 6 bytes. Shorter ones are refused rather than
  silently corrupted by a trampoline overrunning into the next function. (No
  function in the Eloquence modules is smaller than 6 bytes — checked across all
  3,144 in the English speech path.)
- Addresses must be from the same slice being converted. An address from the
  other architecture is caught and reported.
- x86_64 only for now. arm64 needs a different trampoline encoding (an
  `ADRP`/`LDR`/`BR` sequence, 12 bytes, so the minimum function size rises).

## Verification

Validated with three controls against a 67-artefact fidelity corpus:

- **No substitution.** A freshly converted `eci.so` is bit-identical to the
  installed reference. This is the prerequisite: without it, nothing else is
  attributable.
- **Substituting an unused function.** Replacing `eciVersion`, which the corpus
  never calls, leaves every artefact byte-identical — so the trampoline does not
  disturb layout, relocations or surrounding code.
- **Substituting a used function.** Replacing `eciAddText` with a no-op
  diverges 65 of 67 artefacts and produces zero samples on every item — so the
  trampoline is genuinely reached. The two artefacts that still match belong to
  the empty-input item, which was silent in the reference too.

A positive control alone would not be enough: a substitution mechanism that
silently did nothing would also pass it.
