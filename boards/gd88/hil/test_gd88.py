# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""GD-88 checks no other radio has a use for (docs/spec/hardware-gd88.md 13), SoC A only, read only: no transmit, no
bridge to SoC B, no restart. The display, build features and battery are test_health.py's, from board.toml [hil]. The
GD-88 has no USB: the debug link runs on the programming cable's UART1 at 115200, so give the cable's port with
--radio-port. A board's own test: skipped other_board on any other radio (tests/hil/hil_plugin.py)."""
import re

import rfoundry_hil
from rfoundry_hil import diag_lines, find

# ILI9341 variants (drivers/display/ili9341/ili9341.h): 0-3
PANEL_VARIANTS = range(4)


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
