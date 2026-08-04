# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""FM receive, no transmission: the VFO on NOAA weather radio (162.550 MHz by default) opens the squelch or shows an
RSSI above the floor within a few seconds. Depends on reception at the bench."""
import time

import pytest


def test_noaa_receive(radio, pytestconfig):
    if "VFO_FM" not in radio.features():
        pytest.skip("built without the FM VFO")
    radio.go_home()
    if radio.screen() == "hotspot":
        pytest.skip("the radio is in hotspot mode")
    hz = round(pytestconfig.getoption("--noaa-mhz") * 1e6)
    floor = pytestconfig.getoption("--rssi-floor")
    with radio.vfo_session():
        radio.link.vfo_set(hz)
        radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="VFO tuned")
        best, opened, end = -999, False, time.monotonic() + 10
        while time.monotonic() < end and not opened:
            v = radio.core()
            best = max(best, int(v["rssi_dbm"]))
            opened = v["state"] == "rx"
            time.sleep(0.2)
        assert opened or best > floor, (f"no signal on {hz / 1e6:.3f} MHz in 10 s: best RSSI {best} dBm "
                                        f"(floor {floor}); squelch {radio.core()['squelch']}")
