# CJK Phase 0 findings — 2026-05-12

## Environment

- Host: Arch Linux (CachyOS), kernel 7.0.0-1-cachyos
- Engine: /usr/lib/eloquence/eci.so (Apple-derived tvOS 18.2 build, 2.0 MB)
- Probe: `build/examples/cjk_probe`
- Tools used: gdb (valgrind unavailable on this host)
- Symbol map: docs/cjk-investigation/eci-so-symbols.txt (756 symbols; `reset_sent_vars` IS
  stripped — we get addresses only, not function names, for engine-internal crash sites)
- Note: gdb -batch disables ASLR by default; eci.so is consistently loaded at base
  0x00007ffff7800000 across all four runs

## Probes run

| Dialect | Hex       | Result                     | Crash site                                         |
|---------|-----------|----------------------------|----------------------------------------------------|
| zh-CN   | 0x00060000 | crash at exit — 10219 samples, peak 14948 produced | rip=0x1d610 in .m2e_cstring (chsrom.so unslid atexit ptr) |
| zh-TW   | 0x00060001 | crash at exit — 10219 samples, peak 14948 produced | rip=0x1d600 in .m2e_cstring (chtrom.so unslid atexit ptr) |
| ja-JP   | 0x00080000 | crash at exit — 10186 samples, peak 17557 produced | 0x00007ffff621653a in freed anon region (jpnrom.so dangling atexit ptr) |
| ko-KR   | 0x000A0000 | clean exit — 13112 samples, peak 10437 produced    | n/a (korrom.so has no __cxa_atexit) |

## Backtraces (verbatim)

### zh-CN
```
Thread 1 "cjk_probe" received signal SIGSEGV, Segmentation fault.
0x000000000001d610 in ?? ()
#0  0x000000000001d610 in ?? ()
#1  0x00007ffff7c478f0 in ?? () from /usr/lib/libc.so.6
#2  0x00007ffff7c479e0 in exit () from /usr/lib/libc.so.6
#3  0x00007ffff7c27c95 in ?? () from /usr/lib/libc.so.6
#4  0x00007ffff7c27dcb in __libc_start_main () from /usr/lib/libc.so.6
#5  0x00005555555550c5 in _start ()
rax            0x1d610             120336
rbx            0x7ffff7e4af88      140737352347528
rcx            0x1                 1
rdx            0x1                 1
rsi            0x0                 0
rdi            0xd3470             865392
rbp            0x19                0x19
rsp            0x7fffffffe1d8      0x7fffffffe1d8
r8             0x1                 1
r9             0x0                 0
r10            0x2500              9472
r11            0x206               518
r12            0x0                 0
r13            0x7ffff7e49680      140737352341120
r14            0xd3470             865392
r15            0x7ffff7e4afa0      140737352347552
rip            0x1d610             0x1d610
eflags         0x10246             [ PF ZF IF RF ]
cs             0x33                51
ss             0x2b                43
ds             0x0                 0
es             0x0                 0
fs             0x0                 0
gs             0x0                 0
fs_base        0x7ffff7f89740      140737353652032
gs_base        0x0                 0
  Id   Target Id                                       Frame
* 1    Thread 0x7ffff7f89740 (LWP 4144731) "cjk_probe" 0x000000000001d610 in ?? ()
```

### zh-TW
```
Thread 1 "cjk_probe" received signal SIGSEGV, Segmentation fault.
0x000000000001d600 in ?? ()
#0  0x000000000001d600 in ?? ()
#1  0x00007ffff7c478f0 in ?? () from /usr/lib/libc.so.6
#2  0x00007ffff7c479e0 in exit () from /usr/lib/libc.so.6
#3  0x00007ffff7c27c95 in ?? () from /usr/lib/libc.so.6
#4  0x00007ffff7c27dcb in __libc_start_main () from /usr/lib/libc.so.6
#5  0x00005555555550c5 in _start ()
```

### ja-JP
```
Thread 1 "cjk_probe" received signal SIGSEGV, Segmentation fault.
0x00007ffff621653a in ?? ()
#0  0x00007ffff621653a in ?? ()
#1  0x00007ffff7c478f0 in ?? () from /usr/lib/libc.so.6
#2  0x00007ffff7c479e0 in exit () from /usr/lib/libc.so.6
#3  0x00007ffff7c27c95 in ?? () from /usr/lib/libc.so.6
#4  0x00007ffff7c27dcb in __libc_start_main () from /usr/lib/libc.so.6
#5  0x00005555555550c5 in _start ()
```

### ko-KR
```
cjk_probe: eciNewEx(0x000a0000)...
  -> handle=0x555555562440
cjk_probe: AddText (4 bytes)
  ProgStatus = 0x0
cjk_probe: Synthesize + Synchronize...
cjk_probe: 13112 samples, peak 10437
[Inferior 1 (process 4145794) exited normally]
No stack.
```

## Crash analysis

All three crashing dialects crash inside `exit()` while calling an atexit/`__cxa_atexit`
handler, **after synthesis has already completed successfully**. The crash is not in speech
production; it is in cleanup.

### zh-CN / zh-TW: unslid m2e static-VA pointer

`chsrom.so` and `chtrom.so` each have:
- A `.m2e_init_offs` section and a `.init_array` containing a single 8-byte pointer
  (0x2838 and 0x2870 respectively) pointing to a C++ static initializer
  (`Lexicon::setCounter(int*)` / `Lexicon::getWordlistsLen()`)
- An imported `__cxa_atexit` symbol — the static initializer registers a destructor via it

At exit, libc calls the registered destructor. The pointer stored by the m2e (Mach-O→ELF)
translator retains the **unslid Mach-O virtual address** (0x1d610 / 0x1d600) rather than
(base + offset). Because gdb disables ASLR the eci.so load base is fixed at 0x7ffff7800000,
so the correct runtime address would be 0x7ffff781d610 — but 0x1d610 is passed to the
instruction pointer directly. 0x1d610 falls inside `.m2e_cstring` (read-only string data),
not in executable code → SIGSEGV.

`addr2line -e /usr/lib/eloquence/chsrom.so -f -C 0x1d610` resolves to
`.m2e_cstring_start / stub.o:?` confirming it is string-section data, not a function entry.

### ja-JP: dangling pointer into freed anonymous heap region

`jpnrom.so` similarly has `.init_array` (pointer 0x8315 → `Romanizer::GenerateKanaOutput`)
and imports `__cxa_atexit`. The registered destructor address, 0x00007ffff621653a, is a
valid-looking VA but falls **0x1653a bytes past the end** of a 12.2 MB anonymous `rw-p`
region (0x7ffff55ca000–0x7ffff6200000) that was allocated during synthesis (almost certainly
the jpnrom.so romanizer's working heap/JIT area) and freed before `exit()` runs the handler.
Calling into freed memory → SIGSEGV.

The crash address is entirely outside eci.so (base 0x7ffff7800000); `addr2line` against
eci.so yields nothing useful for ja-JP.

### ko-KR: no atexit — clean exit

`korrom.so` has neither a `.m2e_init_offs` nor a `.init_array` section, and does **not**
import `__cxa_atexit`. Therefore no destructor is registered and `exit()` completes normally.

## Hypotheses (testable in subsequent phases)

1. **Encoding mismatch** — probe sends pre-encoded gb18030/cp932/cp949/big5; zh-CN/zh-TW/ja-JP
   still crash regardless. The crash is in atexit cleanup, not in text ingestion — encoding is
   irrelevant to whether synthesis produces audio.
   Status: **RULED OUT** as the root cause of the crashes. All four dialects synthesized audio
   successfully; encoding did not prevent synthesis from completing.

2. **Romanizer pre-load** — eci.ini wires `Path_Rom=` for chs/cht/jpn/kor. All four dialects
   loaded their romanizers and synthesized output. Pre-load is working correctly.
   Status: **RULED OUT** as a blocking issue. The romanizers load and produce output.

3. **State carry-over** — probe creates a fresh engine via `eciNewEx` (not switching
   mid-process from English). All four dialects succeeded with `eciNewEx` returning a valid
   handle, ruling out the "switching" theory for this symptom.
   Status: **RULED OUT** for fresh-process audio production. The bug only manifests at
   process exit, not during synthesis.

4. **Engine-internal init bug** — all four dialects returned a valid handle from `eciNewEx`
   and produced audio. The crashes are all in `exit()` atexit handlers, not in
   `eciNewEx`/`eciAddText`/`eciSynchronize`. The per-language init code path itself appears
   functional.
   Status: **RULED OUT** for audio production. The crash is a cleanup-time m2e translation
   artifact, not an init bug.

5. **m2e atexit pointer bug (NEW)** — chsrom.so, chtrom.so, and jpnrom.so register C++ static
   destructors via `__cxa_atexit` using unslid (Mach-O static VA) or stale (freed-heap)
   function pointer values. At process exit, libc calls those pointers, which are invalid.
   korrom.so does not use `__cxa_atexit` and exits cleanly.
   Status: **CONFIRMED** as the direct cause of exit-time crashes for zh-CN/zh-TW/ja-JP.
   The fix is to suppress or intercept these atexit calls before `dlclose`/`exit`, or to call
   `eciDelete` explicitly before returning from main so the engine (and its romanizers) get
   a chance to clean up internal state with valid pointers before libc runs the atexit queue.

## Recommendation

All four dialects produce audio successfully. The crashes are exit-time atexit artifacts from
the m2e-translated romanizer .so files (chsrom, chtrom, jpnrom), not synthesis failures.

The highest-value next experiment is **Phase 1: confirm that calling `eciDelete` before
process exit silences the atexit crashes for zh-CN, zh-TW, and ja-JP**. If `eciDelete` (or
`eciUnloadEngine`) tears down the romanizer and unregisters or safely calls its destructors
before the atexit queue fires, we can determine whether the bug is purely in cleanup ordering.
If crashes persist after `eciDelete`, intercepting `dlopen`/`dlclose`
of the romanizer .so files and stripping their `.init_array` entries
at load time is the next candidate. (Phase 2 picked an in-module
`__cxa_atexit` override instead.)

A secondary experiment worth queuing: run the same probe with `LD_PRELOAD` of a small shim
that replaces `__cxa_atexit` with a no-op, to quantify whether suppressing all atexit
registrations in the romanizers eliminates the crashes without affecting audio quality.
