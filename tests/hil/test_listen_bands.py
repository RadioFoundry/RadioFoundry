# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The receive-only bands (docs/spec/airband-and-broadcast.md): the FM VFO tuned into the air band is received in AM,
tuned into the broadcast band it plays on the broadcast receiver; the VFO set to AM at 2 m where the radio can. Radio only: nothing keys, since a fault in the TX
refusal would put the radio on the air in a band it must never transmit in. The host tests cover the refusal."""
import pytest

from rfoundry_hil import skip

AIR_HZ = 118000000  # an air band frequency on the 25 kHz grid
TWO_M_HZ = 145500000  # 2 m, outside the air band: AM only where the board says its receiver demodulates it there
BCAST_HZ = 100100000  # an odd tenth, on every broadcast receiver's grid


def tune(radio, hz):
    radio.link.vfo_set(hz)
    return radio.wait_for(lambda v: v.get("rx_hz") == str(hz), 3.0, what=f"VFO at {hz}")


def need_vfo(radio, switch):
    # the feature marker only files the test under its checklist feature (bringup_plugin.py); the switch decides
    if switch not in radio.features():
        skip("not_built", f"built without CONFIG_{switch} (no such receiver on this radio, or switched off)")
    if "VFO_FM" not in radio.features():
        skip("not_built", "built without the FM VFO")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")


@pytest.mark.feature("am_rx")
def test_air_band_am(radio):
    need_vfo(radio, "AM_RX")
    with radio.vfo_session():
        v = tune(radio, AIR_HZ)
        assert v.get("am") == "1" and v.get("bcast") == "0", v
        assert v.get("mode") == "fm", v


@pytest.mark.feature("bcast_fm")
def test_broadcast_receiver(radio):
    need_vfo(radio, "BCAST_FM")
    with radio.vfo_session():
        v = tune(radio, BCAST_HZ)
        assert v.get("bcast") == "1" and v.get("am") == "0", v
        assert v.get("bcast_fault") == "0", "the broadcast receiver didn't answer"
    v = radio.core()
    assert v.get("bcast") == "0", "the broadcast receiver stayed on after the VFO left the band"


@pytest.mark.feature("am_rx")
def test_am_vfo_past_the_air_band(radio):
    """The VFO set to AM at 2 m (airband-and-broadcast.md 3.4), on a radio whose receiver demodulates AM past the air
    band. Nothing keys: AM is receive only, and the host tests cover the refusal."""
    need_vfo(radio, "AM_RX")
    if "AM" not in radio.features():
        skip("not_built", "this radio receives AM in the air band only (no board_t.am_bands)")
    with radio.vfo_session():
        tune(radio, TWO_M_HZ)
        try:
            radio.link.dmr_fields(mode="am")
            v = radio.wait_for(lambda v: v.get("am") == "1", 3.0, what="the VFO in AM at 2 m")
            assert v.get("bcast") == "0" and v.get("rx_hz") == str(TWO_M_HZ), v
            ch = radio.view(["channel"])
            assert ch.get("ch.kind") == "5", ch
        finally:
            radio.link.dmr_fields(mode="fm")     # the VFO's AM is kept with it: back to FM for the next test
            radio.wait_for(lambda v: v.get("am") == "0", 3.0, what="the VFO back in FM")
