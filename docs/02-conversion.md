# 02 — Converting Mach-O dylibs to Linux ELF .so files

The conversion is one command per dylib per architecture, after you have
single-arch slices.

## Prerequisites

System packages (Arch examples; equivalents exist on Debian/Ubuntu/Fedora):
```
sudo pacman -S llvm                              # llvm-lipo, llvm-otool, llvm-nm
sudo pacman -S libc++ libc++abi                  # link target for x86_64 builds
sudo pacman -S aarch64-linux-gnu-binutils \
               aarch64-linux-gnu-gcc             # cross-toolchain (only needed for arm64 builds)
```

Python:
```
python3 -m venv venv
./venv/bin/pip install lief
```

LIEF version 0.17+ is required (chained-fixup parsing). The repo's tests run
against LIEF 0.17.6.

## Step 1: extract per-arch slices

Apple ships universal binaries with both x86_64 and arm64 in one file.
`macho2elf.py` expects a single-arch slice.

```
mkdir -p slices
for d in vendor/tvOS-16.4/*.dylib; do
    name=$(basename "$d")
    llvm-lipo -extract x86_64 "$d" -output "slices/${name}.x86_64"
    llvm-lipo -extract arm64  "$d" -output "slices/${name}.arm64"
done
```

## Step 2: run the converter

```
mkdir -p out/x86_64 out/aarch64

# x86_64
for s in slices/*.x86_64; do
    base=$(basename "$s" .dylib.x86_64)
    ./venv/bin/python3 macho2elf/macho2elf.py "$s" -o "out/x86_64/${base}.so"
done

# arm64 (cross-compile)
for s in slices/*.arm64; do
    base=$(basename "$s" .dylib.arm64)
    ./venv/bin/python3 macho2elf/macho2elf.py "$s" -o "out/aarch64/${base}.so"
done
```

Output of one successful conversion:
```
[macho2elf] input:    .../eci.dylib.x86_64
[macho2elf] output:   .../eci.so
[macho2elf] arch:     x86_64
[macho2elf] extracted 14 non-empty sections
[macho2elf] exports: 719
[macho2elf] imports: 147, total binding sites: 215
    __DATA_CONST,__got: 76 bindings
    __DATA_CONST,__const: 10 bindings
    __DATA,__got_weak: 7 bindings
    __DATA,__const_weak: 122 bindings
[macho2elf] assembly: .../stub.s (719 exports emitted, 215 skipped)
[macho2elf] linker script: .../link.lds
[macho2elf] runtime stubs: .../stubs.c
[macho2elf] building...
[macho2elf] SUCCESS: .../eci.so
```

`macho2elf.py` leaves a working directory (`.m2e_<basename>` by default
next to the output, or override with `--workdir`) containing the
intermediate files: the section dumps, generated `stub.s`, linker script,
and runtime stub C file. Useful for debugging conversion issues.

The default flow is x86_64 → x86_64 ELF and arm64 → aarch64 ELF. The
script auto-detects via the Mach-O CPU type field and selects the right
toolchain (`gcc` for x86_64, `aarch64-linux-gnu-gcc` for arm64).

## Build options

```
--no-link       Generate stub.s and link.lds but skip the gcc invocation.
                Useful for inspecting the assembly output.
--workdir DIR   Where to place intermediate files. Default: <output_dir>/.m2e_<input_stem>
```

## What the output looks like

The resulting `.so` file:
- Has standard ELF DYN type, identified by `file` as a shared object
- Exports all the original Mach-O exports with their leading `_` stripped
  (so `_eciNew` becomes `eciNew`)
- Has DT_NEEDED entries for `libc.so.6`, `libm.so.6`, `libc++.so.1`,
  `libc++abi.so.1` (and on aarch64, the cross-link uses stub .so files that
  ld.so resolves to the real libs at load time)
- Preserves the original Mach-O virtual-address layout for all `__TEXT`
  sections — meaning every RIP-relative branch and load in the original
  code works unchanged

`readelf -d` should show roughly:
```
 0x0000000000000001 (NEEDED)  Shared library: [libc.so.6]
 0x0000000000000001 (NEEDED)  Shared library: [libm.so.6]
 0x0000000000000001 (NEEDED)  Shared library: [libc++.so.1]
 0x0000000000000001 (NEEDED)  Shared library: [libc++abi.so.1]
 0x000000000000000e (SONAME)  Library soname: [eci.so]
```

## Sanity-checking the output

```
file out/x86_64/eci.so
# ELF 64-bit LSB shared object, x86-64

nm -D --defined-only out/x86_64/eci.so | grep ' T eci' | head -5
# 0000000000001cfe T eciNew
# 000000000000272f T eciVersion
# 0000000000002c3f T eciSpeakText
# ...
```

If you have a simple harness handy:
```
gcc -Wl,-z,defs -ldl examples/version.c -o /tmp/version
/tmp/version ./out/x86_64/eci.so
# eciVersion: '6.1.0.0'
```

If `eciVersion` returns the version string, your converted .so is fully
loaded and the ECI API surface is reachable. From there, the full TTS
test (`examples/speak.c`) requires a real `eci.ini` next to the .so —
see `docs/03-integration.md`.
