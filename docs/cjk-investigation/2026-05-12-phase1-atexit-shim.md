# CJK Phase 1 — LD_PRELOAD __cxa_atexit shim experiment

Hypothesis from Phase 0: suppressing `__cxa_atexit` registrations from
`chsrom.so` / `chtrom.so` / `jpnrom.so` eliminates the exit-time SIGSEGVs while
leaving synthesis unaffected.

## Setup

- Shim: `tools/cjk_atexit_shim.c`
- Built: `tools/cjk_atexit_shim.so`
- Probe: `build/examples/cjk_probe`
- Engine: `/usr/lib/eloquence/eci.so`

## Shim design — two detection strategies

The initial shim used only `__builtin_return_address(0)` to identify the caller.
This caught 16 of the 17 chsrom.so/chtrom.so registrations but **missed the fatal
one** for each dialect: the dynamic linker (`ld-linux-x86-64.so.2`) registers the
`.fini_array`/`DT_FINI_ARRAY` entry of each romanizer .so on its behalf at
`dlopen()` time, passing through with `ld-linux` as the caller but with a `func`
pointer that lives inside the romanizer .so.

The final shim applies three strategies in order:

1. **Caller-based**: `dladdr(__builtin_return_address(0))` — catches explicit
   registrations from C++ static initializers that call `__cxa_atexit` directly.
2. **Func-based**: `dladdr(func)` — catches linker-mediated registrations where
   `ld-linux` is the caller but `func` resolves into a suspect .so.
3. **Unslid-VA fallback**: if `func < 0x200000`, no valid user-space mapping can
   exist at that address; any such registration is suppressed regardless of caller.
   This catches the zh-CN/zh-TW unslid Mach-O static VA pointers (0x1d610 /
   0x1d600) even if strategies 1 and 2 both fail.

## Results

| Dialect | Without shim | With shim | Suppressions logged |
|---------|--------------|-----------|---------------------|
| zh-CN (0x00060000) | 10219 samples, peak 14948 — SIGSEGV at exit (exit=139) | 10219 samples, peak 14948 — clean exit (exit=0) | 17 (16 caller-based + 1 func-based/unslid: func=0x1d610 from chsrom.so) |
| zh-TW (0x00060001) | 10219 samples, peak 14948 — SIGSEGV at exit (exit=139) | 10219 samples, peak 14948 — clean exit (exit=0) | 17 (16 caller-based + 1 func-based/unslid: func=0x1d600 from chtrom.so) |
| ja-JP (0x00080000) | 10186 samples, peak 17557 — SIGSEGV at exit (exit=139) | 10186 samples, peak 17557 — clean exit (exit=0) | 1 (func-based: func=0x7f…1653a from jpnrom.so, registered by ld-linux) |
| ko-KR (0x000A0000) | 13112 samples, peak 10437 — clean exit (exit=0) | 13112 samples, peak 10437 — clean exit (exit=0) | 0 (korrom.so has no __cxa_atexit — as expected from Phase 0) |

## Key diagnostic finding — the ld-linux registration path

A full-logging shim (logging all `__cxa_atexit` calls without suppression) revealed
the exact registration sequence for zh-CN:

```
caller=/usr/lib/eloquence/chsrom.so  func=0x133da  (16 × — the .init_array explicit registrations)
caller=/lib64/ld-linux-x86-64.so.2   func=0x1d610  (1 × — the fatal .fini_array registration)
```

The fatal 17th call has `func=0x1d610`, which is the unslid Mach-O static VA (confirmed
by Phase 0: `addr2line` resolves it to `.m2e_cstring` string data, not code). This call
originates from the dynamic linker, not from chsrom.so itself, because the m2e
translator preserved the `.fini_array` pointer verbatim (unslid). The linker calls
`__cxa_atexit(fini_array_func, …)` when `dlopen()` loads the .so; at exit, libc calls
`0x1d610` as a function pointer → SIGSEGV.

For ja-JP, jpnrom.so does not make any direct `__cxa_atexit` calls — only the single
linker-mediated `.fini_array` registration with `func=0x7f…1653a` (a dangling pointer
into freed heap, as established in Phase 0).

The caller-only shim suppressed the 16 explicit chsrom.so calls but passed through the
fatal linker call. The improved func-based detection catches it because `func < 0x200000`
(for zh-CN/zh-TW) and because `dladdr(func)` resolves to jpnrom.so (for ja-JP).

## Verdict

**HYPOTHESIS CONFIRMED**

Suppressing `__cxa_atexit` registrations where the function pointer originates from
`chsrom.so`, `chtrom.so`, or `jpnrom.so` — whether registered directly by the .so or
indirectly by the dynamic linker as a `.fini_array` handler — eliminates all three
exit-time SIGSEGVs while leaving audio synthesis entirely unaffected. All four dialects
produce identical sample counts and peak amplitudes with the shim active. `ko-KR` was
unaffected in both modes, consistent with Phase 0's finding that `korrom.so` registers
no `__cxa_atexit` handlers.

The root cause is the m2e (Mach-O→ELF) translator preserving `.fini_array` function
pointers at their original unslid Mach-O virtual addresses, or pointing into anonymous
heap regions that are deallocated before exit runs the atexit queue. The correct fix is
to zero or skip `.fini_array`/`DT_FINI` entries in the m2e output for these three .so
files, rather than suppressing at runtime via LD_PRELOAD.

## Next steps

Phase 2 implemented the in-module `__cxa_atexit` override approach,
eliminating the need for `LD_PRELOAD` in production. See
`2026-05-12-phase2-in-module-override.md`.
