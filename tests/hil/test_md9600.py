# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""MD-9600 bring-up checks (docs/spec/hardware-md9600.md 19), radio only, never transmits. They read the debug link's
`board` view section (19.6) and the storage section: identity, the clock that started, the data flash and its map,
the calibration and option blocks, idle keys, the ADC scan, the synthesizer and the transmitter switches held off.
Skipped on any other board."""
import pytest

KEYS = ["rev", "rev.source", "rev.usable", "rev.confirmed", "rev.stored", "probe", "clock", "sysclk_hz", "flash.id",
        "flash.size", "flash.map", "cal", "opt", "opt.backlight_switched", "opt.vhf_hz", "opt.uhf_hz", "keys", "ptt",
        "supply_mv", "lo_hz", "retunes", "refused", "pin.power", "pin.c5", "pin.r5", "pin.tx", "adc.noise_u",
        "adc.noise_v", "adc.rssi_u", "adc.rssi_v", "adc.mic", "adc.supply", "adc.mic_row", "adc.mic_col"]


@pytest.fixture
def md9600(radio):
    if radio.version().get("board") != "MD-9600":
        pytest.skip("not an MD-9600")
    return radio


def board_view(r):
    v = r.view(["board"])
    return {k[len("board."):]: val for k, val in v.items() if k.startswith("board.")}


def test_identity(md9600):
    v = md9600.version(fresh=True)
    assert v["display"] == "128x64x1", "the 128x64 monochrome ST7567"
    assert {"USB", "DEBUGLINK", "CPS_OPENFW"} <= md9600.features()


def test_board_view_keys(md9600):
    b = board_view(md9600)
    assert b.get("name") == "MD-9600"
    missing = [k for k in KEYS if k not in b]
    assert not missing, f"board view keys missing: {missing}"


def test_transmitter_held_off(md9600):
    """Transmit is refused in this build: both T5 supplies and the PA loop stay low whatever the radio does."""
    b = board_view(md9600)
    assert b["pin.tx"] == "000", f"T5 UHF, T5 VHF, APC enable: {b['pin.tx']}"
    assert b["rev.confirmed"] in ("0", "1")


def test_clock(md9600):
    """USB works only on the PLL; the debug link answering over USB already says the PLL runs. Which source it took
    settles hardware-md9600.md 17 #18 (crystal or oscillator)."""
    b = board_view(md9600)
    assert b["clock"] in ("crystal", "bypass"), f"clock {b['clock']}: the 8 MHz HSE did not start"
    assert int(b["sysclk_hz"]) == 72000000


def test_data_flash(md9600):
    b = board_view(md9600)
    dev = int(b["flash.id"], 16)
    assert dev & 0xFF == 0x18, f"flash capacity code {dev:#06x}: not a 16 MB part"
    assert int(b["flash.size"]) == 16 * 1024 * 1024 and b["flash.map"] == "1"
    s = md9600.view(["storage"])
    assert s["storage.region"] in ("ours", "foreign"), s["storage.region"]
    assert int(s["storage.errors"]) == 0


def test_calibration_and_options_read(md9600):
    """The security registers answer. A radio from the factory has both blocks; 'blank' means erased registers (worth a
    note in the bring-up log), 'unreadable' a bus or command problem."""
    b = board_view(md9600)
    assert b["cal"] in ("factory", "blank"), f"calibration {b['cal']}"
    assert b["opt"] in ("factory", "blank"), f"option block {b['opt']}"
    if b["opt"] == "factory" and b["opt.vhf_hz"] != "0-0":
        vlo, vhi = (int(x) for x in b["opt.vhf_hz"].split("-"))
        ulo, uhi = (int(x) for x in b["opt.uhf_hz"].split("-"))
        assert 130e6 <= vlo < vhi <= 180e6, b["opt.vhf_hz"]
        assert 400e6 <= ulo < uhi <= 520e6, b["opt.uhf_hz"]


def test_keys_idle(md9600):
    """Nobody at the panel: no matrix key and no PTT reads as pressed (a stuck row or column shows here)."""
    b = board_view(md9600)
    assert b["keys"] == "0x0000", f"matrix rows K0-K3: {b['keys']}"
    assert b["ptt"] == "0"


def test_adc_scan(md9600):
    """The ADC1 scan runs: every rank in range, and the supply reading plausible for a 13.8 V supply (the scale is
    unverified, hardware-md9600.md 10.3)."""
    b = board_view(md9600)
    for k in KEYS:
        if k.startswith("adc."):
            assert 0 <= int(b[k]) <= 4095, (k, b[k])
    assert int(b["adc.supply"]) > 0, "supply rank reads 0: the scan isn't running or the channel is wrong"
    mv = int(b["supply_mv"])
    assert 10000 <= mv <= 16000, f"supply {mv} mV from raw {b['adc.supply']}: scale or channel wrong"


def test_receiver_tuned(md9600):
    """With a revision this image drives, the synthesizer was programmed for the channel."""
    b = board_view(md9600)
    if b["rev.usable"] != "1":
        pytest.skip(f"revision {b['rev']} ({b['rev.source']}) not driven by this image")
    assert int(b["lo_hz"]) > 0 and int(b["retunes"]) > 0, b
    assert b["pin.power"] == "1", "Power_Control off while receiving"
