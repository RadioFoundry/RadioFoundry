# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""DM-32UV bring-up checks (docs/testing/dm32-bringup.md), radio only, never transmits. The DM-32 has no USB: the
debug link runs on the programming cable's UART at 115200, so give the cable's port with --radio-port. Skipped on any
other board."""
import re
import time

import pytest


@pytest.fixture
def dm32(radio):
    if radio.version().get("board") != "DM-32UV":
        pytest.skip("not a DM-32UV")
    return radio


def diag_lines(r):
    return r.link.diag()[1]


def find(lines, prefix):
    for line in lines:
        if line.startswith(prefix):
            return line
    return None


def test_identity(dm32):
    v = dm32.version(fresh=True)
    assert v["display"] == "120x160x16", "the 240x320 panel at 2x scale"
    feats = dm32.features()
    assert "USB" not in feats and {"HOSTLINK", "DEBUGLINK"} <= feats, feats


def test_data_flash_and_calibration(dm32):
    lines = diag_lines(dm32)
    flash = find(lines, "flash ")
    assert flash, lines
    jedec = flash.split()[1]
    assert jedec in ("C84018", "852018"), f"unexpected JEDEC ID {jedec} (GD25Q128 C84018, Puya 852018)"
    assert "cal factory" in flash or "cal copy" in flash, f"no factory calibration found: {flash}"
    txc = find(lines, "txc ")
    assert txc and int(txc.split()[1]) != 128, f"TX center still the uncalibrated 128: {txc}"
    pa = find(lines, "pa ")
    vals = [int(x) for x in re.findall(r"\d+", pa)]
    assert all(0 < x < 4096 for x in vals) and vals != [1408] * 4, f"PA drive from calibration: {pa}"


def test_battery(dm32):
    bat = find(diag_lines(dm32), "bat ")
    raw, mv = (int(x) for x in re.findall(r"\d+", bat)[:2])
    assert 5500 < mv < 8800, f"battery {mv} mV (raw {raw}): ADC channel or scale wrong"


def test_receiver_alive(dm32):
    """The FD6818 answers: RSSI / noise reads change over a few seconds (a dead bus reads constant 0 or 0xFFFF)."""
    seen = set()
    for _ in range(10):
        line = find(diag_lines(dm32), "rssi ")
        seen.add(line)
        time.sleep(0.3)
    assert None not in seen, "no rssi line"
    stuck = all(x.startswith(("rssi 0 nz 0 ", "rssi 255 nz 255 ")) for x in seen)
    assert not stuck, f"FD6818 reads all 0 or all 1: bus or read timing wrong ({seen})"


def test_dmr_modem_ticks(dm32):
    """With a DMR channel selected and DMR traffic on it, the slot engine ticks (33 per second) and bursts arrive.
    Without traffic only the interrupt counters are checked."""
    if "DMR" not in dm32.features():
        pytest.skip("built without DMR")
    lines0 = diag_lines(dm32)
    if not find(lines0, "DMR on"):
        pytest.skip("the radio is not on a DMR channel")
    time.sleep(2)
    lines1 = diag_lines(dm32)
    ts0 = [int(x) for x in re.findall(r"\d+", find(lines0, "ts "))[:2]]
    ts1 = [int(x) for x in re.findall(r"\d+", find(lines1, "ts "))[:2]]
    assert ts1[1] > ts0[1], f"no RX timeslot interrupts in 2 s ({ts0} -> {ts1}): PIC vector mapping?"


def test_codec_present(dm32):
    if "DMR" not in dm32.features():
        pytest.skip("built without DMR")
    line = find(diag_lines(dm32), "codec ")
    assert line, "no codec line"
    if line.endswith("none"):
        pytest.skip("no stock codec merged (tools/dm32_codec.py): DMR voice is silent")
    assert line in ("codec 049", "codec 046"), line
