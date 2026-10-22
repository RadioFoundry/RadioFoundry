# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""DM-32UV bring-up checks (docs/testing/dm32-bringup.md), radio only, never transmits. The DM-32 has no USB: the
debug link runs on the programming cable's UART at 115200, so give the cable's port with --radio-port. Skipped on any
other board."""
import re
import time

import pytest

from odr_hil import skip


@pytest.fixture
def dm32(radio):
    if radio.version().get("board") != "DM-32UV":
        skip("other_board", "not a DM-32UV")
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
    assert v["display"] == "240x320x4", "the 240x320 panel at full resolution in 16 colors"
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
    The HR_C7000 raises timeslot interrupts only once it has synced to a signal, so a quiet channel has nothing to
    check (the SDR bench call tests cover the interrupts then)."""
    if "DMR" not in dm32.features():
        skip("not_built", "built without DMR")
    lines0 = diag_lines(dm32)
    if not find(lines0, "DMR on"):
        skip("state", "the radio is not on a DMR channel")
    time.sleep(2)
    lines1 = diag_lines(dm32)
    ts0 = [int(x) for x in re.findall(r"\d+", find(lines0, "ts "))[:2]]
    ts1 = [int(x) for x in re.findall(r"\d+", find(lines1, "ts "))[:2]]
    tk0 = [int(x) for x in re.findall(r"\d+", find(lines0, "DMR on"))[:1]]
    tk1 = [int(x) for x in re.findall(r"\d+", find(lines1, "DMR on"))[:1]]
    if tk1 == tk0 and ts1 == ts0:
        skip("bench", "no DMR traffic on the channel: the modem raises no slot interrupts until it syncs")
    assert ts1[1] > ts0[1], f"no RX timeslot interrupts in 2 s ({ts0} -> {ts1}): PIC vector mapping?"


def test_codec_present(dm32):
    if "DMR" not in dm32.features():
        skip("not_built", "built without DMR")
    line = find(diag_lines(dm32), "codec ")
    assert line, "no codec line"
    if line.endswith("none"):
        skip("not_built", "no stock codec merged (tools/hrc7000_codec.py): DMR voice is silent")
    assert line in ("codec 049", "codec 046"), line


def test_gps_nmea(dm32):
    """The GPS module talks on UART1 (hardware-dm32.md 12): heard at start-up, valid sentences keep coming (about one a
    second, fix or not), few bad ones. Right after start-up it waits out the driver's 5 s listen."""
    line = find(diag_lines(dm32), "gps ")
    if line is None:
        skip("not_built", "a build without the GPS driver")
    if line.startswith("gps no"):
        time.sleep(6)  # just started: the driver may still be listening
        line = find(diag_lines(dm32), "gps ")
    good0, bad0, b0 = (int(x) for x in re.findall(r"\d+", line)[:3])
    assert line.startswith("gps yes"), f"no valid NMEA heard: {line} (b0 = no bytes: PTA9 mux or GPIO 0x2C power)"
    if line.endswith(" off"):
        skip("state", "Settings > Location > GPS is off: the module was heard at start-up, then powered down")
    time.sleep(3)
    good1, bad1, b1 = (int(x) for x in re.findall(r"\d+", find(diag_lines(dm32), "gps "))[:3])
    assert good1 > good0, f"no new sentences in 3 s ({good0} -> {good1})"
    assert b1 > b0
    assert bad1 - bad0 <= (good1 - good0) // 4, f"many bad sentences ({bad0} -> {bad1}): baud rate or line noise"
