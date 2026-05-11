# 05 — Troubleshooting

Known issues, common errors, and their causes.

## dlopen errors

### `cannot open shared object file: libc++.so.1`

You don't have libc++ installed. The Apple dylibs use libc++ (not libstdc++);
install it:

- Arch: `sudo pacman -S libc++ libc++abi`
- Debian/Ubuntu: `sudo apt install libc++1 libc++abi1`
- Fedora: `sudo dnf install libcxx libcxxabi`

### `cannot enable executable stack as shared object requires`

Your converted .so doesn't have a PT_GNU_STACK program header. This
should be fixed by macho2elf.py's linker script automatically. If you
see it, your build is missing the `.note.GNU-stack` section. Check
that the linker script generation correctly emits the `gnustack` PHDR
entry.

### `object file has no dynamic section`

The PT_DYNAMIC program header is missing. Should be auto-emitted by ld
when you pass an output that has `.dynamic`. If you see this, check
that the `dynamic PT_DYNAMIC FLAGS(6);` entry is in your generated
linker script.

### `ELF load command address/offset not page-aligned`

PT_LOAD segments overlap or have mismatched file-offset/vaddr modulo
page size. Most commonly caused by:
- Linker-auto sections (`.plt`, `.text` from stubs.c) being placed
  inside the Mach-O image's vaddr range. The converter handles this
  by placing them in a separate `auxtext` PHDR far above the Mach-O
  range — make sure that's working.
- `__DATA_CONST` going into a separate rodata PHDR while `__DATA`
  is in rwdata. Mach-O packs them at adjacent vaddrs which creates
  segment overlap. The converter unifies both into rwdata.

## Runtime crashes

### `*** stack smashing detected ***` inside eciNew

The ETI Eloquence engine has internal fixed-size stack buffers that
overflow on long config-file section names. Specifically `setInitialState`
strcpies eci.ini section names into a 10-byte buffer at `rbp-0x3a`,
adjacent to the canary at `rbp-0x30`. Section names longer than 9 chars
(including null) overflow the buffer and smash the canary.

**Fix:** keep eci.ini section names ≤ 7 chars (so `[name]\0` is ≤ 10
bytes total). The included `examples/eci.ini` uses `[1.0]` which fits.

### Segfault inside `eciAddText` on tvOS 18.2 builds (with full eci.ini)

The 18.2 Eloquence build has smaller internal config-table buffers than
16.4. Older comprehensive eci.ini files (such as LevelStar Icon's, which
includes 37 `Phoneme<N>=` entries plus 8 `Voice<N>=` entries) overflow a
config-table buffer somewhere during eciAddText, corrupting the Klatt
hook callback's instance-data slot. The next time the hook fires, it
gets passed a garbage pointer that points into ASCII text, and `eciAddText`
segfaults inside `SynthThread::staticKlattConstHook`.

The threshold appears to be around 35-38 combined Voice+Phoneme entries
(varies based on combination — not a simple total).

**Fix:** use a minimal `eci.ini` like the bundled `examples/eci.ini`:
```
[1.0]
Path=/absolute/path/to/your/language.so
Version=6.1
```

The engine has fully-tuned built-in defaults; the Voice/Phoneme entries
are tuning overrides that aren't required. Audio quality is unchanged
without them.

If you need the Voice tuning capability, use the tvOS 16.4 build instead —
it accepts the full LevelStar-style config.

### `Language Library not opened: <path>: cannot open shared object file`

The engine's `Path=` line in `eci.ini` points at a file that doesn't
exist or isn't a valid .so. Use an absolute path. Verify the language
.so loads with `dlopen` standalone before pointing eci.ini at it.

### `Language Library not opened: <path>: undefined symbol: <name>`

The language .so has a Darwin-specific symbol that doesn't resolve on
Linux. Check the symbol against `DARWIN_TO_LINUX` in macho2elf.py.
Common ones we already handle:

- `__error` → `__errno_location`
- `__stderrp`/`__stdoutp`/`__stdinp` → `stderr`/`stdout`/`stdin`
- `__tolower`/`__toupper` → `tolower`/`toupper`
- `_DefaultRuneLocale` → stub provided by stubs.c
- `__maskrune` → stub provided by stubs.c

If you see a new one, add it to the `DARWIN_TO_LINUX` dict at the
top of `macho2elf.py` and rebuild.

### `eciNew` returns NULL silently

- `eci.ini` not in current working directory of the process
- `eci.ini` section name too long (see above)
- `Path=` pointing at nonexistent language .so
- Engine ran out of file descriptors / memory

Create an empty `eci.dbg` file in cwd (must be O_RDWR-able) and re-run.
The engine writes detailed diagnostic logs there, including which step
of init failed.

### `eciSetOutputBuffer` returns "NULL eciHandle or no callback"

You called `eciSetOutputBuffer` before `eciRegisterCallback`. The engine
requires a callback to be registered first so it knows where to send
audio-ready notifications. Reorder your code:

```c
void *eci = eciNew();
eciRegisterCallback(eci, my_callback, my_data);   // first
eciSetOutputBuffer(eci, sample_count, my_buffer); // then this
```

## Architecture-specific notes

### arm64 builds segfault during dlopen under qemu-user

Known issue. The converted aarch64 .so files are structurally correct
(valid ELF, correct DT_NEEDED, valid INIT_ARRAY) but qemu-aarch64's
emulation of glibc's complex C++ initialization (in particular the
unwind tables and libc++abi's exception machinery) is incomplete.

**Workarounds:**
- Test on actual aarch64 hardware (Raspberry Pi 4/5, Asahi-Linux Mac,
  AWS Graviton, etc.) — qemu-user is not representative.
- If you must use emulation, qemu-system (full-system emulation with
  a real arm64 kernel and userspace) works more reliably than
  qemu-user.

### Romanization helper modules return `eciNew NULL`

The 18.2 set includes 4 `*rom.dylib` modules (`jpnrom`, `korrom`, `chsrom`,
`chtrom`). These are **not standalone synthesizers** — they're auxiliary
text-to-phoneme transliterators that the engine uses internally when the
main synthesizer needs to consume already-romanized text. Calling `eciNew`
against a `*rom.so` directly will fail with NULL because they don't
register a complete language. They need to be configured as secondary
language modules alongside their primary (e.g., `chs.so` + `chsrom.so`),
which the public ECI API doesn't seem to surface cleanly.

For now: use the primary CJK synthesizers (`chs.so`, `cht.so`, `jpn.so`,
`kor.so`) directly. They can accept native CJK text and synthesize it
without the `*rom` helpers in basic usage.

## Build errors

### `Error: junk at end of line, first unrecognized character is '#'`

GAS on aarch64 doesn't accept `#` as a line comment — `#` denotes
immediate values (e.g., `mov x0, #4`). If you see this when running
macho2elf.py against an arm64 slice, it means a comment somewhere is
using `#` syntax. The converter uses `/* */` block comments to
sidestep this; if you've modified the converter, make sure new
comment lines use that form.

### `Error: invalid operands (*UND* and *ABS* sections) for '/'`

Inverse of the above — on x86_64 GAS, `//` is NOT a line comment
(unlike aarch64), so a `//` comment becomes `/` (division operator).
Use `#` or `/* */` for x86_64.

The converter standardizes on `/* */` so this shouldn't happen.

### Linker: `cannot find -l:libc++.so.1`

Building for arm64 and your cross-toolchain sysroot doesn't have
libc++. macho2elf.py generates empty stub .so files at link time
specifically to work around this. If you see this error, the stub
generation may have failed — check `<workdir>/stub_libs/` for the
`.so` files. Should be tiny (a few KB) with just the right SONAME.

## Verifying your build

```bash
./tools/verify.sh
```

Compares SHA256 of every file under `vendor/` and `prebuilt/` against
the expected values in `tools/checksums.txt`. Use this after fresh
extraction or conversion to ensure binary integrity.
