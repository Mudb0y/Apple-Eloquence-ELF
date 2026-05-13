# eciNewEx2

**Inferred signature:**

```c
ECIHand eciNewEx2(void);
```

Takes no arguments. Internally calls `eciNew2` with `esi=0` (dialect hardcoded
to zero / `eciGeneralAmericanEnglish`). Returns an `ECIHand` (pointer-sized,
via `rax`).

This differs from the legacy `eciNewEx(enum ECILanguageDialect dialect)`, which
preserves the caller-supplied dialect in `r14d` and forwards it to `eciNew2`.
`eciNewEx2` always uses dialect=0 and performs additional post-construction
initialization (language-module state, copy of voice table into the instance)
that the legacy `eciNewEx` does not.

The Apple tvOS 18.2 framework (`TextToSpeechKonaSupport`) does **not** import
`_eciNewEx2` directly; it calls `_eciNew2` instead. `eciNewEx2` is a
higher-level wrapper exported by `eci.so` for embedders that want the fully
initialized instance without managing the two-step `eciNew2` + init sequence.

## Apple framework call site

`TextToSpeechKonaSupport` does not call `_eciNewEx2`. It calls `_eciNew2`
(import[58], stub at 0x9c8c) directly from
`-[AXKonaSpeechEngine _initializeWrappedEngineForVoice:]` at offset 0x301c:

```asm
    2ff5:      mov  rdi, rbx
    2ff8:      mov  esi, eax
    2ffa:      call 0x9caa          ; _eciRegisterCallback2
    2fff:      mov  rsi, qword ptr [rip + 0xd96a]
    3006:      mov  rdi, r14
    3009:      mov  r15, r12
    300c:      call qword ptr [rip + 0xb24e]   ; indirect (objc_msgSend-class)
    3012:      mov  r12, qword ptr [rbp - 0x50]
    3016:      lea  rdi, [rbp - 0x30]   ; arg1 = pointer to output slot (ECIHand*)
    301a:      mov  esi, eax            ; arg2 = dialect (int, from prior return)
    301c:      call 0x9c8c              ; _eciNew2 (GOT[58] = _eciNew2)
    3021:      mov  r13, rax            ; result = ECIHand
```

Interpretation: `_eciNew2` takes two arguments — `rdi` = `ECIHand *` (output
pointer), `esi` = `int` (dialect). The Apple framework does the two-arg call
directly. `eciNewEx2` wraps `eciNew2` with these two args hardcoded.

## eci.so function prologue

`eci.so` + 0x1cda:

```asm
0000000000001cda <eciNewEx2>:
    1cda:      push  rbp
    1cdb:      mov   rbp, rsp
    1cde:      push  rbx
    1cdf:      push  rax
    1ce0:      lea   rdi, [rip + 0x1b245]    ; trace string (overwrites arg1 slot)
    1ce7:      xor   eax, eax
    1ce9:      call  0x8fa2 <_Z8eciTracePKcz>
    1cee:      lea   rbx, [rip + 0x2270b]    ; mutex pointer
    1cf5:      push  -0x1
    1cf7:      pop   rsi
    1cf8:      mov   rdi, rbx
    1cfb:      call  0x13ebc <_ZN5Mutex4waitEl>
    1d00:      inc   dword ptr [rip + 0x22716]
    1d06:      mov   rdi, rbx
    1d09:      call  0x13e64 <_ZN5Mutex7releaseEv>
    1d0e:      mov   edi, 0x24f8             ; malloc(0x24f8)
    1d13:      call  0x1743e                 ; malloc stub
    1d18:      test  rax, rax
    1d1b:      je    0x1d5b <eciNewEx2+0x81>
    1d1d:      mov   rbx, rax               ; rbx = new instance
    1d20:      mov   edx, 0x24f8
    1d25:      mov   rdi, rax
    1d28:      xor   esi, esi               ; zero-fill
    1d2a:      call  0x1744a                 ; memset stub
    1d2f:      mov   dword ptr [rbx + 0x2c], 0x1
    1d36:      mov   rdi, rbx               ; arg1 = instance*
    1d39:      xor   esi, esi               ; arg2 = dialect 0 (hardcoded)
    1d3b:      call  0x6f33 <eciNew2>       ; -> ECIHand stored at [instance]
```

Interpretation: `edi` (the first integer argument register) is immediately
overwritten at `1ce0` with a trace string pointer. No incoming dialect argument
is saved. The function constructs a new instance internally, passes `esi=0`
(dialect=0) to `eciNew2`, then performs additional initialization steps before
returning `rax`.

## Comparison: eciNewEx (legacy) prologue

```asm
000000000000138e <eciNewEx>:
    138e:      push  rbp
    138f:      mov   rbp, rsp
    1392:      push  r14
    1394:      push  rbx
    1395:      mov   r14d, edi          ; SAVES dialect argument
    1398:      lea   rdi, [rip + 0x1ba9b]
    ...
    13f1:      mov   dword ptr [rbx + 0x3c], r14d  ; stores dialect in instance
    13f5:      mov   rdi, rbx
    13f8:      mov   esi, r14d          ; forwards dialect to eciNew2
    13fb:      call  0x6f33 <eciNew2>
```

`eciNewEx` saves `edi` into `r14d` at the third instruction and uses it.
`eciNewEx2` never saves or uses `edi`. This is the disassembly evidence that
`eciNewEx2` takes zero arguments.

## Notes

- **Zero arguments** — `edi` is not saved; dialect is hardcoded to 0 internally.
- Return in `rax` (pointer-sized, `ECIHand = void *`).
- No stack args, no floating-point args.
- The "2" suffix indicates the *modern* initialization code path: after `eciNew2`
  it runs additional init steps (voice-table copy, language-module state) that
  `eciNewEx` skips. This is why CJK crashes with the legacy path — those
  language-specific structures are never populated.
- Apple's framework bypasses `eciNewEx2` entirely and calls `eciNew2` directly
  with a dialect value, giving it full control over dialect selection.
