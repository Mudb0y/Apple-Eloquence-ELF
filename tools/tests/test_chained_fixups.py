"""Unit tests for tools/dump_chained_fixups.py.

The fixture `minimal_chained_fixup.bin` is the raw LC_DYLD_CHAINED_FIXUPS
payload extracted from vendor/tvOS-18.2/enu.dylib. We don't need to test
every relocation; we just need a smoke test that the walker decodes the
header + at least one rebase entry without throwing, and reports plausible
values.
"""

import pathlib
import pytest

from tools.dump_chained_fixups import (
    parse_chained_fixups_blob,
    ChainedRebase,
    ChainedBind,
)


FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "minimal_chained_fixup.bin"


def test_fixture_exists():
    assert FIXTURE.exists(), "fixture not generated; see Task A2 step 2"
    assert FIXTURE.stat().st_size > 64, "fixture is implausibly small"


def test_walker_parses_header_without_error():
    blob = FIXTURE.read_bytes()
    result = parse_chained_fixups_blob(blob)
    assert result.header.fixups_version == 0
    assert result.header.imports_count >= 0
    assert len(result.fixups) > 0, "expected at least one fixup entry"


def test_walker_classifies_entries():
    blob = FIXTURE.read_bytes()
    result = parse_chained_fixups_blob(blob)
    rebases = [f for f in result.fixups if isinstance(f, ChainedRebase)]
    binds   = [f for f in result.fixups if isinstance(f, ChainedBind)]
    # enu.dylib has both; the exact counts aren't fixed but both > 0.
    assert len(rebases) > 0
    assert len(binds)   > 0


def test_rebase_targets_resolve_to_section_offsets():
    """Every rebase entry must point at a section we can identify by name."""
    blob = FIXTURE.read_bytes()
    result = parse_chained_fixups_blob(blob)
    for r in (f for f in result.fixups if isinstance(f, ChainedRebase)):
        # offset within the section the target lives in
        assert r.target_offset >= 0
        # both segment + section names must be non-empty
        assert r.target_segment
        assert r.target_section
