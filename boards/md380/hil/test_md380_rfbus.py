# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""MD-380 RF bus check (docs/testing/md380-bringup.md, issue #3), radio only, never transmits: after boot the bit-banged
HR_C5000 port reads its register 0x10 back as 0x80 and the SKY72310 synthesizer locks, which only happens when the pin
map and the clock edges in boards/md380/rfbus_md380.c are right. boards/md380/tests/md380RfbusTest.c checks the code
against the documented wiring; this is what checks the wiring. A board's own test: skipped other_board on any other
radio (tests/hil/hil_plugin.py)."""
from rfoundry_hil import diag_lines, find


def test_c5000_register_reads_back(radio):
    """The HR_C5000 answers on the configuration port: register 0x10 reads 0x80 after the driver's start-up (a dead or
    mis-wired bus reads 0xFF, which the board reports as --)."""
    line = find(diag_lines(radio), "C5000 10=")
    assert line, "no C5000 line in the diagnostics"
    assert line.startswith("C5000 10=80 "), f"HR_C5000 register 0x10 did not read back 0x80: {line}"
    assert line.endswith(" errs 0"), f"configuration port errors: {line}"


def test_synthesizer_locks(radio):
    """The SKY72310 took its words (clock PE4, chip select PD11, data PE5) and reports lock."""
    line = find(diag_lines(radio), "pll ")
    assert line, "no pll line in the diagnostics"
    assert line.startswith("pll lock "), f"synthesizer not locked: {line}"
