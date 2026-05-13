#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
macho2elf — Convert a libc-only Mach-O dylib to a Linux ELF .so

Targeted at Apple's bundled ETI Eloquence engine (eci.dylib + language dylibs)
which has only /usr/lib/libSystem.B.dylib + /usr/lib/libc++.1.dylib deps.

Strategy:
  1. Parse Mach-O via LIEF, extract every section as a binary blob
  2. Emit an assembly stub that .incbin's each blob into a named ELF section
  3. Emit symbol definitions: each export becomes a .globl alias at the right
     offset within the section it lives in
  4. Emit import stubs: declare each Darwin import as ELF UND, apply Darwin->
     Linux renames (e.g. ___error -> __errno_location)
  5. Emit a custom GOT section: .quad <symbol> for each Mach-O __DATA_CONST,__got
     entry, in original order, at original vaddrs (so the existing __stubs jmps
     still resolve correctly)
  6. Emit a linker script that pins each ELF section at its original Mach-O
     virtual address (preserves all RIP-relative offsets unchanged)
  7. Invoke gcc -shared to link the final .so

Usage:
  macho2elf.py <input.dylib.x86_64> -o <output.so> [--workdir <dir>]

The input must already be the extracted x86_64 slice (use llvm-lipo -extract).
"""

import argparse
import os
import struct
import sys
from pathlib import Path

import lief

# ----------------------------------------------------------------------------
# Symbol renaming: Darwin name (after stripping the leading underscore that
# Mach-O prepends to every C/C++ symbol) -> Linux equivalent.
# ----------------------------------------------------------------------------

DARWIN_TO_LINUX = {
    # errno accessor
    "__error":           "__errno_location",
    # Darwin ctype function-form: Linux has tolower/toupper/etc. directly
    "__tolower":         "tolower",
    "__toupper":         "toupper",
    # Darwin's _DefaultRuneLocale ctype table — glibc uses a different model
    "_DefaultRuneLocale": "_macho2elf_rune_locale_stub",
    # Darwin's __stderrp/__stdoutp/__stdinp are FILE * globals — Linux uses
    # plain stderr/stdout/stdin variable names
    "__stderrp":         "stderr",
    "__stdoutp":         "stdout",
    "__stdinp":          "stdin",
}

# C symbols that exist on both platforms with identical names and semantics.
# We don't need to rename these — just strip the Mach-O leading underscore.
# (kept here for documentation / sanity checks)

# ----------------------------------------------------------------------------
# Mach-O section -> ELF section mapping
# ----------------------------------------------------------------------------

# Each Mach-O section gets mapped to a uniquely named ELF section so the linker
# script can place it at the right vaddr. Sections that need special handling
# (got, common, bss) are emitted differently and not listed here.

SECTION_LAYOUT = [
    # (seg, sect, elf_name, flags, type)
    ("__TEXT",       "__text",            ".m2e_text",         '"ax"',  "@progbits"),
    ("__TEXT",       "__stubs",           ".m2e_stubs",        '"ax"',  "@progbits"),
    ("__TEXT",       "__init_offsets",    ".m2e_init_offs",    '"a"',   "@progbits"),
    ("__TEXT",       "__gcc_except_tab",  ".m2e_gcc_except",   '"a"',   "@progbits"),
    ("__TEXT",       "__const",           ".m2e_text_const",   '"a"',   "@progbits"),
    ("__TEXT",       "__cstring",         ".m2e_cstring",      '"a"',   "@progbits"),
    ("__TEXT",       "__unwind_info",     ".m2e_unwind",       '"a"',   "@progbits"),
    ("__DATA_CONST", "__const",           ".m2e_data_const",   '"aw"',  "@progbits"),
    ("__DATA",       "__got_weak",        ".m2e_got_weak",     '"aw"',  "@progbits"),
    ("__DATA",       "__const_weak",      ".m2e_const_weak",   '"aw"',  "@progbits"),
    ("__DATA",       "__data",            ".m2e_data",         '"aw"',  "@progbits"),
]

# ----------------------------------------------------------------------------


def collect_relocation_events(binary):
    """Walk a LIEF MachO.Binary, return events_by_section.

    Public hook so tools/audit_relocs.py can compare what macho2elf would
    emit against ground truth, without running gcc. The format of
    events_by_section matches what the assembly emitter consumes:
        events_by_section[(seg, sect)] = [
            (site_offset_within_section, "rebase", (tgt_label, tgt_off, tgt_seg, tgt_sect)),
            (site_offset_within_section, "symref", (symbol_name, library_name)),
            ...
        ]

    Note: the rebase tuple is extended from the existing (tgt_label, tgt_off)
    form to also carry (tgt_seg, tgt_sect) for the audit's diff machinery.
    Consumers that don't need the extras can ignore tuple indices 2 and 3.
    """
    macho_to_elf_label = {}
    for seg, sect, elf_name, _, _ in SECTION_LAYOUT:
        macho_to_elf_label[(seg, sect)] = f"{elf_name}_start"
    macho_to_elf_label[("__DATA_CONST", "__got")] = ".m2e_got_start"
    macho_to_elf_label[("__DATA", "__common")] = ".m2e_common_start"
    macho_to_elf_label[("__DATA", "__bss")] = ".m2e_bss_start"

    events_by_section = {}

    # External bindings (libSystem / libc++ imports)
    binding_addrs = {bi.address for bi in binary.bindings}
    for bi in binary.bindings:
        site_seg = site_sect = None
        site_off = None
        for s in binary.sections:
            if s.virtual_address <= bi.address < s.virtual_address + s.size:
                site_seg, site_sect = s.segment_name, s.name
                site_off = bi.address - s.virtual_address
                break
        if site_seg is None:
            continue
        try:
            symbol = bi.symbol.name if bi.has_symbol else "<no_sym>"
        except (AttributeError, RuntimeError):
            symbol = "<no_sym>"
        try:
            library = bi.library.name if hasattr(bi, "library") and bi.library else "?"
        except (AttributeError, RuntimeError):
            library = "?"
        events_by_section.setdefault((site_seg, site_sect), []).append(
            (site_off, "symref", (symbol, library)))

    # Internal rebases (anything in binary.relocations that ISN'T a binding)
    for r in binary.relocations:
        if r.address in binding_addrs:
            continue
        site_seg = site_sect = None
        site_off = None
        for s in binary.sections:
            if s.virtual_address <= r.address < s.virtual_address + s.size:
                site_seg, site_sect = s.segment_name, s.name
                site_off = r.address - s.virtual_address
                break
        if site_seg is None:
            continue
        tgt = r.target
        tgt_seg = tgt_sect = None
        tgt_off = None
        for s in binary.sections:
            if s.virtual_address <= tgt < s.virtual_address + s.size:
                tgt_seg, tgt_sect = s.segment_name, s.name
                tgt_off = tgt - s.virtual_address
                break
        if tgt_seg is None:
            continue
        tgt_label = macho_to_elf_label.get((tgt_seg, tgt_sect))
        if tgt_label is None:
            continue
        events_by_section.setdefault((site_seg, site_sect), []).append(
            (site_off, "rebase", (tgt_label, tgt_off, tgt_seg, tgt_sect)))

    return events_by_section


# ----------------------------------------------------------------------------

def strip_underscore(name: str) -> str:
    """Strip a single leading underscore from a Mach-O symbol name."""
    if name.startswith('_'):
        return name[1:]
    return name


def rename_import(macho_name: str) -> str:
    """Strip leading underscore and apply Darwin->Linux rename if needed."""
    stripped = strip_underscore(macho_name)
    return DARWIN_TO_LINUX.get(stripped, stripped)


def extract_sections(binary: lief.MachO.Binary, sections_dir: Path) -> dict:
    """Dump every section's content as a binary blob; return metadata."""
    sections_dir.mkdir(parents=True, exist_ok=True)
    out = {}
    for s in binary.sections:
        key = (s.segment_name, s.name)
        # Skip zero-sized or special "fill" sections (handled separately)
        if s.size == 0:
            out[key] = {
                "vaddr": s.virtual_address,
                "size":  s.size,
                "file":  None,
            }
            continue
        blob_path = sections_dir / f"{s.segment_name.strip('_')}__{s.name.strip('_')}.bin"
        with open(blob_path, "wb") as f:
            f.write(bytes(s.content))
        out[key] = {
            "vaddr": s.virtual_address,
            "size":  s.size,
            "file":  blob_path,
        }
    return out


def collect_exports(binary: lief.MachO.Binary) -> list:
    """Return a list of (linux_name, vaddr) tuples for every export."""
    exports = []
    for sym in binary.exported_symbols:
        linux_name = strip_underscore(sym.name)
        exports.append((linux_name, sym.value))
    return exports


def collect_bindings_per_section(binary: lief.MachO.Binary) -> tuple:
    """Walk all bindings (legacy dyld_info AND chained-fixup formats).

    Returns:
      imports:       set of Linux symbol names to extern-declare
      bindings_by_section:  {(seg, sect): [(offset_within_section, linux_name), ...]}
    """
    imports = set()
    by_section = {}

    # Build section lookup: vaddr range -> (seg, sect, base_vaddr)
    sect_ranges = []
    for s in binary.sections:
        sect_ranges.append((s.virtual_address, s.virtual_address + s.size,
                            s.segment_name, s.name))

    def find_section(vaddr):
        for lo, hi, seg, sect in sect_ranges:
            if lo <= vaddr < hi:
                return seg, sect, lo
        return None, None, None

    # Walk bindings — LIEF abstracts dyld_info AND chained-fixup formats here
    for b in binary.bindings:
        if not b.has_symbol:
            continue
        seg, sect, base = find_section(b.address)
        if seg is None:
            continue
        offset = b.address - base
        linux_name = rename_import(b.symbol.name)
        by_section.setdefault((seg, sect), []).append((offset, linux_name))
        imports.add(linux_name)

    # Also include any imports that may have no binding site
    for sym in binary.imported_symbols:
        imports.add(rename_import(sym.name))

    for k in by_section:
        by_section[k].sort(key=lambda x: x[0])

    return sorted(imports), by_section


def get_section_vaddr(binary, seg, sect):
    """Return vaddr of a given Mach-O section, or None if absent."""
    for s in binary.sections:
        if s.segment_name == seg and s.name == sect:
            return s.virtual_address, s.size
    return None, 0


def get_segment_layout(binary):
    """Return list of (name, vaddr, vsize, fsize) for each segment."""
    return [(s.name, s.virtual_address, s.virtual_size, s.file_size)
            for s in binary.segments]


def is_executable_section(seg, sect):
    return seg == "__TEXT" and sect in ("__text", "__stubs")


def emit_progbits_with_events(add, elf_name, flags, sect_type,
                               start_label, blob_filename, total_size,
                               events, default_export_type):
    """Emit a PROGBITS section interleaving .incbin chunks with events.

    events: sorted list of (offset, kind, data):
      kind="label", data=name     -> .globl/.type/label (no byte consumed)
      kind="symref", data=sym     -> .quad sym (replaces 8 bytes)
      kind="rebase", data=(L,off) -> .quad L + off (replaces 8 bytes)
    """
    add(f".section {elf_name}, {flags}, {sect_type}")
    add(f".balign 1")
    add(f"{start_label}:")

    cursor = 0
    for off, kind, data in events:
        if off > cursor:
            add(f'.incbin "{blob_filename}", {cursor:#x}, {off - cursor:#x}')
            cursor = off
        if kind == "label":
            name = data
            add(f".globl {name}")
            add(f".type {name}, {default_export_type}")
            add(f"{name}:")
        elif kind == "symref":
            sym = data
            if sym:
                add(f".quad {sym}")
            else:
                add(f".quad 0  /* unresolved binding */")
            cursor = off + 8
        elif kind == "rebase":
            target_label, target_off = data
            add(f".quad {target_label} + {target_off:#x}")
            cursor = off + 8

    if cursor < total_size:
        add(f'.incbin "{blob_filename}", {cursor:#x}, {total_size - cursor:#x}')


def emit_nobits_with_events(add, elf_name, flags, start_label, total_size,
                             events, default_export_type):
    """Emit a NOBITS section with label events (no byte payloads)."""
    add(f".section {elf_name}, {flags}, @nobits")
    add(f".balign 1")
    add(f"{start_label}:")

    cursor = 0
    for off, kind, data in events:
        if kind != "label":
            continue
        if off > cursor:
            add(f".zero {off - cursor:#x}")
            cursor = off
        name = data
        add(f".globl {name}")
        add(f".type {name}, {default_export_type}")
        add(f"{name}:")
    if cursor < total_size:
        add(f".zero {total_size - cursor:#x}")


def emit_assembly(binary, exports, imports, bindings_by_section,
                  sections, sections_dir, asm_path):
    """Emit the assembly stub. Exports go INSIDE their containing sections so
    symbols are section-relative and ld puts them in .dynsym."""
    lines = []
    add = lines.append

    add(f"/* macho2elf generated stub */")
    add(f"/* Source dylib imagebase: {binary.imagebase} */")
    add("")

    add("/* Mark stack as non-executable (otherwise loader rejects the .so) */")
    add('.section .note.GNU-stack, "", @progbits')
    add("")

    add("/* === IMPORTS (declared extern; linker resolves) === */")
    for imp in imports:
        add(f".extern {imp}")
    add("")

    # Build per-section event lists from exports + bindings + rebases
    events_by_section = {}  # (seg, sect) -> [(offset, kind, data), ...]

    # Bindings (symbol refs in data sections)
    for (seg, sect), bindings in bindings_by_section.items():
        for off, sym in bindings:
            events_by_section.setdefault((seg, sect), []).append((off, "symref", sym))

    # Exports (labels)
    for name, vaddr in exports:
        for s in binary.sections:
            if s.virtual_address <= vaddr < s.virtual_address + s.size:
                off = vaddr - s.virtual_address
                events_by_section.setdefault((s.segment_name, s.name), []).append(
                    (off, "label", name))
                break

    # Rebases (internal pointers — need R_X86_64_RELATIVE at load time)
    # Build (seg, sect) -> elf_section_label map
    macho_to_elf_label = {}
    for seg, sect, elf_name, _, _ in SECTION_LAYOUT:
        macho_to_elf_label[(seg, sect)] = f"{elf_name}_start"
    macho_to_elf_label[("__DATA_CONST", "__got")] = ".m2e_got_start"
    macho_to_elf_label[("__DATA", "__common")] = ".m2e_common_start"
    macho_to_elf_label[("__DATA", "__bss")] = ".m2e_bss_start"

    # Track addresses already covered by b.bindings so we don't double-emit.
    # b.bindings only covers EXTERNAL bindings (libSystem/libc++). Other relocs
    # (has_symbol=True or False) are all target-based fixups — has_symbol just
    # means LIEF knows a name for that slot, not that it's a different kind.
    binding_addrs = {bi.address for bi in binary.bindings}

    rebases_emitted = 0
    rebases_dropped = 0
    for r in binary.relocations:
        if r.address in binding_addrs:
            continue
        site_seg = site_sect = None
        site_off = None
        for s in binary.sections:
            if s.virtual_address <= r.address < s.virtual_address + s.size:
                site_seg, site_sect = s.segment_name, s.name
                site_off = r.address - s.virtual_address
                break
        if site_seg is None:
            rebases_dropped += 1
            continue
        tgt = r.target
        tgt_seg = tgt_sect = None
        tgt_off = None
        for s in binary.sections:
            if s.virtual_address <= tgt < s.virtual_address + s.size:
                tgt_seg, tgt_sect = s.segment_name, s.name
                tgt_off = tgt - s.virtual_address
                break
        if tgt_seg is None:
            rebases_dropped += 1
            continue
        tgt_label = macho_to_elf_label.get((tgt_seg, tgt_sect))
        if tgt_label is None:
            rebases_dropped += 1
            continue
        events_by_section.setdefault((site_seg, site_sect), []).append(
            (site_off, "rebase", (tgt_label, tgt_off)))
        rebases_emitted += 1
    sym_relocs_emitted = 0  # legacy counter; kept for diag-line compat

    # Sort and deduplicate (in case binding+rebase land on same slot — bindings win)
    for k in events_by_section:
        # Stable sort: order is (offset, kind_priority). Label first, then symref over rebase.
        prio = {"label": 0, "symref": 1, "rebase": 2}
        events_by_section[k].sort(key=lambda e: (e[0], prio.get(e[1], 9)))
        # Dedup: if multiple events at same offset & kind, keep first
        seen = set()
        deduped = []
        for ev in events_by_section[k]:
            key = (ev[0], ev[1])
            if key in seen and ev[1] != "label":
                continue
            seen.add(key)
            deduped.append(ev)
        events_by_section[k] = deduped

    add(f"/* rebases: {rebases_emitted} emitted, {sym_relocs_emitted} internal-symbol relocs, {rebases_dropped} dropped */")
    add("")

    # --- PROGBITS sections -----------------------------------------------
    for seg, sect, elf_name, flags, sect_type in SECTION_LAYOUT:
        key = (seg, sect)
        if key not in sections:
            continue
        meta = sections[key]
        if meta["file"] is None or meta["size"] == 0:
            continue
        events = events_by_section.get(key, [])
        export_type = "@function" if is_executable_section(seg, sect) else "@object"
        add(f"/* === {seg},{sect} -> {elf_name} (vaddr={meta['vaddr']:#x} size={meta['size']:#x} events={len(events)}) === */")
        emit_progbits_with_events(
            add, elf_name, flags, sect_type,
            f"{elf_name}_start", meta["file"].name, meta["size"],
            events, export_type)
        add("")

    # --- __DATA_CONST,__got (pure symbol table, no raw bytes) ------------
    # The GOT can hold two kinds of 8-byte entries:
    #   - External bindings: pointer to a symbol the loader resolves at
    #     load time (libc/libc++/...). We collected these as `bindings`
    #     above and emit them as `.quad <sym>` so the static linker can
    #     generate the right GLOB_DAT / JUMP_SLOT relocation.
    #   - Internal rebases: pointer to an address inside this dylib.
    #     These were registered in events_by_section as "rebase" events.
    #     We must emit them as `.quad <elf_label> + offset` so the static
    #     linker generates an R_X86_64_RELATIVE that re-bases the pointer
    #     at load time.
    #
    # The earlier implementation only walked `got_bindings` and zero-
    # filled the gaps, which dropped every rebase. For Apple's
    # jpnrom.so (and any other language module with internal pointers in
    # its GOT) that produced NULL pointers in __const that the language
    # code crashes on the first time it dereferences them.
    got_vaddr, got_size = get_section_vaddr(binary, "__DATA_CONST", "__got")
    if got_vaddr is not None and got_size > 0:
        got_events = events_by_section.get(("__DATA_CONST", "__got"), [])
        # Merge bindings (which weren't added to events_by_section because
        # we wanted to keep the symref/rebase split for normal sections)
        # with the rebase events. Sort by offset; if both kinds claim the
        # same slot the binding wins (Mach-O may report both, but the
        # binding is the canonical external link).
        merged = {}
        for off, sym_name in bindings_by_section.get(("__DATA_CONST", "__got"), []):
            merged[off] = ("symref", sym_name)
        for off, kind, data in got_events:
            if kind == "rebase" and off not in merged:
                merged[off] = ("rebase", data)
        items = sorted(merged.items())
        n_sym = sum(1 for _, (k, _) in items if k == "symref")
        n_reb = sum(1 for _, (k, _) in items if k == "rebase")
        add(f"/* === __DATA_CONST,__got -> .m2e_got (vaddr={got_vaddr:#x} size={got_size:#x} bindings={n_sym} rebases={n_reb}) === */")
        add(f".section .m2e_got, \"aw\", @progbits")
        add(f".balign 8")
        add(f".m2e_got_start:")
        prev_off = 0
        for off, (kind, data) in items:
            if off > prev_off:
                add(f".zero {off - prev_off}")
            if kind == "symref":
                add(f".quad {data}" if data else ".quad 0")
            else:  # rebase
                tgt_label, tgt_off = data
                add(f".quad {tgt_label} + {tgt_off:#x}")
            prev_off = off + 8
        if got_size > prev_off:
            add(f".zero {got_size - prev_off}")
        add("")

    # --- NOBITS sections -------------------------------------------------
    common_vaddr, common_size = get_section_vaddr(binary, "__DATA", "__common")
    if common_size > 0:
        events = events_by_section.get(("__DATA", "__common"), [])
        add(f"/* === __DATA,__common -> .m2e_common (vaddr={common_vaddr:#x} size={common_size:#x} events={len(events)}) === */")
        emit_nobits_with_events(add, ".m2e_common", '"aw"',
                                  ".m2e_common_start", common_size,
                                  events, "@object")
        add("")

    bss_vaddr, bss_size = get_section_vaddr(binary, "__DATA", "__bss")
    if bss_size > 0:
        events = events_by_section.get(("__DATA", "__bss"), [])
        add(f"/* === __DATA,__bss -> .m2e_bss (vaddr={bss_vaddr:#x} size={bss_size:#x} events={len(events)}) === */")
        emit_nobits_with_events(add, ".m2e_bss", '"aw"',
                                  ".m2e_bss_start", bss_size,
                                  events, "@object")
        add("")

    # --- .init_array (run Mach-O __init_offsets initializers at load) ----
    # Mach-O __init_offsets format: array of 4-byte image-relative offsets to
    # initializer functions (C++ static constructors). ld.so runs .init_array
    # before any other code can use the library — exactly what we need.
    for s in binary.sections:
        if s.segment_name == "__TEXT" and s.name == "__init_offsets" and s.size > 0:
            raw = bytes(s.content)
            init_count = s.size // 4
            offsets = [struct.unpack("<I", raw[i*4:i*4+4])[0] for i in range(init_count)]
            add(f"/* === .init_array (from {init_count} entries in __init_offsets) === */")
            add(f".section .init_array, \"aw\", @init_array")
            add(f".balign 8")
            for init_idx, off in enumerate(offsets):
                # Find which section the function lives in and emit a
                # section-relative reference so ld generates R_X86_64_RELATIVE.
                tgt_seg = tgt_sect = None
                tgt_inner = None
                for ts in binary.sections:
                    if ts.virtual_address <= off < ts.virtual_address + ts.size:
                        tgt_seg, tgt_sect = ts.segment_name, ts.name
                        tgt_inner = off - ts.virtual_address
                        break
                if tgt_seg is None:
                    add(f"/* WARNING: init offset {off:#x} not in any section */")
                    add(f".quad 0")
                    continue
                tgt_label = macho_to_elf_label.get((tgt_seg, tgt_sect), None)
                if tgt_label is None:
                    add(f"/* WARNING: init offset {off:#x} in {tgt_seg},{tgt_sect} has no ELF label */")
                    add(f".quad 0")
                    continue
                add(f".quad {tgt_label} + {tgt_inner:#x}  /* init #{init_idx} */")
            add("")
            break

    # --- Tally -----------------------------------------------------------
    total_labels = sum(1 for evs in events_by_section.values() for e in evs if e[1] == "label")
    total_symrefs = sum(1 for evs in events_by_section.values() for e in evs if e[1] == "symref")

    with open(asm_path, "w") as f:
        f.write("\n".join(lines))

    return total_labels, total_symrefs


def get_section_vaddr_size(binary, seg, sect):
    """Return size of a given Mach-O section, or None if absent."""
    for s in binary.sections:
        if s.segment_name == seg and s.name == sect:
            return s.size
    return None


# Reverse lookup of ELF section label -> (seg, sect) for use in dyn_base calc
_ELF_TO_MACHO = None
def _macho_for_label(elf_name):
    global _ELF_TO_MACHO
    if _ELF_TO_MACHO is None:
        _ELF_TO_MACHO = {f"{e}": (s, n) for s, n, e, _, _ in SECTION_LAYOUT}
        _ELF_TO_MACHO[".m2e_got"] = ("__DATA_CONST", "__got")
        _ELF_TO_MACHO[".m2e_common"] = ("__DATA", "__common")
        _ELF_TO_MACHO[".m2e_bss"] = ("__DATA", "__bss")
    return _ELF_TO_MACHO.get(elf_name, ("", ""))


def emit_linker_script(binary, sections, lds_path, arch_cfg=None):
    """Generate a linker script that pins each section at its Mach-O vaddr."""
    lines = []
    add = lines.append

    segments = get_segment_layout(binary)

    add("/* macho2elf generated linker script */")
    if arch_cfg:
        add(f"OUTPUT_FORMAT({arch_cfg['elf_format']})")
        add(f"OUTPUT_ARCH({arch_cfg['elf_arch']})")
    else:
        add("OUTPUT_FORMAT(elf64-x86-64)")
        add("OUTPUT_ARCH(i386:x86-64)")
    add("")
    add("PHDRS {")
    add("    text     PT_LOAD       FLAGS(5);  /* R-X (Mach-O __TEXT) */")
    add("    rwdata   PT_LOAD       FLAGS(6);  /* RW- (Mach-O __DATA*) */")
    add("    auxtext  PT_LOAD       FLAGS(5);  /* R-X (linker-auto .plt/.text from stubs.c) */")
    add("    dynamic  PT_DYNAMIC    FLAGS(6);")
    add("    gnustack PT_GNU_STACK  FLAGS(6);")
    add("}")
    add("")
    add("SECTIONS {")

    # We need each section placed at its original Mach-O vaddr.
    # Use position-dependent `. = vaddr` then place the section.
    # Provide _start labels for offset calculation in the asm stub.

    add("    . = 0;")
    add("")

    # Collect all sections to place, sorted by vaddr
    placements = []  # (vaddr, elf_name, phdr, nobits)

    for seg, sect, elf_name, _, _ in SECTION_LAYOUT:
        key = (seg, sect)
        if key not in sections:
            continue
        meta = sections[key]
        if meta["size"] == 0:
            continue
        # Everything outside __TEXT goes into rwdata (matches Mach-O's
        # __DATA_CONST-then-mprotect behavior using ELF .data.rel.ro
        # semantics). Keeping __DATA_CONST in a separate "rodata" PT_LOAD
        # causes segment overlap because Mach-O packs __DATA_CONST and
        # __DATA at adjacent vaddrs.
        phdr = "text" if seg == "__TEXT" else "rwdata"
        placements.append((meta["vaddr"], elf_name, phdr, False))

    got_vaddr, got_size = get_section_vaddr(binary, "__DATA_CONST", "__got")
    if got_vaddr is not None and got_size > 0:
        placements.append((got_vaddr, ".m2e_got", "rwdata", False))

    common_vaddr, common_size = get_section_vaddr(binary, "__DATA", "__common")
    if common_size > 0:
        placements.append((common_vaddr, ".m2e_common", "rwdata", True))

    bss_vaddr, bss_size = get_section_vaddr(binary, "__DATA", "__bss")
    if bss_size > 0:
        placements.append((bss_vaddr, ".m2e_bss", "rwdata", True))

    # Sort by vaddr so the script is monotonic
    placements.sort(key=lambda p: p[0])

    for vaddr, elf_name, phdr, nobits in placements:
        add(f"    . = {vaddr:#x};")
        add(f"    {elf_name}_start = .;")
        if nobits:
            add(f"    {elf_name} (NOLOAD) : {{ KEEP(*({elf_name})) }} :{phdr}")
        else:
            add(f"    {elf_name} : {{ KEEP(*({elf_name})) }} :{phdr}")

    # Compute end of all Mach-O placements + page-align, then push linker-
    # generated dynamic sections that high so they never overlap.
    max_end = max((vaddr + (get_section_vaddr_size(binary, *_macho_for_label(elf_name)) or 0)
                   for vaddr, elf_name, _, _ in placements), default=0x100000)
    # Round up to next 64KB boundary for safety
    dyn_base = (max_end + 0xffff) & ~0xffff
    add("")
    add(f"    . = {dyn_base:#x};")
    add("    .dynsym             : { *(.dynsym) } :rwdata")
    add("    .dynstr             : { *(.dynstr) } :rwdata")
    add("    .gnu.hash           : { *(.gnu.hash) } :rwdata")
    add("    .gnu.version        : { *(.gnu.version) } :rwdata")
    add("    .gnu.version_r      : { *(.gnu.version_r) } :rwdata")
    add("    .rela.dyn           : { *(.rela.dyn) } :rwdata")
    add("    .rela.plt           : { *(.rela.plt) } :rwdata")
    add("    .dynamic            : { *(.dynamic) } :rwdata :dynamic")
    add("    .note.gnu.build-id  : { *(.note.gnu.build-id) } :rwdata")
    add("    .got                : { *(.got) } :rwdata")
    add("    .got.plt            : { *(.got.plt) } :rwdata")
    # Place auto-generated text sections (from stubs.c) AFTER all rwdata in
    # a SEPARATE PT_LOAD segment (auxtext) so the main text segment doesn't
    # need to span all the way out here. Big gap to make sure rwdata's
    # memsize (which includes .bss zero-fill) doesn't overlap us.
    add("    . = ALIGN(0x100000);")
    add("    . = . + 0x100000;")
    add("    .plt                : { *(.plt) } :auxtext")
    add("    .plt.got            : { *(.plt.got) } :auxtext")
    add("    .plt.sec            : { *(.plt.sec) } :auxtext")
    add("    .text               : { *(.text) } :auxtext")
    add("    .rodata             : { *(.rodata*) } :auxtext")
    add("    .eh_frame           : { *(.eh_frame) } :rwdata")
    add("    .eh_frame_hdr       : { *(.eh_frame_hdr) } :rwdata")
    add("    .bss                : { *(.bss) *(COMMON) } :rwdata")

    add("")
    add("    .note.GNU-stack     : { *(.note.GNU-stack) } :gnustack")
    add("    /DISCARD/ : { *(.comment) *(.note.gnu.property) }")
    add("}")

    with open(lds_path, "w") as f:
        f.write("\n".join(lines))


def emit_runtime_stubs(stubs_path):
    """Emit C stubs for Darwin-specific symbols we need to provide.

    Currently just _DefaultRuneLocale — a stub struct that mimics enough of
    Darwin's _RuneLocale layout to keep inlined ctype checks happy.
    """
    content = r"""// macho2elf runtime stubs — Darwin-specific symbols with no Linux equivalent.

#include <stddef.h>
#include <stdint.h>
#include <ctype.h>

// __DefaultRuneLocale stub. Apple's libc <ctype.h> inlines isXXX(c) checks
// against this struct. We provide a buffer to satisfy GOT bindings; the
// actual classification is done via __maskrune below.
struct { char placeholder[8192]; } _macho2elf_rune_locale_stub = {{0}};

// __stack_chk_guard — glibc keeps the real canary in TLS (fs:0x28) and
// doesn't export the symbol. Our converted Mach-O references it via GOT,
// so we provide it. Apple's convention is to have the high byte be 0x00
// (so string-based overflow attacks include a null terminator). Match that.
uintptr_t __stack_chk_guard = 0x00cafebabedeadbeULL;

// __maskrune(c, mask) — Darwin's runtime ctype lookup. Returns nonzero if
// character c has any of the bits in mask set in __runetype[c]. Apple's
// inline ctype.h macros call this. Map Darwin's _CTYPE_* bits to glibc's
// ctype functions for ASCII (which is all the engine ever passes).
//
// Darwin _CTYPE_* values (from <_ctype.h>):
#define DARWIN_CTYPE_A    0x00000100L  // Alpha
#define DARWIN_CTYPE_C    0x00000200L  // Cntrl
#define DARWIN_CTYPE_D    0x00000400L  // Digit
#define DARWIN_CTYPE_G    0x00000800L  // Graph
#define DARWIN_CTYPE_L    0x00001000L  // Lower
#define DARWIN_CTYPE_P    0x00002000L  // Punct
#define DARWIN_CTYPE_S    0x00004000L  // Space
#define DARWIN_CTYPE_U    0x00008000L  // Upper
#define DARWIN_CTYPE_X    0x00010000L  // X digit
#define DARWIN_CTYPE_B    0x00020000L  // Blank
#define DARWIN_CTYPE_R    0x00040000L  // Print

unsigned long __maskrune(int c, unsigned long mask) {
    if (c < 0 || c > 127) return 0;  // engine only passes ASCII
    unsigned long r = 0;
    if (isalpha(c))  r |= DARWIN_CTYPE_A;
    if (iscntrl(c))  r |= DARWIN_CTYPE_C;
    if (isdigit(c))  r |= DARWIN_CTYPE_D;
    if (isgraph(c))  r |= DARWIN_CTYPE_G;
    if (islower(c))  r |= DARWIN_CTYPE_L;
    if (ispunct(c))  r |= DARWIN_CTYPE_P;
    if (isspace(c))  r |= DARWIN_CTYPE_S;
    if (isupper(c))  r |= DARWIN_CTYPE_U;
    if (isxdigit(c)) r |= DARWIN_CTYPE_X;
    if (isblank(c))  r |= DARWIN_CTYPE_B;
    if (isprint(c))  r |= DARWIN_CTYPE_R;
    return r & mask;
}

"""
    with open(stubs_path, "w") as f:
        f.write(content)


ARCH_CONFIG = {
    "x86_64": {
        "elf_format": "elf64-x86-64",
        "elf_arch":   "i386:x86-64",
        "gcc":        "gcc",
        # x86_64 libc++/libc++abi typically resolves cleanly at link time on host
        "link_libs":  ["-lc", "-lm", "-lpthread", "-ldl",
                       "-l:libc++.so.1", "-l:libc++abi.so.1"],
        "page_size":  0x1000,
    },
    "arm64": {
        "elf_format": "elf64-littleaarch64",
        "elf_arch":   "aarch64",
        "gcc":        "aarch64-linux-gnu-gcc",
        # aarch64 sysroot lacks libc++ — generate empty stub .so files at
        # build time so the linker can satisfy DT_NEEDED; ld.so resolves real
        # symbols at load time using the target system's libc++.
        "link_libs":  ["-Wl,--unresolved-symbols=ignore-all",
                       "-lc", "-lm", "-lpthread", "-ldl",
                       "{STUB}libc++.so.1", "{STUB}libc++abi.so.1"],
        # Stub libs to generate (soname -> file). Generated at build time.
        "stub_libs":  ["libc++.so.1", "libc++abi.so.1"],
        # Apple arm64 dylibs use 16KB segment alignment; honor that.
        "page_size":  0x4000,
    },
}


def detect_arch(binary):
    """Return 'x86_64' or 'arm64' for the parsed Mach-O binary."""
    cpu = str(binary.header.cpu_type)
    if "X86_64" in cpu:
        return "x86_64"
    if "ARM64" in cpu:
        return "arm64"
    raise RuntimeError(f"Unsupported CPU type: {cpu}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", help="Path to Mach-O dylib slice (x86_64 or arm64)")
    ap.add_argument("-o", "--output", required=True, help="Output ELF .so path")
    ap.add_argument("--workdir", default=None, help="Intermediate files directory")
    ap.add_argument("--cc", default=None,
                    help="Override the C compiler to link with (defaults are 'gcc' for "
                         "x86_64 and 'aarch64-linux-gnu-gcc' for arm64).")
    ap.add_argument("--no-link", action="store_true", help="Stop after generating asm/lds")
    args = ap.parse_args()

    input_path = Path(args.input).resolve()
    output_path = Path(args.output).resolve()
    workdir = Path(args.workdir).resolve() if args.workdir else output_path.parent / f".m2e_{input_path.stem}"
    workdir.mkdir(parents=True, exist_ok=True)
    sections_dir = workdir / "sections"

    print(f"[macho2elf] input:    {input_path}")
    print(f"[macho2elf] output:   {output_path}")
    print(f"[macho2elf] workdir:  {workdir}")

    fat = lief.MachO.parse(str(input_path))
    if not fat or len(fat) == 0:
        print(f"ERROR: failed to parse {input_path}", file=sys.stderr)
        sys.exit(1)
    if len(fat) > 1:
        slices = [str(fat.at(i).header.cpu_type) for i in range(len(fat))]
        print(f"ERROR: {input_path} is a fat binary with {len(fat)} slices ({', '.join(slices)}).",
              file=sys.stderr)
        print("       Extract a single-arch slice first, e.g.",
              file=sys.stderr)
        print(f"         llvm-lipo -extract x86_64 {input_path} -output /tmp/slice.dylib",
              file=sys.stderr)
        sys.exit(1)
    binary = fat.at(0)
    arch = detect_arch(binary)
    arch_cfg = ARCH_CONFIG[arch]
    print(f"[macho2elf] arch:     {arch}")

    # 1) Extract section blobs
    sections = extract_sections(binary, sections_dir)
    print(f"[macho2elf] extracted {sum(1 for v in sections.values() if v['file']):d} non-empty sections")

    # 2) Collect exports
    exports = collect_exports(binary)
    print(f"[macho2elf] exports: {len(exports)}")

    # 3) Collect imports + bindings per section
    imports, bindings_by_section = collect_bindings_per_section(binary)
    total_bindings = sum(len(v) for v in bindings_by_section.values())
    print(f"[macho2elf] imports: {len(imports)}, total binding sites: {total_bindings}")
    for (seg, sect), bs in bindings_by_section.items():
        print(f"    {seg},{sect}: {len(bs)} bindings")

    # 4) Emit assembly
    asm_path = workdir / "stub.s"
    emitted, skipped = emit_assembly(binary, exports, imports, bindings_by_section,
                                      sections, sections_dir, asm_path)
    print(f"[macho2elf] assembly: {asm_path}  ({emitted} exports emitted, {skipped} skipped)")

    # 5) Emit linker script
    lds_path = workdir / "link.lds"
    emit_linker_script(binary, sections, lds_path, arch_cfg=arch_cfg)
    print(f"[macho2elf] linker script: {lds_path}")

    # 6) Emit runtime stubs
    stubs_path = workdir / "stubs.c"
    emit_runtime_stubs(stubs_path)
    print(f"[macho2elf] runtime stubs: {stubs_path}")

    if args.no_link:
        print("[macho2elf] --no-link set; stopping before invocation of gcc")
        return

    # 7) Build
    import subprocess
    print("[macho2elf] building...")

    cc = args.cc or arch_cfg["gcc"]

    # Generate empty stub shared objects for libraries the cross-sysroot lacks
    # (arm64 typically needs this for libc++/libc++abi).
    stub_dir = workdir / "stub_libs"
    if arch_cfg.get("stub_libs"):
        stub_dir.mkdir(parents=True, exist_ok=True)
        empty_c = stub_dir / "_empty.c"
        empty_c.write_text("/* stub */\n")
        empty_o = stub_dir / "_empty.o"
        subprocess.check_call([cc, "-c", "-fPIC", str(empty_c), "-o", str(empty_o)])
        for soname in arch_cfg["stub_libs"]:
            stub_so = stub_dir / soname
            subprocess.check_call([
                cc, "-shared", "-nostdlib", "-fPIC",
                f"-Wl,-soname,{soname}",
                str(empty_o),
                "-o", str(stub_so),
            ])

    # Compile stubs.c (arch-targeted)
    stubs_o = workdir / "stubs.o"
    subprocess.check_call([cc, "-c", "-fPIC", "-O2", str(stubs_path), "-o", str(stubs_o)])

    # Assemble the stub (cwd = sections dir so .incbin paths resolve)
    stub_o = workdir / "stub.o"
    subprocess.check_call([cc, "-c", "-fPIC", "-xassembler", str(asm_path), "-o", str(stub_o)],
                          cwd=sections_dir)

    # Link. For x86_64 we resolve libc++ at link time (libc++.so.1 is on the
    # host); for arm64 we leave C++ symbols unresolved at link time and let
    # ld.so resolve them at load time on the target system.
    # Expand {STUB} placeholders in link_libs with our generated stub paths
    link_libs = []
    for lib in arch_cfg["link_libs"]:
        if lib.startswith("{STUB}"):
            soname = lib[len("{STUB}"):]
            link_libs.append(str(stub_dir / soname))
        else:
            link_libs.append(lib)

    link_cmd = [
        cc, "-shared", "-fPIC", "-nostdlib",
        f"-Wl,-soname,{output_path.name}",
        "-Wl,-z,noexecstack",
        f"-Wl,-z,max-page-size={arch_cfg['page_size']:#x}",
        "-T", str(lds_path),
        str(stub_o), str(stubs_o),
        *link_libs,
        "-o", str(output_path),
    ]
    print(f"[macho2elf] {' '.join(link_cmd)}")
    subprocess.check_call(link_cmd)
    print(f"[macho2elf] SUCCESS: {output_path}")


if __name__ == "__main__":
    main()
