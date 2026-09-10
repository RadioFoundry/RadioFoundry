# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""APRS (docs/spec/aprs.md 13). Without --allow-rf only the debug link side runs (the settings and view keys, the RAM
test callsign, host NMEA as the position source; nothing transmits). With --allow-rf the radio sends one position on
the APRS frequency (amateur 2 m: 144.390 MHz, or the region given by ODR_APRS_REGION, e.g. eu for 144.800) and comes
back to its channel. Needs a licensed operator's callsign: ODR_APRS_CALL=<your call> (RAM only, never saved).

Decoding the transmission is done off the radio (an SDR or a second receiver into Direwolf); the test prints what to
look for. Pass criteria: README.md."""
import os
import time

import pytest

REGIONS = {"na": (0, 144390000), "eu": (1, 144800000), "au": (2, 145175000), "nz": (3, 144575000),
           "jp": (4, 144660000), "cn": (5, 144640000)}


def _need_aprs(radio):
    if "APRS" not in radio.features():
        pytest.skip("built without CONFIG_APRS")


def _aprs(radio):
    v = radio.view(["aprs"])
    v.update(radio.view(["aprs_set"]))
    return v


def test_aprs_view_and_settings(radio):
    """The view sections answer, a setting round-trips through the debug link (and is put back)."""
    _need_aprs(radio)
    v = _aprs(radio)
    assert v.get("aprs.built") == "1", v
    for k in ("aprs.block", "aprs.freq_hz", "aprs.pos_src", "aprs.rf_sent", "set.aprs_tx", "set.aprs_region"):
        assert k in v, k
    old = int(v["set.aprs_ssid"])
    try:
        radio.link.aprs_set("ssid", 9 if old != 9 else 8)
        assert _aprs(radio)["set.aprs_ssid"] == str(9 if old != 9 else 8)
    finally:
        radio.link.aprs_set("ssid", old)


def test_aprs_test_call_and_nmea(radio):
    """A RAM test callsign clears the "no callsign" block; host NMEA becomes the position (source "host")."""
    _need_aprs(radio)
    before = _aprs(radio)
    try:
        radio.link.aprs_test_call("N0TST")
        radio.link.aprs_set("pos_src", 2)       # Computer (host NMEA)
        radio.link.aprs_nmea("$GPRMC,123519,A,3344.940,N,08423.280,W,000.0,000.0,101026,,")
        v = radio.wait_for(lambda x: x.get("aprs.pos_valid") == "1", sections=("aprs",), what="host position")
        assert v["aprs.pos_src"] == "host" and v["aprs.call"].startswith("N0TST"), v
        assert abs(float(v["aprs.lat"]) - 33.749) < 1e-3 and abs(float(v["aprs.lon"]) + 84.388) < 1e-3, v
        assert v["aprs.block"] in ("ok", "not_amateur", "no_afsk"), v
    finally:
        radio.link.aprs_test_call("")
        radio.link.aprs_set("pos_src", int(before.get("set.aprs_pos_src", 0)))


@pytest.mark.radio_tx
def test_aprs_send_on_air(radio):
    """One position on the APRS frequency at low power: the radio leaves its channel, listens, sends about 1 s and
    comes back (rf_sent + 1, the channel unchanged)."""
    _need_aprs(radio)
    call = os.environ.get("ODR_APRS_CALL")
    if not call:
        pytest.skip("set ODR_APRS_CALL to your callsign (amateur transmission)")
    region, hz = REGIONS[os.environ.get("ODR_APRS_REGION", "na")]
    if "FM" not in radio.features():
        pytest.skip("built without FM")
    before = _aprs(radio)
    radio.go_home()
    radio.lowest_power()
    ch_before = radio.view(["channel"])
    try:
        radio.link.aprs_test_call(call)
        radio.link.aprs_set("region", region)
        radio.link.aprs_set("lat", 33.7490)
        radio.link.aprs_set("lon", -84.3880)
        radio.link.aprs_set("pos_src", 3)
        radio.link.aprs_set("via", 0)
        v = _aprs(radio)
        assert v["aprs.block"] == "ok" and int(v["aprs.freq_hz"]) == hz, v
        sent0 = int(v["aprs.rf_sent"])
        print(f"listen on {hz / 1e6:.3f} MHz FM (Direwolf): expect {call.upper()}-{v['set.aprs_ssid']}>APZODR,...")
        radio.link.aprs_send_now()
        v = radio.wait_for(lambda x: int(x.get("aprs.rf_sent", 0)) > sent0 or int(x.get("aprs.rf_busy", 0)) >
                           int(before.get("aprs.rf_busy", 0)), timeout=15, sections=("aprs",), what="APRS sent")
        print("aprs view:", {k: v[k] for k in v if k.startswith("aprs.")})
        assert int(v["aprs.rf_sent"]) == sent0 + 1, "channel busy (rf_busy) or the transmission failed"
        time.sleep(0.5)
        v = radio.wait_for(lambda x: x.get("aprs.rf_active") == "0", sections=("aprs",), what="back on the channel")
        ch = radio.view(["channel"])
        assert ch.get("ch.rx_hz") == ch_before.get("ch.rx_hz"), (ch_before, ch)
    finally:
        radio.link.aprs_test_call("")
        radio.link.aprs_set("region", int(before.get("set.aprs_region", 0)))
        radio.link.aprs_set("pos_src", int(before.get("set.aprs_pos_src", 0)))
