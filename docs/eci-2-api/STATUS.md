# ECI 2-API switch — DEFERRED TO V2

**Status:** parked after RE1 + RE2 (2026-05-13). Resume when v2 (CJK support) is on the roadmap.

## What's here

Two reverse-engineered function signatures, committed as durable research:

- `eciNewEx2.md` — found to take ZERO args (`ECIHand eciNewEx2(void)`), defaults dialect to 0. Internal wrapper around `eciNew2(instance, 0)` + additional init.
- `eciDelete2.md` — found to take an `ECIinstance *` (the raw engine instance pointer), NOT the wrapper `ECIHand`. Legacy `eciDelete(ECIHand)` literally `mov rdi, [rbx]; call eciDelete2`.

Plus the workflow `README.md` for picking up subsequent RE.

## Why parked

The v1 brainstorm in `docs/superpowers/specs/2026-05-13-eci-2-api-switch-design.md` assumed the `2`-suffixed functions were drop-in replacements for their legacy counterparts. RE1 and RE2 falsified that:

- Signatures differ structurally (arg counts, types).
- `2` functions operate at a different abstraction layer (`ECIinstance *` instead of `ECIHand`).
- Apple's framework doesn't use the `2`-suffixed wrappers we initially targeted (e.g. it uses `eciNew2` directly, not `eciNewEx2`).

A clean wholesale swap isn't viable without significantly more RE — likely 14+ more function tasks plus an architectural rethink of the engine wrapper to handle both abstraction layers cleanly.

Since v1 ships Latin-only (CJK was the only consumer that needed the modern API), this work doesn't block v1. It moves to v2 if/when CJK support becomes a goal.

## Picking it up later

If/when you resume:

1. Read the spec + plan at `docs/superpowers/specs/2026-05-13-eci-2-api-switch-design.md` and `docs/superpowers/plans/2026-05-13-eci-2-api-switch.md` — both are still valid as roadmaps, but the per-function "likely signature" guesses in the plan are guidance only; verify each via RE.
2. Continue with RE3 (`eciSetParam2`) using the workflow in `README.md`.
3. After ~5-6 more RE tasks, consider whether the wholesale swap in the spec still makes sense or whether a different architecture (e.g. dual-layer engine handle) is warranted.
4. The `ECIinstance *` vs `ECIHand` divergence found in RE2 is the most important structural finding — it suggests the engine wrapper needs to track both pointer types if we use the 2 API alongside legacy.

## Related artifacts

- `docs/cjk-investigation/2026-05-13-phase3-api-divergence.md` — the original finding that motivated this project.
- `docs/macho2elf-audit/` — the audit that ruled out converter bugs.
- `sd_eloquence/src/module.c` line ~80 — where CJK is gated for v1.
