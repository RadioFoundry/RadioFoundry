# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""PPP on the serial host port (docs/spec/ppp.md), radio only, without pppd or root: tests/hil/ppp_client.py is the
peer. It opens LCP and IPCP on the debug link's own port as pppd would (noipdefault), pings the radio (a short echo
and a 500 byte one carrying every byte value, so the escapes are crossed both ways), runs the debug link over UDP
3334 through the link (the network personality, ppp.md 4), reaches the network services that ride it (HTTP over TCP,
SNMP, TFTP), then hangs up with LCP Terminate and checks the port is the debug link's again. Nothing transmits: the
radio's own address is answered by the gateway itself, and the services only read.

Skips on a build without CONFIG_NET_PPP, on a radio without a DMR ID (no address to give), and on a debug link that
isn't a serial port."""
import json
import socket
import time
import types

import pytest

from rfoundry_hil import skip
from rfoundry_debug import DebugLink
from ppp_client import PppClient, PppError, UdpPort, http_get, udp_socket
import rfoundry_snmp
import tftp_client

LCP_OPENED = 9


@pytest.fixture(scope="module")
def ppp(radio):
    feats = radio.features()
    if "NET_PPP" not in feats:
        skip("not_built", "built without CONFIG_NET_PPP")
    if str(radio.port).startswith("udp:"):
        skip("state", "the debug link runs over UDP: PPP needs the serial port")
    own = int(radio.view(["dmr"]).get("dmr.own_id", "0") or 0)
    if own == 0:
        skip("state", "the radio has no DMR ID (dmr.own_id=0): IPCP has no address to give")
    board = radio.version().get("board")      # now: a debug link frame on the port would end PPP (ppp.md 4)
    port = radio.link.s
    port.reset_input_buffer()
    client = PppClient(port)
    try:
        try:
            client.open()
        except PppError as e:
            pytest.fail(f"{e} (bad frames {client.bad_frames})")
        print(f"PPP up: host {client.local_ip}, radio {client.peer_ip}")
        yield {"ppp": client, "own": own, "features": feats, "board": board}
    finally:
        hung_up = client.close()
        time.sleep(4.0)           # the radio leaves PPP one restart time after the Terminate-Ack (ppp.md 4)
        port.reset_input_buffer()
    assert hung_up, "no Terminate-Ack from the radio"
    radio.ping()
    assert radio.view(["hotspot"]).get("ppp.running") == "0", "the radio still runs PPP after the hang-up"


@pytest.mark.feature("net_ppp")
def test_ppp_link(ppp):
    c, own = ppp["ppp"], ppp["own"]
    want = f"12.{(own >> 16) & 0xFF}.{(own >> 8) & 0xFF}.{own & 0xFF}"
    assert c.local_ip == want, f"IPCP gave the host {c.local_ip}, expected 12.<DMR ID> = {want}"
    assert c.peer_ip and c.peer_ip != c.local_ip

    rtts = [c.ping(56, seq=1), c.ping(500, seq=2), c.ping(56, seq=3)]
    print("ping round trips (s):", rtts)
    assert all(r is not None for r in rtts), f"pings lost: {rtts}"

    dl = DebugLink(s=UdpPort(c), timeout=3.0)
    v = dl.version()
    assert v.get("board") == ppp["board"], f"over UDP the radio says board {v.get('board')}"
    h = dl.view(["hotspot"])
    print({k: h.get(k) for k in ("usb.network", "usb.net_up", "ppp.lcp", "ppp.ipcp", "ppp.rx_errors")})
    assert int(h.get("ppp.lcp", 0)) == LCP_OPENED and int(h.get("ppp.ipcp", 0)) == LCP_OPENED
    assert h.get("usb.net_up") == "1"
    assert int(h.get("ppp.rx_errors", 0)) == 0, "the radio counted bad frames from a clean peer"
    assert c.bad_frames == 0, "frames from the radio failed their FCS"


@pytest.mark.feature("net_ppp", "net_http")
def test_ppp_http(ppp):
    if "NET_HTTP" not in ppp["features"]:
        skip("not_built", "built without CONFIG_NET_HTTP")
    c = ppp["ppp"]
    st, hd, body = http_get(c, "/status")
    assert st == 200 and hd.get("content-type") == "application/json", (st, hd)
    j = json.loads(body)
    print("status:", {k: j.get(k) for k in ("radio", "state", "tcp_open")})
    assert j["radio"] and j["state"] in ("idle", "rx", "tx", "tx_denied"), j
    st, hd, body = http_get(c, "/nothing")
    assert st == 404


@pytest.mark.feature("net_ppp", "snmp")
def test_ppp_snmp(ppp):
    if "SNMP" not in ppp["features"]:
        skip("not_built", "built without CONFIG_SNMP")
    c = ppp["ppp"]
    m = rfoundry_snmp.Manager(c.peer_ip, timeout=3.0)
    m.sock.close()
    m.sock = udp_socket(c)
    m.sock.settimeout(3.0)
    vbs = m.get("sysName.0", "sysDescr.0")
    print("snmp:", [(rfoundry_snmp.name_of(o), v) for o, _, v in vbs])
    assert len(vbs) == 2 and all(v not in (None, b"", "") for _, _, v in vbs), vbs


@pytest.mark.feature("net_ppp", "tftp")
def test_ppp_tftp_read(ppp, monkeypatch):
    if "TFTP" not in ppp["features"]:
        skip("not_built", "built without CONFIG_TFTP")
    c = ppp["ppp"]
    # The client's socket module, for this test only: its sockets are the PPP link's
    monkeypatch.setattr(tftp_client, "socket", types.SimpleNamespace(
        socket=lambda *a, **k: udp_socket(c), AF_INET=socket.AF_INET, SOCK_DGRAM=socket.SOCK_DGRAM,
        timeout=socket.timeout))
    info = tftp_client.get(c.peer_ip, "info.txt", blksize=512, timeout=2.0).decode(errors="replace")
    print("info.txt:", info.strip().replace("\n", " | ")[:200])
    assert str(ppp["own"]) in info, "info.txt doesn't name the radio's DMR ID"
