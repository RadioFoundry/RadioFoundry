# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""GD-88 checks no other radio has a use for (docs/spec/hardware-gd88.md 13), read only: no transmit, no bridge to SoC
B, no restart. SoC B is reached only through A's RX2 FORWARD (docs/spec/rx2-link.md 2.3), one read-only request at a
time. The display, build features and battery are test_health.py's, from board.toml [hil]. The
GD-88 has no USB: the debug link runs on the programming cable's UART1 at 115200, so give the cable's port with
--radio-port. A board's own test: skipped other_board on any other radio (tests/hil/hil_plugin.py)."""
import contextlib
import re

import pytest

import rfoundry_hil
from rfoundry_hil import diag_lines, find

# ILI9341 variants (drivers/display/ili9341/ili9341.h): 0-3
PANEL_VARIANTS = range(4)
# B's program flash: RadioFoundry's first stage at 0x0300C000 starts with this word (boot/README.md)
B_FIRST_STAGE, B_FIRST_STAGE_SIG = 0x0300C000, bytes([0x2C, 0x11, 0x01, 0xC0])


@pytest.fixture
def soc_b(radio):
    """A's debug link with every request forwarded to SoC B (RX2 FORWARD); skips when B can't be reached."""
    flags = radio.link.rx2_info()
    if "present" not in flags:
        rfoundry_hil.skip("no_hw", f"no second receiver: RX2 INFO {sorted(flags)}")
    if "forward" not in flags:
        rfoundry_hil.skip("not_built", f"A's firmware has no RX2 FORWARD: RX2 INFO {sorted(flags)}")
    if "up" not in flags:
        rfoundry_hil.skip("state", "SoC B does not answer A on UART3 (not running RadioFoundry, or down): install it "
                                   "through the bridge")
    with forwarded(radio.link) as link:
        yield link


@contextlib.contextmanager
def forwarded(link):
    link.rx2 = True
    try:
        yield link
    finally:
        link.rx2 = False


def test_panel_variant(radio):
    line = find(diag_lines(radio), "lcd ")
    assert line, "no lcd line"
    m = re.match(r"lcd v(\d+) (rec|dflt)$", line)
    assert m, line
    assert int(m.group(1)) in PANEL_VARIANTS, line
    if m.group(2) == "dflt":
        rfoundry_hil.skip("no_hw", f"no panel record in program flash, the default variant is in use ({line})")


def test_second_receiver(radio):
    """SoC B is there and answers on UART3; the core knows two receivers. Only the INFO request: no bridge, no
    restart."""
    flags = radio.link.rx2_info()
    assert "present" in flags, f"no second receiver: {flags}"
    assert "bridge" in flags and "restart" in flags, f"the bridge and restart operations: {flags}"
    assert "up" in flags, f"SoC B doesn't answer on UART3: {flags} ({find(diag_lines(radio), 'b ')})"
    f = radio.link.view(["features"])
    assert f.get("dual.receivers") == "2", f"the core sees {f.get('dual.receivers')} receivers"


def test_nand_and_calibration(radio):
    """The SPI NAND answers and a valid factory calibration page is found (A's diagnostics print nand and cal lines,
    not the flash line test_health.py reads)."""
    lines = diag_lines(radio)
    nand = find(lines, "nand ")
    assert nand and nand != "nand none", f"no SPI NAND: {nand}"
    assert nand.split()[1] not in ("0000", "FFFF"), f"the NAND's ID reads as an idle bus: {nand}"
    cal = find(lines, "cal ")
    assert cal and cal != "cal none", f"no calibration page found: {cal}"
    assert re.match(r"cal (flash|nand) #\d+ c \d+", cal), f"calibration found but not valid: {cal}"


def test_soc_b_version(soc_b):
    """B answers VERSION through A: a board of its own, the forward route, a build."""
    vb = soc_b.version()
    soc_b.rx2 = False                   # A's own VERSION, then back to B
    try:
        va = soc_b.version()
    finally:
        soc_b.rx2 = True
    assert vb.get("via") == "forward", vb
    b_id, a_id = rfoundry_hil.board_id(vb.get("board")), rfoundry_hil.board_id(va.get("board"))
    assert b_id and b_id != a_id, f"B reports board {vb.get('board')!r} (A: {va.get('board')!r})"
    assert vb.get("git"), vb
    if vb.get("git") != va.get("git"):
        print(f"B runs git {vb.get('git')}, A {va.get('git')}: installed from different builds")


def test_soc_b_log(soc_b):
    """B's own log ring reads through A (it may be empty)."""
    dropped, text = soc_b.log()
    assert dropped >= 0 and isinstance(text, str)


def test_soc_b_first_stage(soc_b):
    """B's READ_MEM of its program flash: RadioFoundry's first stage at 0x0300C000."""
    got = soc_b.read_mem(B_FIRST_STAGE, len(B_FIRST_STAGE_SIG))
    assert got == B_FIRST_STAGE_SIG, f"0x{B_FIRST_STAGE:08X}: {got.hex(' ')}"


def test_soc_b_view(soc_b):
    """B's GET_VIEW: its own sections, at least one with keys."""
    v = soc_b.view()
    assert set(v) - {"ms"}, f"no view keys from B: {v}"
