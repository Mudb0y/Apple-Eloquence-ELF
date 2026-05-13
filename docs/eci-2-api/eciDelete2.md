# eciDelete2

**Inferred signature:**

```c
void eciDelete2(ECIinstance *instance);
```

Takes one argument: a raw `ECIinstance *` (the internal instance pointer, not
the outer `ECIHand` wrapper). Returns nothing — all three Apple call sites
and the legacy `eciDelete` wrapper discard `rax` after the call.

This differs from the legacy `eciDelete(ECIHand hEngine)`:

- Legacy `eciDelete` accepts the opaque `ECIHand` wrapper and dereferences it
  to obtain the raw `ECIinstance *`, then calls `eciDelete2` with that pointer.
- `eciDelete2` operates directly on the `ECIinstance *` — it is the actual
  destruction routine; `eciDelete` is just the public-facing adapter.

## Apple framework call sites

`TextToSpeechKonaSupport` imports `_eciDelete2` (import[54], stub at `0x9c74`,
GOT slot `0xe1b0`). It is called from at least three places; all follow the
same pattern: obtain an `ECIinstance *` via an objc property getter, load the
result into `rdi`, then call the stub. The return value is never used.

**Call site 1** — `-[AXKonaSpeechEngine _resetEnginePreservingParams]` at
offset `0x2671`:

```asm
    2665:      mov  rdi, rbx
    2668:      mov  rsi, r14
    266b:      call r13                     ; objc_msgSend (getter)
    266e:      mov  rdi, rax               ; rdi = ECIinstance* from getter
    2671:      call 0x9c74                  ; _eciDelete2
    2676:      mov  rsi, qword ptr [rip + 0xe283]  ; rax discarded — void return
```

**Call site 2** — `-[AXKonaSpeechEngine dealloc]` at offset `0x27f1`:

```asm
    27e2:      mov  rdi, r14
    27e5:      mov  rsi, r15
    27e8:      call qword ptr [rip + 0xba72]  ; objc_msgSend (getter)
    27ee:      mov  rdi, rax                  ; rdi = ECIinstance* from getter
    27f1:      call 0x9c74                    ; _eciDelete2
    27f6:      lea  rdi, [rbp - 0x48]         ; rax discarded — void return
```

`rdi` is always loaded with the return value of an Objective-C message send
(64-bit pointer register), confirming the single pointer argument.

## eci.so function prologue

`eci.so` + 0x7191:

```asm
0000000000007191 <eciDelete2>:
    7191:      push  rbp
    7192:      mov   rbp, rsp
    7195:      push  r14
    7197:      push  rbx
    7198:      mov   rbx, rdi               ; SAVES first arg (64-bit pointer)
    719b:      lea   rdi, [rip + 0x17ac7]   ; trace string (replaces rdi)
    71a2:      mov   rsi, rbx               ; passes saved instance* to trace
    71a5:      xor   eax, eax
    71a7:      call  0x8fa2 <_Z8eciTracePKcz>
    71ac:      test  rbx, rbx               ; null-check
    71af:      je    0x71ce <eciDelete2+0x3d>  ; branch if null
    71b1:      mov   rdi, rbx
    71b4:      call  0x8370 <_ZN11ECIinstanceD1Ev>  ; ECIinstance destructor
    71b9:      mov   rdi, rbx
    71bc:      call  0x17372 <.m2e_stubs_start+0x12>  ; free(rbx)
    71c1:      lea   rdi, [rip + 0x17ab5]   ; success trace string
    71c8:      xor   ebx, ebx               ; return value = 0 (not used)
    71ca:      xor   esi, esi
    71cc:      jmp   0x71db <eciDelete2+0x4a>
    71ce:      lea   rdi, [rip + 0x17ac5]   ; null-pointer error trace string
    71d5:      push  -0x3
    71d7:      pop   rbx                    ; rbx = -3 (error sentinel, discarded)
    71d8:      mov   rsi, rbx
    71db:      xor   eax, eax
    71dd:      call  0x8fa2 <_Z8eciTracePKcz>
    71e2:      mov   rax, rbx               ; rax = 0 or -3
    71e5:      pop   rbx
    71e6:      pop   r14
    71e8:      pop   rbp
    71e9:      ret
```

Key observations:

- `mov rbx, rdi` at `7198` — the first argument is saved as a 64-bit
  pointer into `rbx`. This is the single argument.
- `rdi` is immediately overwritten afterward with a trace string, so there is
  no second argument read from `rsi`.
- The function calls `_ZN11ECIinstanceD1Ev` (the `ECIinstance` C++ destructor)
  on the pointer, then frees the memory — it is the core destructor for the
  engine instance.
- `rax` on return is `0` (success) or `-3` (null input), but this value is
  never read by any caller observed.

## Comparison: eciDelete (legacy) prologue

```asm
0000000000001fb4 <eciDelete>:
    1fb4:      push  rbp
    1fb5:      mov   rbp, rsp
    1fb8:      push  rbx
    1fb9:      push  rax
    1fba:      test  rdi, rdi               ; null-check the ECIHand
    1fbd:      je    0x1fef <eciDelete+0x3b>
    1fbf:      mov   rbx, rdi               ; save ECIHand (the wrapper pointer)
    ...
    2019:      mov   rdi, qword ptr [rbx]   ; DEREFERENCE: load ECIinstance* from handle
    201c:      call  0x7191 <eciDelete2>    ; call eciDelete2 with raw instance*
    2021:      cmp   qword ptr [rbx + 0x3e0], 0x0  ; rax from eciDelete2 discarded
```

Legacy `eciDelete` is a wrapper: it validates and dereferences the `ECIHand`
shell to obtain the raw `ECIinstance *` at `[hEngine]`, then delegates to
`eciDelete2`. The "2" suffix indicates direct operation on the internal instance
pointer — the lower-level ABI that Apple's framework uses directly.

## Notes

- **One argument** — `rdi` is saved to `rbx` at the third instruction
  (64-bit, pointer). No other argument registers are read.
- **Effectively void return** — `rax` is set to 0 or -3 internally, but no
  caller inspects it.
- The "2" API operates on `ECIinstance *` directly; the legacy API accepts the
  opaque `ECIHand` wrapper and unwraps it.
- Apple's framework bypasses the legacy wrapper entirely and calls `eciDelete2`
  with the raw instance pointer it maintains in its Objective-C object graph.
- The function body is: null-check, destructor (`~ECIinstance()`), free —
  straightforward RAII cleanup.
