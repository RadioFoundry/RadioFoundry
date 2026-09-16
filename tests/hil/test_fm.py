# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""FM receive, no transmission: the VFO on NOAA weather radio (162.550 MHz by default) opens the squelch or shows an
RSSI above the floor within a few seconds. Depends on reception at the bench.

test_rx_tone_squelch: CTCSS / DCS decode, from the side we can test without a tone source (the bench has no FM
transmitter, and NOAA sends no sub-audio tone): on NOAA with the squelch opening (stepped down from the radio's
level as far as 1 until it does), an RX tone of CTCSS 100.0 Hz and
then DCS D023N must keep the audio muted (tone_ok=0, the state never rx, for 5 s each) while the carrier is there;
with the RX tone back to none the squelch opens again within 10 s. The VFO's RX tone is put back."""
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


def test_rx_tone_squelch(radio, pytestconfig):
    if "VFO_FM" not in radio.features() or "SUBTONE" not in radio.features():
        pytest.skip("built without the FM VFO or CTCSS / DCS")
    radio.go_home()
    if radio.screen() == "hotspot":
        pytest.skip("the radio is in hotspot mode")
    hz = round(pytestconfig.getoption("--noaa-mhz") * 1e6)
    L = radio.link
    with radio.vfo_session():
        L.vfo_set(hz)
        radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="VFO tuned")
        tone0 = radio.view(["channel"])["ch.rx_tone"]
        try:
            L.tone(2, "none", 0)
            radio.wait_for(lambda v: v.get("ch.rx_tone") == "none:0", sections=("channel",), what="no RX tone")
            # the squelch stepped down (restore_radio puts it back) until NOAA opens it, but never to 0 (always open)
            while True:
                try:
                    radio.wait_for(lambda v: v.get("state") == "rx", 4, interval=0.2, what="NOAA opens the squelch")
                    break
                except AssertionError:
                    if int(radio.core()["squelch"]) <= 1:
                        pytest.skip(f"NOAA on {hz / 1e6:.3f} MHz doesn't open the squelch at level 1: nothing to mute")
                    L.squelch_step(-1)
            print(f"NOAA opens the squelch at level {radio.core()['squelch']}")
            for kind, value, name in (("ctcss", 1000, "CTCSS 100.0"), ("dcs", 0o23, "DCS D023N")):
                L.tone(2, kind, value)
                radio.wait_for(lambda v: v.get("ch.rx_tone") == f"{kind}:{value}", sections=("channel",),
                               what=f"RX tone {name}")
                time.sleep(0.5)          # the tone debounce (8 polls) after the change
                states, rssi, end = set(), [], time.monotonic() + 5
                while time.monotonic() < end:
                    v = radio.core()
                    states.add((v.get("state"), v.get("tone_ok")))
                    rssi.append(int(v["rssi_dbm"]))
                    time.sleep(0.1)
                print(f"{name}: states {sorted(states)}, RSSI {min(rssi)}..{max(rssi)} dBm")
                assert all(st != "rx" for st, _ in states), f"{name}: NOAA (no tone) opened the audio: {states}"
                assert all(t == "0" for _, t in states), f"{name}: a tone decoded on NOAA: {states}"
                L.tone(2, "none", 0)
                radio.wait_for(lambda v: v.get("state") == "rx", 10, interval=0.2, what="open again without a tone")
        finally:
            kind, value = tone0.split(":")
            L.tone(2, kind, int(value))
            radio.wait_for(lambda v: v.get("ch.rx_tone") == tone0, sections=("channel",), what="RX tone restored")
