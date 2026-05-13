#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Per-module Mach-O relocation audit.

For a given .dylib, produce six artifacts in <workdir>:
    ground-truth.txt       -- llvm-otool + our dump_chained_fixups walker
    lief.txt               -- what LIEF reports for the same relocations
    converter.txt          -- what macho2elf.py would emit (no gcc invoked)
    diff-lief.txt          -- diff -u ground-truth.txt lief.txt
    diff-converter.txt     -- diff -u ground-truth.txt converter.txt
    summary.json           -- counts per relocation kind + lief diffs list

Usage:
    python3 tools/audit_relocs.py <dylib> --workdir <out-dir>

When `diff-converter.txt` is empty for every module audited, the
converter handles every relocation kind correctly. This is the metric
that gates Phase D's fix loop in the plan.
"""

import argparse
import json
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Dict, List

# Ensure the repo root is on sys.path so `from tools.X import ...` works
# whether this script is run as `python3 tools/audit_relocs.py` or as a module.
_REPO_ROOT = Path(__file__).resolve().parent.parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))


@dataclass
class FixupRecord:
    """Normalized cross-format relocation record."""
    site_segment: str
    site_section: str
    site_offset: int    # offset within site_section
    kind: str           # "rebase" | "bind"
    target_segment: str = ""   # for rebase
    target_section: str = ""
    target_offset: int = 0
    symbol: str = ""           # for bind
    library: str = ""

    def normalized_line(self) -> str:
        """Single-line canonical form used by all three dumps for diffing.

        The library name is intentionally omitted: it varies by source
        (LIEF reports "/usr/lib/libc++.1.dylib"; our chained-fixup walker
        emits "<resolved-later>" because resolving lib_ordinal to a name
        requires a separate pass over LC_LOAD_DYLIB commands). Library
        name is preserved in summary.json for diagnostic purposes; what
        matters for converter accuracy is whether the bind/rebase target
        is correctly identified, not which dylib we annotated it from."""
        site = f"[{self.site_segment},{self.site_section} + {self.site_offset:#06x}]"
        if self.kind == "bind":
            return f"{site}  bind     {self.symbol}"
        return (f"{site}  rebase   {self.target_segment},{self.target_section}"
                f" + {self.target_offset:#06x}")


def emit_ground_truth(dylib_path: Path) -> List[FixupRecord]:
    """Source 1: our hand-rolled walker + llvm-otool's classic-reloc table.

    Modern tvOS dylibs use chained fixups for most relocations, but a
    classic reloc table may still exist for things chained fixups don't
    cover (e.g., bindings that the linker emitted as external relocs
    instead of chained-bind slots). We merge both sources into one
    canonical record list, with chained-fixup records winning on
    conflicts (their target_offset values are more precise)."""
    records = _gather_chained_fixups(dylib_path)
    classic = _gather_classic_relocs(dylib_path)
    return _merge_dedupe(records + classic)


def _gather_chained_fixups(dylib_path):
    from tools.dump_chained_fixups import parse_dylib, ChainedRebase, ChainedBind
    try:
        result = parse_dylib(dylib_path)
    except RuntimeError:
        return []  # no LC_DYLD_CHAINED_FIXUPS in this dylib
    records = []
    for fx in result.fixups:
        site = _file_offset_to_site(fx.file_offset, dylib_path)
        if site is None:
            continue
        site_seg, site_sect, site_off = site
        if isinstance(fx, ChainedRebase):
            records.append(FixupRecord(
                site_segment=site_seg, site_section=site_sect,
                site_offset=site_off,
                kind="rebase",
                target_segment=fx.target_segment,
                target_section=fx.target_section,
                target_offset=fx.target_offset,
            ))
        else:
            records.append(FixupRecord(
                site_segment=site_seg, site_section=site_sect,
                site_offset=site_off,
                kind="bind",
                symbol=fx.symbol, library=fx.library or "?",
            ))
    return records


def _gather_classic_relocs(dylib_path):
    """Parse llvm-otool -rv. Output format varies by version but typically:

        File: <path> (architecture <arch>)
        Relocation information (__SEGMENT,__section) <N> entries
        address     pcrel length extern type    scattered symbolnum/value
        00000000    False  3      True   X86_64_RELOC_BRANCH      0x00000007 _foo
        ...

    Robustly skip any unrecognized lines; this is best-effort enrichment."""
    try:
        out = subprocess.run(
            ["llvm-otool", "-rv", str(dylib_path)],
            capture_output=True, text=True, check=False).stdout
    except FileNotFoundError:
        return []

    records = []
    cur_section = None
    for raw_line in out.splitlines():
        line = raw_line.strip()
        if not line:
            cur_section = None
            continue
        # Section header: "Relocation information (__TEXT,__text) N entries"
        if line.startswith("Relocation information ("):
            inner = line[len("Relocation information ("):]
            # Trim trailing ") N entries" or just ")"
            close = inner.find(")")
            if close < 0:
                continue
            tag = inner[:close]
            if "," in tag:
                seg, sect = tag.split(",", 1)
                cur_section = (seg.strip(), sect.strip())
            continue
        if line.startswith("address") or not cur_section:
            continue
        parts = line.split()
        # Tolerate format variation; need at least: addr, pcrel, length, extern, type, symbol
        if len(parts) < 6:
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        # Find the "extern" boolean -- typically column 3 (0-indexed)
        extern_token = parts[3].lower()
        extern = extern_token in ("true", "1")
        # Symbol/section is typically the last column
        symbol = parts[-1]
        kind = "bind" if extern else "rebase"
        records.append(FixupRecord(
            site_segment=cur_section[0], site_section=cur_section[1],
            site_offset=addr,
            kind=kind,
            symbol=symbol if extern else "",
            library="?" if extern else "",
        ))
    return records


def _merge_dedupe(records):
    """Dedupe by (site, kind). Chained-fixup records appear first in the
    merged list (because _gather_chained_fixups runs first); they win on
    conflicts because their target_offset values are more precise."""
    seen = {}
    out = []
    for r in records:
        key = (r.site_segment, r.site_section, r.site_offset, r.kind)
        if key not in seen:
            seen[key] = True
            out.append(r)
    return out


def emit_lief(dylib_path: Path) -> List[FixupRecord]:
    """Source 2: what LIEF reports via binary.relocations.

    Walks LIEF's unified relocation list and produces FixupRecords; we'll
    compare the result to ground-truth to find LIEF's gaps."""
    import lief
    b = lief.MachO.parse(str(dylib_path))[0]
    records = []
    sections = list(b.sections)
    bind_addrs = {}
    for bi in b.bindings:
        bind_addrs[bi.address] = bi

    for r in b.relocations:
        site = _vm_to_site(r.address, sections)
        if site is None:
            continue
        site_seg, site_sect, site_off = site
        if r.address in bind_addrs:
            bi = bind_addrs[r.address]
            try:
                sym_name = bi.symbol.name if bi.has_symbol else "<no_sym>"
            except (AttributeError, RuntimeError):
                sym_name = "<no_sym>"
            try:
                lib_name = bi.library.name if hasattr(bi, "library") and bi.library else "?"
            except (AttributeError, RuntimeError):
                lib_name = "?"
            records.append(FixupRecord(
                site_segment=site_seg, site_section=site_sect,
                site_offset=site_off,
                kind="bind",
                symbol=sym_name, library=lib_name,
            ))
        else:
            target = _vm_to_site(r.target, sections)
            if target is None:
                continue
            tgt_seg, tgt_sect, tgt_off = target
            records.append(FixupRecord(
                site_segment=site_seg, site_section=site_sect,
                site_offset=site_off,
                kind="rebase",
                target_segment=tgt_seg, target_section=tgt_sect,
                target_offset=tgt_off,
            ))
    return records


def emit_converter(dylib_path: Path) -> List[FixupRecord]:
    """Source 3: what macho2elf.py would emit. Calls into the converter
    code without running gcc."""
    import lief
    sys.path.insert(0, str(Path(__file__).parent.parent / "macho2elf"))
    from macho2elf import collect_relocation_events

    b = lief.MachO.parse(str(dylib_path))[0]
    events = collect_relocation_events(b)

    records = []
    for (site_seg, site_sect), evlist in events.items():
        for ev in evlist:
            off, kind, data = ev[0], ev[1], ev[2]
            if kind == "rebase":
                # Tuple is (tgt_label, tgt_off, tgt_seg, tgt_sect)
                _tgt_label, tgt_off, tgt_seg, tgt_sect = data
                records.append(FixupRecord(
                    site_segment=site_seg, site_section=site_sect,
                    site_offset=off,
                    kind="rebase",
                    target_segment=tgt_seg, target_section=tgt_sect,
                    target_offset=tgt_off,
                ))
            elif kind == "symref":
                symbol, library = data
                records.append(FixupRecord(
                    site_segment=site_seg, site_section=site_sect,
                    site_offset=off,
                    kind="bind", symbol=symbol, library=library,
                ))
    return records


def _file_offset_to_site(file_off, dylib_path):
    """Resolve a file offset back to (segment, section, offset_within)."""
    import lief
    b = lief.MachO.parse(str(dylib_path))[0]
    # FAT binary: chain walker stores absolute file offsets (already includes
    # fat_offset). LIEF section offsets are slice-relative. Account for that.
    fat = getattr(b, "fat_offset", 0)
    for s in b.sections:
        sect_file_start = fat + s.offset
        if sect_file_start <= file_off < sect_file_start + s.size:
            return s.segment_name, s.name, file_off - sect_file_start
    return None


def _vm_to_site(vm_addr, sections):
    for s in sections:
        if s.virtual_address <= vm_addr < s.virtual_address + s.size:
            return s.segment_name, s.name, vm_addr - s.virtual_address
    return None


def write_dump(records: List[FixupRecord], path: Path):
    """Write a list of FixupRecords as one normalized line per record,
    sorted by site for deterministic diffs."""
    lines = sorted(r.normalized_line() for r in records)
    path.write_text("\n".join(lines) + "\n")


def diff_dumps(a_path: Path, b_path: Path, out_path: Path) -> bool:
    """Run `diff -u a b > out`. Returns True if outputs differ."""
    result = subprocess.run(
        ["diff", "-u", str(a_path), str(b_path)],
        capture_output=True, text=True)
    out_path.write_text(result.stdout)
    return result.returncode != 0


def build_summary(records, lief_records, conv_records) -> dict:
    """Count per (kind, target) tuple for each source."""
    def counts(rs):
        out = {}
        for r in rs:
            if r.kind == "rebase":
                key = f"rebase:{r.target_segment},{r.target_section}"
            else:
                key = f"bind:{r.library or '?'}"
            out[key] = out.get(key, 0) + 1
        return out
    return {
        "ground_truth": counts(records),
        "lief":        counts(lief_records),
        "converter":   counts(conv_records),
        "totals": {
            "ground_truth": len(records),
            "lief":         len(lief_records),
            "converter":    len(conv_records),
        },
    }


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("dylib", help="Path to Mach-O dylib")
    ap.add_argument("--workdir", required=True,
                    help="Output directory (created if missing)")
    args = ap.parse_args(argv[1:])

    dylib_path = Path(args.dylib)
    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)

    gt   = emit_ground_truth(dylib_path)
    lief_records = emit_lief(dylib_path)
    conv = emit_converter(dylib_path)

    write_dump(gt,   workdir / "ground-truth.txt")
    write_dump(lief_records, workdir / "lief.txt")
    write_dump(conv, workdir / "converter.txt")

    diff_dumps(workdir / "ground-truth.txt", workdir / "lief.txt",
               workdir / "diff-lief.txt")
    diff_dumps(workdir / "ground-truth.txt", workdir / "converter.txt",
               workdir / "diff-converter.txt")

    summary = build_summary(gt, lief_records, conv)
    (workdir / "summary.json").write_text(json.dumps(summary, indent=2))

    print(f"{dylib_path.name}: {len(gt)} ground-truth, "
          f"{len(lief_records)} lief, {len(conv)} converter records")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
