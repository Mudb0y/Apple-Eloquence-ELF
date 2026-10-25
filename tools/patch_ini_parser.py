#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
patch_ini_parser.py -- fix Apple's Eloquence ini-parser section-skipping bug.

Apple's converted eci.so carries a bug in IniFileReader::goEndSection: it scans
forward until a blank line, swallowing any [X.Y] section header in its path.
Because IniFileReader::getString leaves the cursor at the current section's
trailing blank line, every getNextSection call drops exactly one section. The
language table built by InitializeStandardVoices therefore only registers every
other eci.ini section, and eciSetParam(eciLanguageDialect, ...) returns -1 for
the dropped languages -- speech silently falls back to the initial dialect
(usually en-US). See docs/05-troubleshooting.md.

The fix: getNextSection must not call goEndSection; locating the next '[' via
stringSearch from the current cursor is already correct. This tool NOPs the
call by replacing it with "mov $1,%al" + padding on x86_64 (the 0 return-check
after the call must see non-zero), or "mov w0,#1" on aarch64.

Locating the call site is deliberately indirect so it survives re-layouts
between Apple engine versions:
  1. resolve IniFileReader::getNextSection and IniFileReader::goEndSection
     through .dynsym (Apple keeps C++ names on all builds we ship);
  2. scan getNextSection's body for a call instruction whose target is
     goEndSection;
  3. verify the exact bytes before writing, and refuse anything unexpected.

Usage:
  python3 tools/patch_ini_parser.py [--check] <file.so> [more.so ...]

Exit codes: 0 = all files patched (or already patched), 1 = usage/ELF error,
2 = pattern not found or unexpected bytes (do NOT ignore in CI).
"""

import argparse
import struct
import sys

SYM_GET_NEXT = b"_ZN13IniFileReader14getNextSectionEPl"
SYM_GO_END = b"_ZN13IniFileReader12goEndSectionEPl"

# x86_64: "call rel32" targeting goEndSection -> mov $1,%al ; nop nop nop
X86_CALL_OP, X86_CALL_LEN = 0xE8, 5
X86_REPLACEMENT = bytes.fromhex("b001909090")

# aarch64: "bl imm26" targeting goEndSection -> mov w0,#1
# NOTE: located but not yet validated against a real aarch64 engine build;
# the script reports it plainly so CI can gate on verified arches only.
ARM64_BL_MASK, ARM64_BL_OP = 0xFC000000, 0x94000000
ARM64_MOV_W0_1 = struct.pack("<I", 0x52800020)


class Elf:
    """Minimal ELF64 reader: section headers + .dynsym/.dynstr + vaddr->offset."""

    def __init__(self, data: bytearray):
        if data[:4] != b"\x7fELF":
            raise ValueError("not an ELF file")
        self.is64 = data[4] == 2
        if not self.is64:
            raise ValueError("ELF32 not supported")
        self.little = data[5] == 1
        if not self.little:
            raise ValueError("big-endian not supported")
        self.machine = struct.unpack_from("<H", data, 18)[0]  # 62=x86_64 183=aarch64
        self.data = data
        e_shoff, = struct.unpack_from("<Q", data, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
        self.sections = []
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            (name, stype, flags, addr, offset, size) = struct.unpack_from(
                "<IIQQQQ", data, off)
            self.sections.append(
                {"name": name, "type": stype, "addr": addr,
                 "offset": offset, "size": size})
        shstr = self.sections[e_shstrndx]
        for s in self.sections:
            end = data.index(b"\x00", shstr["offset"] + s["name"])
            s["sname"] = data[shstr["offset"] + s["name"]:end].decode()
        self._sym = {}
        for s in self.sections:
            if s["sname"] in (".dynsym", ".symtab"):
                strtab = None
                for t in self.sections:
                    if t["sname"] == (".dynstr" if s["sname"] == ".dynsym"
                                      else ".strtab"):
                        strtab = t
                if not strtab:
                    continue
                entsize = 24
                for off in range(s["offset"], s["offset"] + s["size"], entsize):
                    (st_name, _info, _other, st_shndx, st_value,
                     st_size) = struct.unpack_from("<IBBHQQ", data, off)
                    if st_name == 0:
                        continue
                    end = data.index(b"\x00", strtab["offset"] + st_name)
                    nm = bytes(data[strtab["offset"] + st_name:end])
                    self._sym[nm] = (st_value, st_size)
                break

    def symbol(self, name: bytes):
        """Return (vaddr, size) of a dynamic symbol, or None."""
        return self._sym.get(name)

    def vaddr_to_offset(self, vaddr: int):
        """Map a virtual address to a file offset via section headers."""
        for s in self.sections:
            if (s["type"] in (1, 8) and s["addr"] != 0
                    and s["addr"] <= vaddr < s["addr"] + s["size"]):
                return s["offset"] + (vaddr - s["addr"])
        raise ValueError(f"vaddr {vaddr:#x} not in any section")

    def symbol_end(self, vaddr: int):
        """Conservative end of a function: start of the next known symbol."""
        candidates = [v for v, _ in self._sym.values() if v > vaddr]
        return min(candidates) if candidates else vaddr + 0x400


def is_already_patched(elf: Elf):
    """True if getNextSection already carries the x86_64 replacement sequence
    (mov $1,%al ; nops) followed by the test of its return."""
    if elf.machine != 62:
        return False
    nxt = elf.symbol(SYM_GET_NEXT)
    if not nxt:
        return False
    start = elf.vaddr_to_offset(nxt[0])
    end = elf.vaddr_to_offset(elf.symbol_end(nxt[0]))
    body = bytes(elf.data[start:end])
    marker = bytes.fromhex("b00190909084c0")  # mov;nops;nops;test %al,%al
    return body.find(marker) != -1


def find_call_site(elf: Elf):
    """Locate (file_offset, length, replacement) of the goEndSection call
    inside getNextSection, or (None, reason) on failure."""
    machine = elf.machine
    nxt = elf.symbol(SYM_GET_NEXT)
    goend = elf.symbol(SYM_GO_END)
    if not nxt or not goend:
        return None, "symbols IniFileReader::{getNextSection,goEndSection} not found"
    nvaddr, _ = nxt
    gvaddr, _ = goend
    start = elf.vaddr_to_offset(nvaddr)
    end = elf.vaddr_to_offset(elf.symbol_end(nvaddr))
    body = elf.data[start:end]

    if machine == 62:  # x86_64
        for i in range(len(body) - X86_CALL_LEN):
            if body[i] != X86_CALL_OP:
                continue
            rel = struct.unpack_from("<i", body, i + 1)[0]
            # call target = address of next instruction + rel32
            if nvaddr + i + X86_CALL_LEN + rel == gvaddr:
                off = start + i
                expect = struct.pack("<B", X86_CALL_OP) + struct.pack(
                    "<i", rel)
                return (off, X86_CALL_LEN, X86_REPLACEMENT, expect,
                        "x86_64: call goEndSection -> mov $1,%al; nops")
        return None, ("call to goEndSection not found inside getNextSection "
                      "(engine changed? refusing to guess)")

    if machine == 183:  # aarch64
        for i in range(0, len(body) - 3, 4):
            (word,) = struct.unpack_from("<I", body, i)
            if word & ARM64_BL_MASK != ARM64_BL_OP:
                continue
            imm26 = word & 0x03FFFFFF
            if imm26 & 0x02000000:
                imm26 -= 0x04000000
            if nvaddr + i + (imm26 << 2) == gvaddr:
                off = start + i
                expect = struct.pack("<I", word)
                repl = ARM64_MOV_W0_1
                return (off, 4, repl, expect,
                        "aarch64: bl goEndSection -> mov w0,#1 (UNVERIFIED "
                        "against a real aarch64 engine build)")
        return None, ("bl to goEndSection not found inside getNextSection "
                      "(aarch64 engine changed? refusing to guess)")

    return None, f"unsupported machine type {machine}"


def process(path: str, check_only: bool):
    with open(path, "rb") as fh:
        data = bytearray(fh.read())
    try:
        elf = Elf(data)
    except ValueError as exc:
        print(f"FAIL {path}: {exc}")
        return 1
    site = find_call_site(elf)
    if isinstance(site, tuple) and len(site) != 5:
        reason = site[1]
        if is_already_patched(elf):
            print(f"OK   {path}: already patched")
            return 0
        print(f"FAIL {path}: {reason}")
        return 2
    off, length, repl, expect, desc = site

    current = bytes(data[off:off + length])
    if current == repl:
        print(f"OK   {path}: already patched ({desc}) at {off:#x}")
        return 0
    if current != expect:
        print(f"FAIL {path}: unexpected bytes at {off:#x}: "
              f"{current.hex()} (expected {expect.hex()}) -- refusing")
        return 2
    if check_only:
        print(f"OK   {path}: needs patch ({desc}) at {off:#x} "
              f"[--check, not written]")
        return 0
    data[off:off + length] = repl
    with open(path, "wb") as fh:
        fh.write(bytes(data))
    print(f"OK   {path}: patched ({desc}) at {off:#x}: "
          f"{expect.hex()} -> {repl.hex()}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--check", action="store_true",
                    help="report patch status without writing")
    ap.add_argument("files", nargs="+", help="converted eci.so files")
    args = ap.parse_args()
    worst = 0
    for path in args.files:
        try:
            rc = process(path, args.check)
        except (OSError, ValueError) as exc:
            print(f"FAIL {path}: {exc}")
            rc = 1
        worst = max(worst, rc)
    return worst


if __name__ == "__main__":
    sys.exit(main())
