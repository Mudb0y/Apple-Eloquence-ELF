# CJK Phase 2 — in-module `__cxa_atexit` override (Path D)

Phase 1 confirmed via `LD_PRELOAD` that suppressing `__cxa_atexit`
registrations from `chsrom` / `chtrom` / `jpnrom` eliminates exit-time
crashes while preserving synthesis output. Phase 2 ships the same
suppression logic compiled directly into the `sd_eloquence` binary — no
`LD_PRELOAD` ceremony, single install artifact.

## Mechanism

`sd_eloquence/src/cjk_atexit_override.c` defines a top-level
`__cxa_atexit` that:

1. Returns 0 (suppress) for callers / func pointers that identify as
   `chsrom.so`, `chtrom.so`, `jpnrom.so`, or `korrom.so`.
2. Falls back to libc's real `__cxa_atexit` (via
   `dlsym(RTLD_NEXT, "__cxa_atexit")`) for every other caller.

Three detection strategies, applied in order:

- `dladdr(__builtin_return_address(0))` — catches explicit calls from
  inside the converted romanizers' code.
- `dladdr((void *)func)` — catches `DT_FINI_ARRAY` entries applied by
  `ld-linux` on the .so's behalf (apparent caller is the dynamic linker,
  but the func pointer falls inside a suspect .so).
- `((uintptr_t)func < 0x200000)` — unslid-static-VA heuristic for
  `chs/chtrom`, where the func pointer is a raw Mach-O VA that doesn't
  `dladdr`-resolve at all.

CMake passes `-rdynamic` so the dynamic linker resolves `chsrom/cht/jpnrom`'s
`__cxa_atexit` references to OUR override before falling back to libc.
Verified with `nm -D`:

```
$ nm -D build/sd_eloquence | grep __cxa_atexit
000000000000ce24 T __cxa_atexit

$ nm -D build/examples/cjk_probe | grep __cxa_atexit
0000000000001672 T __cxa_atexit
```

Both binaries expose `__cxa_atexit` as defined text (`T`), not undefined
(`U` would mean libc resolves it).

## Verification

Linked the same override into `examples/cjk_probe` (also built with
`-rdynamic`) and re-ran all four CJK dialects **without `LD_PRELOAD`**:

| Dialect | Sample count | Exit code | Suppressions logged |
|---------|--------------|-----------|---------------------|
| zh-CN (0x00060000) | 10219, peak 14948 | 0 (was 139) | 3 — all from `chsrom.so` (funcs 0x133da × 2, 0x1d610) |
| zh-TW (0x00060001) | 10219, peak 14948 | 0 (was 139) | 3 — all from `chtrom.so` (funcs 0x13412 × 2, 0x1d600) |
| ja-JP (0x00080000) | 10186, peak 17557 | 0 (was 139) | 1 — from `jpnrom.so` (func 0x7fc62761653a, registered by ld-linux) |
| ko-KR (0x000A0000) | 13112, peak 10437 | 0 (unchanged) | 0 — no registrations to suppress |

Sample counts are bit-identical to Phase 1's "with shim" column, confirming
the in-module path catches exactly the same registrations the LD_PRELOAD
shim did. The 6 existing unit tests (voices / languages / config / marks /
ssml / filters) still pass.

## Follow-up

Phase 3 ([`2026-05-13-phase3-api-divergence.md`](2026-05-13-phase3-api-divergence.md))
discovered the remaining CJK crash isn't atexit-related: Apple's
framework drives the engine via the modern 2-suffixed ECI API, and
the romanizer initialization path needs that codepath rather than
the legacy one v1 uses. CJK stays gated in v1; the API switch is
deferred to v2.
