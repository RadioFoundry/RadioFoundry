# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""GD-77 bring-up checks (docs/spec/hardware-gd77.md 14), radio only, never transmits. Everything is read from the
debug link's board view section (board.* keys: pin levels, the clock plan, the reset cause, ADC readings, what the chip
drivers found) and the diagnostics lines; nothing writes a pin or a chip register. The manual test asks a person to
hold each key, to settle the key roles that are still open. A board's own test: skipped other_board on any other
radio (tests/hil/hil_plugin.py)."""
import time

import pytest

from odr_hil import as_int, skip

PLANS = {49152000: False, 95232000: True}      # core clock -> HSRUN (boards/gd77/clock_gd77.h)
RESET_NAMES = {"por", "pin", "lvd", "wakeup", "sw", "wdog", "lockup", "lol", "loc", "mdm", "jtag", "ezport",
               "sackerr"}


@pytest.fixture
def gd77(radio):
    return radio


def board(r):
    v = r.view(["board"])
    if v.get("board.built") != "1":
        skip("not_built", "firmware without the board view section")
    return v


def bit(v, port, n):
    return (as_int(v[f"board.pins.{port}"]) >> n) & 1


def test_identity(gd77):
    v = gd77.version(fresh=True)
    assert v["display"] == "128x64x1", "the 128x64 monochrome panel"
    feats = gd77.features()
    assert {"USB", "HOSTLINK", "DEBUGLINK"} <= feats, feats
    b = board(gd77)
    assert as_int(b["board.chip.flash_kb"]) == 512, "MK22FN512: 512 KB program flash"
    assert as_int(b["board.chip.sdid"]) not in (0, 0xFFFFFFFF), b["board.chip.sdid"]


def test_clock_plan(gd77):
    b = board(gd77)
    core, bus = as_int(b["board.clk.core_hz"]), as_int(b["board.clk.bus_hz"])
    assert b["board.clk.source"] == "pll", \
        f"on the internal FLL ({core} Hz): the 12.288 MHz reference on EXTAL0 or the PLL never came up"
    assert core in PLANS, f"core {core} Hz is neither clock plan"
    assert b["board.clk.hsrun"] == ("1" if PLANS[core] else "0"), b
    assert bus <= core and bus in (49152000, 47616000), b
    assert b["board.clk.stepped_down"] == "0", "left in the RUN step after a flash command"


def test_reset_cause(gd77):
    b = board(gd77)
    assert b["board.reset.cause"] in RESET_NAMES, b["board.reset.cause"]
    assert b["board.reset.cause"] not in ("wdog", "lockup", "lol", "loc"), \
        f"the last reset was {b['board.reset.cause']} (SRS {b['board.reset.srs']})"


def test_power_pins(gd77):
    """Running: the power hold (PTE26) driven high and the knob switch (PTA13) reading on."""
    b = board(gd77)
    assert bit(b, "e", 26) == 1, "power hold PTE26 low while running"
    assert bit(b, "a", 13) == 1, "the knob switch on PTA13 reads off on a running radio: its role is the open item"


def test_nothing_keyed(gd77):
    """The transmit enable candidate and the RF switch candidates stay low: receive only (spec 5.4)."""
    b = board(gd77)
    for port, n, name in (("a", 17, "PTA17 TX enable"), ("e", 2, "PTE2"), ("e", 3, "PTE3"), ("c", 13, "PTC13"),
                          ("c", 15, "PTC15")):
        assert bit(b, port, n) == 0, f"{name} is high"


def test_buses_and_chips(gd77):
    b = board(gd77)
    for name in ("i2c_a", "i2c_b", "nor", "lcd", "c6000"):
        kind, state = b[f"board.bus.{name}"].split(":")
        assert state == "ok", f"bus {name} ({kind}): {state}"
    assert b["board.chip.at1846s"].startswith("ok:"), f"AT1846S on I2C bus A: {b['board.chip.at1846s']}"
    assert b["board.chip.hrc6000"] == "ok", "HR_C6000 init: check the PTE0 / PTE1 order (spec 11)"
    assert as_int(b["board.chip.nor_jedec"]) not in (0, 0xFFFFFF), "data flash JEDEC ID: chip select PTA19"
    assert b["board.chip.eeprom"] == "ok", "EEPROM at 0x50 on I2C bus A"
    if b["board.bus.i2c_a"].startswith("hw"):
        assert as_int(b["board.i2c0.errors"]) == 0, b["board.i2c0.errors"]


def test_adc_readings(gd77):
    """The three analog inputs read inside the converter's range; the battery candidate isn't a dead channel."""
    deadline = time.monotonic() + 2.0
    while True:
        b = board(gd77)
        raw = [as_int(b[f"board.adc.dp{i}"]) for i in (1, 2, 3)]
        if all(x != 0xFFFF for x in raw) or time.monotonic() > deadline:
            break
        time.sleep(0.3)     # the readings come with the battery poll
    assert all(0 <= x <= 4095 for x in raw), raw
    assert 200 < raw[0] < 4000, f"battery candidate ADC0_DP1 reads {raw[0]}: another channel, or a divider"
    print(f"    ADC0 DP1 {raw[0]}  DP2 {raw[1]}  DP3 {raw[2]}")


def test_usb_clock_recovery(gd77):
    b = board(gd77)
    if b.get("board.usb.built") == "0":
        skip("not_built", "built without USB")
    assert b["board.usb.trim_error"] == "0", "the IRC48M clock recovery ran out of trim range"


def test_keys_idle(gd77):
    """Nobody touching the radio: no key bit set (a bit stuck on is a wrong pull or a wrong direct key pin)."""
    assert as_int(board(gd77)["board.keys.raw"]) == 0, board(gd77)["board.keys.raw"]


KEYS = ["green (menu)", "red (back)", "up arrow", "down arrow", "1", "5", "9", "0", "*", "#",
        "side key 1 (upper)", "side key 2 (lower)", "the top key, if the radio has one"]


@pytest.mark.manual
def test_key_positions(gd77):
    """Ask for each key in turn and print its raw bit (matrix: column x 5 + row; direct keys 20-23), to settle the
    provisional navigation key names and the direct key roles (spec 9.1). Asserts only that each press set one bit.
    PTT isn't asked for: no test presses it. It is the direct key no other key showed up as."""
    found = {}
    for name in KEYS:
        print(f"\n>>> Hold {name} until told to let go (up to 10 s; no press skips it)", flush=True)
        end = time.monotonic() + 10.0
        bits = 0
        while time.monotonic() < end and bits == 0:
            bits = as_int(board(gd77)["board.keys.raw"])
            time.sleep(0.1)
        print(f"    {name}: raw 0x{bits:06x}" + ("" if bits else " (nothing)") + "  - let go now", flush=True)
        while as_int(board(gd77)["board.keys.raw"]) != 0 and time.monotonic() < end + 5.0:
            time.sleep(0.1)
        if bits:
            found[name] = bits
    for name, bits in found.items():
        assert bits & (bits - 1) == 0, f"{name} set more than one bit (0x{bits:06x}): two keys, or a short"
    assert len(set(found.values())) == len(found), f"two keys gave the same bit: {found}"
