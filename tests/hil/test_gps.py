# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The radio's position (docs/spec/position.md): the position service over the debug link, on the radio alone. Nothing
transmits.

The service is there for DMR GPS Info, LRRP, APRS or the GPS receiver, whichever is built, so these tests run on a
build without APRS too (the commercial variant): the pos.* keys and the position settings are in the aprs and aprs_set
view sections of every build with a position service.

- test_position_view: the keys answer.
- test_position_fixed_settings: a fixed position, the source and "Send in DMR calls" round-trip and are what a DMR
  report would carry; the ambiguity blurs it. Every setting is put back.
- test_position_settings_survive_restart (opt-in, --allow-reboot): they are back after a restart, on a build without
  APRS as on one with it.
- test_gps_fix (--manual: the owner takes the radio where it sees the sky): the board GPS is heard and gets a fix.
"""
import time

import pytest

from rfoundry_hil import as_int, skip

POS_AUTO, POS_FIXED = 0, 3
LAT, LON = 33.7490, -84.3880


def _pos(radio):
    v = radio.view(["aprs"])
    v.update(radio.view(["aprs_set"]))
    if v.get("pos.built") != "1":
        skip("not_built", "no position service (needs CONFIG_GPS, CONFIG_APRS, CONFIG_DMR_LRRP or CONFIG_DMR_GPS)")
    return v


def _e7(v, k):
    """A coordinate key as 1e-7 degree (the settings are ints; pos.lat / pos.lon are degrees with 7 decimals)."""
    x = v.get(k, "0")
    return round(float(x) * 1e7) if "." in x else int(x)


def _restore(radio, before):
    radio.link.aprs_set("lat", _e7(before, "set.pos_lat"))
    radio.link.aprs_set("lon", _e7(before, "set.pos_lon"))
    radio.link.aprs_set("pos_src", as_int(before.get("set.aprs_pos_src"), 0))
    radio.link.aprs_set("dmr_gps", as_int(before.get("set.aprs_dmr_gps"), 0))
    radio.link.aprs_set("ambiguity", as_int(before.get("set.pos_ambiguity"), 0))


@pytest.mark.feature("position")
def test_position_view(radio):
    v = _pos(radio)
    for k in ("pos.host_built", "pos.src", "set.aprs_pos_src", "set.aprs_dmr_gps", "set.pos_ambiguity", "set.pos_lat",
              "set.pos_lon"):
        assert k in v, (k, v)
    assert v["pos.src"] in ("none", "gps", "host", "fixed"), v
    # The host half comes with APRS, the GPS keys with the GPS driver
    feats = radio.features()
    assert v["pos.host_built"] == ("1" if "APRS" in feats else "0"), v
    assert ("pos.gps_heard" in v) == ("GPS" in feats), v


@pytest.mark.feature("position")
@pytest.mark.feature("dmr_gps", optional=True)
@pytest.mark.feature("dmr_lrrp", optional=True)
def test_position_fixed_settings(radio):
    """A fixed position set over the debug link is the position in use (what GPS Info and LRRP send), with or without
    APRS in the build."""
    before = _pos(radio)
    try:
        radio.link.aprs_set("lat", LAT)
        radio.link.aprs_set("lon", LON)
        radio.link.aprs_set("pos_src", POS_FIXED)
        radio.link.aprs_set("dmr_gps", 1)
        radio.link.aprs_set("ambiguity", 0)
        v = _pos(radio)
        assert v["pos.src"] == "fixed", v
        assert abs(float(v["pos.lat"]) - LAT) < 2e-6 and abs(float(v["pos.lon"]) - LON) < 2e-6, v
        assert as_int(v["pos.err_m"]) == 200, v           # a typed position
        assert v["set.aprs_pos_src"] == str(POS_FIXED) and v["set.aprs_dmr_gps"] == "1", v
        assert _e7(v, "set.pos_lat") == round(LAT * 1e7) and _e7(v, "set.pos_lon") == round(LON * 1e7), v
        # Ambiguity 2: whole minutes, the middle of the cell, 1852 m
        radio.link.aprs_set("ambiguity", 2)
        v = _pos(radio)
        assert v["set.pos_ambiguity"] == "2" and as_int(v["pos.err_m"]) == 1852, v
        assert abs(float(v["pos.lat"]) - (33 + 44.5 / 60)) < 2e-6, v
        # Out of range values are refused and change nothing
        for field, bad in (("pos_src", 9), ("ambiguity", 5)):
            with pytest.raises(Exception):
                radio.link.aprs_set(field, bad)
        assert _pos(radio)["set.pos_ambiguity"] == "2"
    finally:
        _restore(radio, before)


@pytest.mark.feature("position")
@pytest.mark.reboot
def test_position_settings_survive_restart(radio):
    """The position settings are saved by the position service itself: back after a restart, APRS built or not."""
    before = _pos(radio)
    try:
        radio.link.aprs_set("lat", LAT)
        radio.link.aprs_set("lon", LON)
        radio.link.aprs_set("pos_src", POS_FIXED)
        radio.link.aprs_set("dmr_gps", 1)
        radio.wait_for(lambda s: s.get("settings.dirty") == "0", timeout=5.0, sections=("storage",),
                       what="settings saved")
        radio.link.reboot(0, radio.link.chip()["token"])
        radio.link.close()
        time.sleep(2.0)
        radio.reconnect(20.0)
        radio.wait_for(lambda c: c.get("state") is not None, timeout=5.0)
        v = _pos(radio)
        assert v["set.aprs_pos_src"] == str(POS_FIXED) and v["set.aprs_dmr_gps"] == "1", v
        assert _e7(v, "set.pos_lat") == round(LAT * 1e7) and _e7(v, "set.pos_lon") == round(LON * 1e7), v
        assert v["pos.src"] == "fixed", v
    finally:
        _restore(radio, before)


@pytest.mark.feature("gps", owner="take the radio where it sees the sky, Settings > Location > GPS on, and wait for "
                                  "a fix (Tools > Position)")
@pytest.mark.manual
def test_gps_fix(radio):
    """The board GPS is heard and has a fix; with the source on Auto it is the position in use."""
    v = _pos(radio)
    if "pos.gps_heard" not in v:
        skip("not_built", "built without CONFIG_GPS")
    before = v
    try:
        radio.link.aprs_set("pos_src", POS_AUTO)
        v = radio.wait_for(lambda x: x.get("pos.gps_fix") == "1", timeout=180.0, sections=("aprs",),
                           what="a GPS fix")
        assert v["pos.gps_heard"] == "1" and as_int(v["pos.gps_sats"]) >= 3, v
        assert v["pos.src"] == "gps" and as_int(v["pos.err_m"]) == 20, v
    finally:
        radio.link.aprs_set("pos_src", as_int(before.get("set.aprs_pos_src"), 0))
