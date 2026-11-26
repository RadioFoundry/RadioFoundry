# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Zone, channel and the board GPS over /ws (opt-in: --allow-usb-switch; firmware with CONFIG_NET_WEBCTL,
docs/spec/tcp-services.md 5.1), in the USB network personality or over PPP on a radio without one.

  test_webctl_zone_channel  the hello's proto and caps; the status says where the radio is, as the debug link does;
                            with "status?changes" a step and a select each bring one status, and the radio is left
                            where it was
  test_webctl_gps           "gps": one fix per GPS second near the position the radio reports, none after "gps?off".
                            Needs a fix (the radio sees the sky)

Everything goes to the radio's own address on the link. Nothing is transmitted."""
import os
import sys
import time

import pytest

from rfoundry_hil import skip
from test_net_services import network_mode

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import rfoundry_ws  # noqa: E402


@pytest.fixture(scope="module")
def ws(radio, request):
    if not request.config.getoption("--allow-usb-switch"):
        skip("opt_in", "needs --allow-usb-switch")
    if "NET_WEBCTL" not in radio.features():
        skip("not_built", "built without CONFIG_NET_WEBCTL")
    with network_mode(radio) as (link, radio_ip, _):
        with rfoundry_ws.WebControl(radio_ip) as w:
            yield w, link


def _status(w, seconds=3.0, until=None):
    """The next status (or the first that satisfies until), None if none came"""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        m = w.recv(max(0.05, end - time.monotonic()))
        if m is not None and "zone" in m and (until is None or until(m)):
            return m
    return None


@pytest.mark.feature("net_webctl")
@pytest.mark.usb_switch
def test_webctl_zone_channel(ws):
    w, link = ws
    assert w.hello["proto"] == 1 and {"select", "step", "status"} <= set(w.hello["caps"]), w.hello
    assert w.hello["control"], "not the host: the radio refuses zone and channel"
    w.send("status?changes")
    start = _status(w)
    assert start is not None, "no status"
    if start["vfo"]:
        skip("bench", "the radio is in the VFO: put it on a channel")
    ch = link.view(["core", "channel"])
    assert start["zone_name"] == ch["zone_name"] and start["channel_number"] == int(ch["channel_number"]), (start, ch)
    assert start["rx_hz"] == int(ch["ch.rx_hz"]), (start, ch)
    size, nzones = int(ch["zone_size"]), int(ch["zone_count"])
    here = lambda m: (m["zone"], m["pos"]) == (start["zone"], start["pos"])  # noqa: E731
    # Still: nothing more comes (the clock and the signal don't send it)
    assert _status(w, 2.0) is None, "a status without a change"
    try:
        if size > 1:
            to = (start["pos"] + 1) % size
            t0 = time.monotonic()
            w.send("step?channel=1")
            m = _status(w, until=lambda m: m["pos"] == to)
            assert m is not None and m["zone"] == start["zone"], m
            print(f"status {(time.monotonic() - t0) * 1000:.0f} ms after the step")
            w.send("step?channel=-1")
            assert _status(w, until=here) is not None
        if nzones > 1:
            other = (start["zone"] + 1) % nzones
            w.send(f"select?zone={other}&pos=0")
            assert _status(w, until=lambda m: (m["zone"], m["pos"]) == (other, 0)) is not None
        w.send("select?zone=65000")
        m = w.recv(2.0)
        while m is not None and "err" not in m:
            m = w.recv(2.0)
        assert m == {"err": 400}, m
    finally:
        w.send(f"select?zone={start['zone']}&pos={start['pos']}")
        time.sleep(1.0)
        back = link.view(["core", "channel"])
        assert (back["zone_name"], back["channel_number"]) == (ch["zone_name"], ch["channel_number"]), back
    v = link.view(["net_svc"])
    assert int(v["radio_ctl.selects"]) >= 1, v


@pytest.mark.feature("net_webctl")
@pytest.mark.usb_switch
def test_webctl_gps(ws, radio):
    w, link = ws
    if "gps" not in w.hello["caps"]:
        skip("no_hw", "no GPS (or built without CONFIG_GPS)")
    pos = link.view(["aprs"])
    if pos.get("pos.gps_fix") != "1":
        skip("bench", "no GPS fix: take the radio where it sees the sky")
    w.send("status?changes")
    w.send("gps")
    fixes = []
    try:
        end = time.monotonic() + 15
        while time.monotonic() < end:
            m = w.recv(max(0.05, end - time.monotonic()))
            if m is not None and "lat" in m:
                fixes.append(m)
        assert link.view(["net"]).get("webctl.gps") == "1"
    finally:
        w.send("gps?off")
    assert len(fixes) >= 10, f"{len(fixes)} fixes in 15 s"
    utc = [f["utc"] for f in fixes]
    assert len(set(utc)) == len(utc), "a GPS second sent twice"
    f = fixes[-1]
    assert f["sats"] >= 3 and -90 <= f["lat"] <= 90 and -180 <= f["lon"] <= 180, f
    # the view's position has the ambiguity setting applied: near, not equal
    assert abs(f["lat"] - float(pos["pos.lat"])) < 0.2 and abs(f["lon"] - float(pos["pos.lon"])) < 0.2, (f, pos)
    time.sleep(1.5)
    while w.recv(0.1) is not None:
        pass
    late = []
    end = time.monotonic() + 2.5
    while time.monotonic() < end:
        m = w.recv(0.2)
        if m is not None and "lat" in m:
            late.append(m)
    assert not late, f"fixes after gps?off: {late}"
