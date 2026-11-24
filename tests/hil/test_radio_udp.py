# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Zone and channel control over UDP (opt-in: --allow-usb-switch; firmware with CONFIG_NET_RADIO_UDP,
docs/spec/tcp-services.md 5.2) in the USB network personality: where the radio is agrees with the debug link, a step
and a select move it, each move comes as a notice, and the radio is left where it was.

Every datagram goes to the radio's own address on the link and to its control port only: the IP gateway sends
anything else addressed into the individual range over the air. Nothing is transmitted."""
import json
import socket
import time

import pytest

from rfoundry_hil import skip, usb_network

PORT = 4191      # NET_RADIO_UDP_PORT


class _Ctl:
    """A control client: one socket, bound on the host's end of the link, so replies and notices both come to it."""

    def __init__(self, host_ip, radio_ip):
        self.radio = (radio_ip, PORT)
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind((host_ip, 0))

    def recv(self, seconds=1.0):
        self.s.settimeout(seconds)
        data, src = self.s.recvfrom(2048)
        assert src == self.radio, src
        return json.loads(data)

    def ask(self, req):
        self.s.sendto(req.encode(), self.radio)
        return self.recv()

    def wait_at(self, zone, pos, seconds=3.0):
        """The notices until the radio is at zone, pos (or the time is up): where it came to"""
        end = time.monotonic() + seconds
        at = None
        while time.monotonic() < end and at != (zone, pos):
            try:
                n = self.recv(max(0.05, end - time.monotonic()))
            except socket.timeout:
                break
            at = (n["zone"], n["pos"])
        if at != (zone, pos):
            n = self.ask("select")      # no notice: it didn't move, or one was lost
            at = (n["zone"], n["pos"])
        return at

    def close(self):
        self.s.close()


@pytest.fixture(scope="module")
def udp_ctl(radio, request):
    if not request.config.getoption("--allow-usb-switch"):
        skip("opt_in", "needs --allow-usb-switch")
    if "NET_RADIO_UDP" not in radio.features():
        skip("not_built", "built without CONFIG_NET_RADIO_UDP")
    if "USB_NETWORK" not in radio.features():
        skip("not_built", "no USB network personality (a PPP-only radio: run it over PPP by hand)")
    own_id = int(radio.view(["dmr"]).get("dmr.own_id", "0"))
    assert 0 < own_id < (1 << 24), "no DMR ID: the gateway can't make an address"
    with usb_network(radio, own_id) as (link, host_ip, radio_ip):
        c = _Ctl(host_ip, radio_ip)
        try:
            yield c, link
        finally:
            c.close()


@pytest.mark.feature("net_radio_udp")
@pytest.mark.usb_switch
def test_radio_udp(udp_ctl):
    c, link = udp_ctl
    start = c.ask("select")
    assert start["lease"] == 120 and "err" not in start, start
    if start["vfo"]:
        skip("bench", "the radio is in the VFO: put it on a channel")
    ch = link.view(["core", "channel"])
    assert start["rx_hz"] == int(ch["ch.rx_hz"]) and start["name"] == ch["ch.name"], (start, ch)
    assert start["zone_name"] == ch["zone_name"] and start["number"] == int(ch["channel_number"]), (start, ch)
    size, nzones = int(ch["zone_size"]), int(ch["zone_count"])
    here = (start["zone"], start["pos"])
    try:
        if size > 1:
            to = (start["pos"] + 1) % size
            t0 = time.monotonic()
            assert "err" not in c.ask("step?channel=1")
            assert c.wait_at(start["zone"], to) == (start["zone"], to)
            print(f"notice {(time.monotonic() - t0) * 1000:.0f} ms after the step")
            assert "err" not in c.ask("step?channel=-1")
            assert c.wait_at(*here) == here
        if nzones > 1:
            other = (start["zone"] + 1) % nzones
            assert "err" not in c.ask(f"select?zone={other}&pos=0")
            assert c.wait_at(other, 0) == (other, 0)
        assert c.ask("select?zone=65000")["err"] == 400
    finally:
        c.ask(f"select?zone={start['zone']}&pos={start['pos']}")
        assert c.wait_at(*here) == here
    v = link.view(["net_svc"])
    assert int(v["radio_udp.notify"]) != 0 and int(v["radio_udp.bad"]) >= 1, v
    assert c.ask("select?notify=0")["lease"] == 0
    assert int(link.view(["net_svc"])["radio_udp.notify"]) == 0
