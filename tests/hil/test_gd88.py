# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""GD-88 bring-up checks (docs/spec/hardware-gd88.md 13), SoC A only, read only: no transmit, no bridge to SoC B, no
restart. The GD-88 has no USB: the debug link runs on the programming cable's UART1 at 115200, so give the cable's
port with --radio-port. Skipped on any other board."""
import re

import pytest

# ILI9341 variants (drivers/display/ili9341/ili9341.h): 0-3
PANEL_VARIANTS = range(4)


@pytest.fixture
def gd88(radio):
    if radio.version().get("board") != "GD-88":
        pytest.skip("not a GD-88")
    return radio


def diag_lines(r):
    return r.link.diag()[1]


def find(lines, prefix):
    for line in lines:
        if line.startswith(prefix):
            return line
    return None


def test_identity(gd88):
    v = gd88.version(fresh=True)
    assert v["display"] == "320x240x4", "the 320x240 panel at full resolution in 16 colors"
    feats = gd88.features()
    assert "USB" not in feats and {"HOSTLINK", "DEBUGLINK", "FM", "DIAG"} <= feats, feats


def test_panel_variant(gd88):
    line = find(diag_lines(gd88), "lcd ")
    assert line, "no lcd line"
    m = re.match(r"lcd v(\d+) (rec|dflt)$", line)
    assert m, line
    assert int(m.group(1)) in PANEL_VARIANTS, line
    if m.group(2) == "dflt":
        pytest.skip(f"no panel record in program flash, the default variant is in use ({line})")


def test_second_receiver(gd88):
    """SoC B is there and answers on UART3; the core knows two receivers. Only the INFO request: no bridge, no
    restart."""
    flags = gd88.link.rx2_info()
    assert "present" in flags, f"no second receiver: {flags}"
    assert "bridge" in flags and "restart" in flags, f"the bridge and restart operations: {flags}"
    assert "up" in flags, f"SoC B doesn't answer on UART3: {flags} ({find(diag_lines(gd88), 'b ')})"
    f = gd88.link.view(["features"])
    assert f.get("dual.receivers") == "2", f"the core sees {f.get('dual.receivers')} receivers"


def test_battery(gd88):
    bat = find(diag_lines(gd88), "bat ")
    assert bat, "no bat line"
    mv = int(re.findall(r"\d+", bat)[0])
    assert 5500 < mv < 8800, f"battery {mv} mV: ADC channel or scale wrong ({bat})"


def test_nand_and_calibration(gd88):
    lines = diag_lines(gd88)
    nand = find(lines, "nand ")
    assert nand and nand != "nand none", f"no SPI NAND: {nand}"
    assert nand.split()[1] not in ("0000", "FFFF"), f"the NAND's ID reads as an idle bus: {nand}"
    cal = find(lines, "cal ")
    assert cal and cal != "cal none", f"no calibration page found: {cal}"
    assert re.match(r"cal (flash|nand) #\d+ c \d+", cal), f"calibration found but not valid: {cal}"
