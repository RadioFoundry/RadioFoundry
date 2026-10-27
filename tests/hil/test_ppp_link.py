# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""PPP on the serial host port (docs/spec/ppp.md), radio only, without pppd or root: tests/hil/ppp_client.py is the
peer. It opens LCP and IPCP on the debug link's own port as pppd would (noipdefault), pings the radio (a short echo
and a 500 byte one carrying every byte value, so the escapes are crossed both ways), runs the debug link over UDP
3334 through the link (the network personality, ppp.md 4), then hangs up with LCP Terminate and checks the port is
the debug link's again. Nothing transmits: the radio's own address is answered by the gateway itself.

Skips on a build without CONFIG_NET_PPP, on a radio without a DMR ID (no address to give), and on a debug link that
isn't a serial port."""
import time

import pytest

from rfoundry_hil import skip
from rfoundry_debug import DebugLink
from ppp_client import PppClient, PppError, UdpPort

LCP_OPENED = 9


@pytest.mark.feature("net_ppp")
def test_ppp_link(radio):
    if "NET_PPP" not in radio.features():
        skip("not_built", "built without CONFIG_NET_PPP")
    if str(radio.port).startswith("udp:"):
        skip("state", "the debug link runs over UDP: PPP needs the serial port")
    own = int(radio.view(["dmr"]).get("dmr.own_id", "0") or 0)
    if own == 0:
        skip("state", "the radio has no DMR ID (dmr.own_id=0): IPCP has no address to give")
    board = radio.version().get("board")

    port = radio.link.s
    port.reset_input_buffer()
    ppp = PppClient(port)
    try:
        try:
            local, peer = ppp.open()
        except PppError as e:
            pytest.fail(f"{e} (bad frames {ppp.bad_frames})")
        print(f"PPP up: host {local}, radio {peer}")
        want = f"12.{(own >> 16) & 0xFF}.{(own >> 8) & 0xFF}.{own & 0xFF}"
        assert local == want, f"IPCP gave the host {local}, expected 12.<DMR ID> = {want}"
        assert peer and peer != local

        rtts = [ppp.ping(56, seq=1), ppp.ping(500, seq=2), ppp.ping(56, seq=3)]
        print("ping round trips (s):", rtts)
        assert all(r is not None for r in rtts), f"pings lost: {rtts}"

        dl = DebugLink(s=UdpPort(ppp), timeout=3.0)
        v = dl.version()
        assert v.get("board") == board, f"over UDP the radio says board {v.get('board')}"
        h = dl.view(["hotspot"])
        print({k: h.get(k) for k in ("usb.network", "usb.net_up", "ppp.lcp", "ppp.ipcp", "ppp.rx_errors")})
        assert int(h.get("ppp.lcp", 0)) == LCP_OPENED and int(h.get("ppp.ipcp", 0)) == LCP_OPENED
        assert h.get("usb.net_up") == "1"
        assert int(h.get("ppp.rx_errors", 0)) == 0, "the radio counted bad frames from a clean peer"
        assert ppp.bad_frames == 0, "frames from the radio failed their FCS"
    finally:
        hung_up = ppp.close()
        time.sleep(4.0)           # the radio leaves PPP one restart time after the Terminate-Ack (ppp.md 4)
        port.reset_input_buffer()
    assert hung_up, "no Terminate-Ack from the radio"
    radio.ping()
    assert radio.view(["hotspot"]).get("ppp.running") == "0", "the radio still runs PPP after the hang-up"
