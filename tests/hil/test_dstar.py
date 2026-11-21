# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""D-STAR on the radio (docs/spec/dstar-modem.md, docs/testing/dstar-bench.md), through the debug link's dstar section
and command (docs/spec/debuglink.md 3.30).

test_dstar_view: the section is there with the calibration in force and the board's knobs. No RF.
test_dstar_cal_roundtrip: a calibration value set over the debug link is stored and shown, and the board's value
    comes back with -1. No RF.
test_dstar_rx_from_radio: opt-in (--dstar-call): a person keys a D-STAR radio (an Icom ID-51 or IC-705) with that
    MY callsign within a minute; the radio on a D-STAR channel shows its header with a good CRC (heard from the air,
    not only from the slow data), the polarity it found, voice frames, and the end. The radio never transmits.

The transmit tests need an SDR and a person at the bench: tools/dstar_bench.py txpath and over, in the bench plan."""
import time

import pytest

from rfoundry_hil import skip
from hil_config import rig


def dstar_view(radio):
    if "DSTAR" not in radio.features():
        skip("not_built", "built without CONFIG_DSTAR")
    v = radio.link.dstar()
    if v.get("dstar.built") != "1":
        skip("not_built", "the firmware's view has no D-STAR")
    return v


@pytest.mark.feature("dstar")
def test_dstar_view(radio):
    v = dstar_view(radio)
    for key in ("dstar.on", "dstar.fault", "dstar.voice", "dstar.tx_my", "dstar.tx_ur", "dstar.set_tx_1khz",
                "dstar.rx_samples", "dstar.tx_frames", "dstar.test"):
        assert key in v, key
    assert v["dstar.test"] == "off"
    # A board with the flat paths says what it knows of them
    if v.get("dstar.fault") != "no_path":
        assert "dstar.board_tx_1khz" in v and "dstar.board_tx_latency_ms" in v


@pytest.mark.feature("dstar")
def test_dstar_cal_roundtrip(radio):
    v = dstar_view(radio)
    was = int(v["dstar.set_tx_hpf"])
    try:
        radio.link.dstar_cal("tx_hpf", 123)
        v = radio.wait_for(lambda x: x.get("dstar.set_tx_hpf") == "123", 3.0, sections=("dstar",),
                           what="the stored high-pass")
        radio.link.dstar_cal("tx_hpf", -1)
        v = radio.wait_for(lambda x: x.get("dstar.set_tx_hpf") == "-1", 3.0, sections=("dstar",),
                           what="the board's high-pass again")
    finally:
        radio.link.dstar_cal("tx_hpf", was)


@pytest.mark.feature("dstar", owner="a person keys a D-STAR radio when asked")
def test_dstar_rx_from_radio(radio, pytestconfig):
    call = str(rig(pytestconfig, "dstar-call", "the MY callsign of the D-STAR radio a person keys:")).upper()
    v = dstar_view(radio)
    if v.get("dstar.on") != "1":
        skip("state", "the radio is not on a D-STAR channel")
    radio.link.dstar_reset()
    print(f"\nkey the D-STAR radio ({call}) for 3 s with a message set, within 60 s")
    end = time.time() + 60.0
    seen = None
    while time.time() < end:
        v = radio.link.dstar()
        if v.get("dstar.my") == call and v.get("dstar.rx") == "0" and int(v.get("dstar.rx_frames", "0")) > 0:
            seen = v
            break
        time.sleep(0.3)
    assert seen is not None, "no over from %s heard to its end" % call
    print("header %s/%s UR %s RPT1 %s RPT2 %s msg %r polarity %s frames %s bad %s" % (
        seen["dstar.my"], seen["dstar.suffix"], seen["dstar.ur"], seen["dstar.rpt1"], seen["dstar.rpt2"],
        seen["dstar.msg"], seen["dstar.polarity"], seen["dstar.rx_frames"], seen["dstar.rx_bad"]))
    # From the air: a header whose CRC checked (dstar-modem.md 3.1, the preset and the inversion settled)
    assert seen["dstar.heard_slow"] == "0", "the header came only from the slow data: its CRC failed on air"
    assert int(seen["dstar.rx_frames"]) >= 100
    assert int(seen["dstar.rx_bad"]) <= int(seen["dstar.rx_frames"]) // 20
