# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Health checks for any radio, radio only, never transmits: the display geometry and build features VERSION reports,
the data flash and factory calibration, the battery reading, a receiver that answers, DMR timeslot interrupts, the
merged stock codec and the GPS. The numbers each board must meet come from its HIL expectations (odr_hil.board_hil:
board.toml [hil]). A check whose expectation the board doesn't have skips (other_board: nothing to compare against on
this board, which a bring-up run excuses), and one that reads a diagnostics line the firmware doesn't print skips
not_built."""
import re
import time

import pytest

from odr_hil import board_hil, board_id, diag_lines, find, skip


class Board:
    """The radio and its board's expectations."""

    def __init__(self, radio):
        self.radio = radio
        self.name = radio.version().get("board", "")
        self.id = board_id(self.name)
        self.hil = board_hil(self.id)

    def expect(self, key):
        if key not in self.hil:
            skip("other_board", f"no {key} expectation for {self.id or self.name} (board.toml [hil])")
        return self.hil[key]

    def line(self, prefix, lines=None):
        line = find(diag_lines(self.radio) if lines is None else lines, prefix)
        if line is None:
            skip("not_built", f"the diagnostics have no '{prefix.strip()}' line on this firmware")
        return line


@pytest.fixture
def board(radio):
    return Board(radio)


def numbers(line):
    return [int(x) for x in re.findall(r"\d+", line)]


def test_display(board):
    want = board.expect("display")
    assert board.radio.version(fresh=True)["display"] == want, f"the panel at its full geometry ({want})"


def test_build_features(board):
    if "features" not in board.hil and "no_features" not in board.hil:
        skip("other_board", f"no features expectation for {board.id or board.name} (board.toml [hil])")
    feats = board.radio.features()
    assert set(board.hil.get("features", [])) <= feats, feats
    assert not set(board.hil.get("no_features", [])) & feats, feats


def test_data_flash(board):
    want = board.expect("flash_jedec")
    jedec = board.line("flash ").split()[1]
    assert jedec in want, f"unexpected data flash JEDEC ID {jedec} (accepted: {', '.join(want)})"


def test_calibration(board):
    if not board.expect("calibration"):
        skip("other_board", f"{board.id} has no factory calibration to check")
    lines = diag_lines(board.radio)
    flash = board.line("flash ", lines)
    assert "cal factory" in flash or "cal copy" in flash, f"no factory calibration found: {flash}"
    txc = board.line("txc ", lines)
    assert numbers(txc)[0] != 128, f"TX center still the uncalibrated 128: {txc}"
    pa = numbers(board.line("pa ", lines))
    assert all(0 < x < 4096 for x in pa) and pa != [1408] * 4, f"PA drive not from calibration: {pa}"


def test_battery(board):
    low, high = board.expect("battery_mv")
    raw, mv = numbers(board.line("bat "))[:2]
    assert low < mv < high, f"battery {mv} mV (raw {raw}), not in {low}-{high}: ADC channel or scale wrong"


def test_receiver_alive(board):
    """The receiver chip answers: RSSI and noise reads change over a few seconds (a dead bus reads a constant 0 or
    all ones)."""
    seen = set()
    for _ in range(10):
        seen.add(board.line("rssi "))
        time.sleep(0.3)
    stuck = all(x.startswith(("rssi 0 nz 0 ", "rssi 255 nz 255 ")) for x in seen)
    assert not stuck, f"the receiver reads all 0 or all 1: bus or read timing wrong ({seen})"


def test_dmr_modem_ticks(board):
    """With a DMR channel selected and DMR traffic on it, the slot engine ticks (33 per second) and bursts arrive. A
    modem that raises timeslot interrupts only once it has synced has nothing to show on a quiet channel (the SDR bench
    call tests cover the interrupts then)."""
    if "DMR" not in board.radio.features():
        skip("not_built", "built without DMR")
    lines0 = diag_lines(board.radio)
    if not find(lines0, "DMR on"):
        skip("state", "the radio is not on a DMR channel")
    time.sleep(2)
    lines1 = diag_lines(board.radio)
    ts0, ts1 = (numbers(board.line("ts ", x))[:2] for x in (lines0, lines1))
    tk0, tk1 = (numbers(board.line("DMR on", x))[:1] for x in (lines0, lines1))
    if tk1 == tk0 and ts1 == ts0:
        skip("bench", "no DMR traffic on the channel: the modem raises no slot interrupts until it syncs")
    assert ts1[1] > ts0[1], f"no RX timeslot interrupts in 2 s ({ts0} -> {ts1}): interrupt vector mapping?"


def test_codec_present(board):
    want = board.expect("codec")
    if "DMR" not in board.radio.features():
        skip("not_built", "built without DMR")
    line = board.line("codec ")
    if line.endswith("none"):
        skip("not_built", "no stock codec merged: DMR voice is silent")
    assert line.split()[1] in want, f"{line} (accepted: {', '.join(want)})"


def test_gps_nmea(board):
    """The GPS module talks: heard at start-up, valid sentences keep coming (about one a second, fix or not), few bad
    ones. Right after start-up it waits out the driver's 5 s listen."""
    line = find(diag_lines(board.radio), "gps ")
    if line is None:
        skip("not_built", "a build without the GPS driver")
    if line.startswith("gps no"):
        time.sleep(6)  # just started: the driver may still be listening
        line = board.line("gps ")
    good0, bad0, b0 = numbers(line)[:3]
    assert line.startswith("gps yes"), f"no valid NMEA heard: {line} (b0 = no bytes: the UART pins or the power)"
    if line.endswith(" off"):
        skip("state", "Settings > Location > GPS is off: the module was heard at start-up, then powered down")
    time.sleep(3)
    good1, bad1, b1 = numbers(board.line("gps "))[:3]
    assert good1 > good0, f"no new sentences in 3 s ({good0} -> {good1})"
    assert b1 > b0
    assert bad1 - bad0 <= (good1 - good0) // 4, f"many bad sentences ({bad0} -> {bad1}): baud rate or line noise"
