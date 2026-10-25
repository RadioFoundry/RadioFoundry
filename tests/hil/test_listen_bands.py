# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The receive-only bands (docs/spec/airband-and-broadcast.md): the FM VFO tuned into the air band is received in AM,
tuned into the broadcast band it plays on the broadcast receiver. Radio only: nothing keys, since a fault in the TX
refusal would put the radio on the air in a band it must never transmit in. The host tests cover the refusal."""
import pytest

from rfoundry_hil import skip

AIR_HZ = 118000000  # an air band frequency on the 25 kHz grid
BCAST_HZ = 100100000  # an odd tenth, on every broadcast receiver's grid


def tune(radio, hz):
    radio.link.vfo_set(hz)
    return radio.wait_for(lambda v: v.get("rx_hz") == str(hz), 3.0, what=f"VFO at {hz}")


def need_vfo(radio):
    if "VFO_FM" not in radio.features():
        skip("not_built", "built without the FM VFO")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")


@pytest.mark.feature("am_rx")
def test_air_band_am(radio):
    need_vfo(radio)
    with radio.vfo_session():
        v = tune(radio, AIR_HZ)
        assert v.get("am") == "1" and v.get("bcast") == "0", v
        assert v.get("mode") == "fm", v


@pytest.mark.feature("bcast_fm")
def test_broadcast_receiver(radio):
    need_vfo(radio)
    with radio.vfo_session():
        v = tune(radio, BCAST_HZ)
        assert v.get("bcast") == "1" and v.get("am") == "0", v
        assert v.get("bcast_fault") == "0", "the broadcast receiver didn't answer"
    v = radio.core()
    assert v.get("bcast") == "0", "the broadcast receiver stayed on after the VFO left the band"
