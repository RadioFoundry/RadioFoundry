# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The debug link's peek and poke (DL_CMD_MEM, docs/spec/debuglink.md 3.24), without changing anything on the radio.

- test_mem_read_matches_read_mem: 16 bytes at the start of a READ_MEM window read by MEM at each width equal READ_MEM's.
- test_mem_changes_need_the_key: a write and a pin change unarmed, and after an arm and a disarm, are refused before any
  access (the write goes to program flash, so it would not take anyway); a pin read needs no key.
- test_mem_on_second_soc: the same read on a radio's second SoC through RX2 FORWARD. Skips on a radio with one.

Radio only, nothing transmitted. Skipped on firmware built without CONFIG_DEBUG_MEM.
"""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import rfoundry_debug as dbg  # noqa: E402

from rfoundry_hil import skip  # noqa: E402

N = 16


def _window(link):
    """The start of a READ_MEM window (the chip's, or any known one without INFO_CHIP) that reads the same twice: the
    comparisons below read it more than once, so RAM in use would differ."""
    windows = dbg.MEM_WINDOWS.get(dbg.chip_family(link)) or [w for ws in dbg.MEM_WINDOWS.values() for w in ws]
    for lo, _ in windows:
        try:
            if link.read_mem(lo, N) == link.read_mem(lo, N):
                return lo
        except dbg.DebugLinkError:
            pass
    skip("state", "no READ_MEM window that holds still")


def _need_mem(link):
    try:
        link.mem_read(_window(link), 4, 1)
    except dbg.DebugLinkError as e:
        if e.status in (1, dbg.ST_UNAVAILABLE):
            skip("not_built", "firmware without DL_CMD_MEM (CONFIG_DEBUG_MEM=n)")
        raise


def _read_matches(link):
    base = _window(link)
    want = link.read_mem(base, N)
    for w in (1, 2, 4):
        got = b"".join(v.to_bytes(w, "little") for v in link.mem_read(base, w, N // w))
        assert got == want, f"width {w}: {got.hex()} != READ_MEM {want.hex()}"


def _refused(fn):
    with pytest.raises(dbg.DebugLinkError) as e:
        fn()
    return e.value.status


@pytest.mark.feature("debug_mem")
def test_mem_read_matches_read_mem(radio):
    _need_mem(radio.link)
    _read_matches(radio.link)
    assert _refused(lambda: radio.link.mem_read(_window(radio.link) + 2, 4, 1)) == 2, "an unaligned read must fail"


@pytest.mark.feature("debug_mem")
def test_mem_changes_need_the_key(radio):
    L = radio.link
    _need_mem(L)
    base = _window(L)
    L.mem_disarm()
    assert _refused(lambda: L.mem_write(base, 0)) == dbg.ST_REFUSED
    L.mem_arm()
    L.mem_disarm()
    assert _refused(lambda: L.mem_write(base, 0, op="clear")) == dbg.ST_REFUSED
    try:
        state = L.pin(0)
    except dbg.DebugLinkError as e:
        assert e.status == dbg.ST_UNAVAILABLE, e
        return      # no pin hook on this platform
    print("pin 0:", state)
    assert _refused(lambda: L.pin(0, "in")) == dbg.ST_REFUSED
    assert L.pin(0) == state


@pytest.mark.feature("debug_mem")
def test_mem_on_second_soc(radio):
    L = radio.link
    if "forward" not in L.rx2_info():
        skip("not_built", "no second SoC (or no RX2 FORWARD)")
    L.rx2 = True
    try:
        _need_mem(L)
        _read_matches(L)
        assert _refused(lambda: L.mem_write(_window(L), 0)) == dbg.ST_REFUSED
    finally:
        L.rx2 = False
