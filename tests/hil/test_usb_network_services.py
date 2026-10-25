# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The host port's protocols over UDP 3334 in the USB network personality (docs/spec/usb-network.md; opt-in:
--allow-usb-switch). One switch to the network personality for the module, back to serial at the end.

- test_net_split_protocol: the split duplex extension (split-duplex.md 3) over UDP: GET_VERSION " SPLIT1", INFO,
  SET_ROLE 2 and back to 0, STATUS. No RF.
- test_net_split_rx_half_labels (rf, bench_service: --allow-rf --allow-service-stop): the RX half's labels against a
  bench call, as test_split_duplex.py does on the serial port. Nothing is transmitted by the radio.
- test_net_kiss: the KISS TNC (aprs.md 9) over UDP: with KISS on and the host as position source, an empty DATA frame
  puts the host link in TNC mode and a SetHardware 'N' $GPRMC frame gives the radio a host position. APRS TX stays as
  it was (the test skips if APRS TX is on, so nothing can beacon). The settings it changes are restored.
"""
import ipaddress
import os
import socket
import sys
import time
from datetime import datetime, timezone

import pytest

from rfoundry_hil import DebugLink, skip
from test_split_duplex import check_rx_labels, rx_half_labels
from test_usb_network import INDIVIDUAL_NET, _serial_back, _udp_link

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))


class NetRadio:
    def __init__(self, ip):
        self.ip = ip
        self.port = f"udp:{ip}"
        self.link = None

    def open(self):
        if self.link is None:
            self.link = _udp_link(self.ip, 15)
            assert self.link is not None, f"no debug link over UDP at {self.ip}"
        return self.link

    def close(self):
        # One protocol at a time on UDP 3334: the radio answers the sender of the last frame
        if self.link is not None:
            self.link.close()
            self.link = None

    def leave_hotspot(self):
        link = self.open()
        link.hotspot_exit()
        end = time.monotonic() + 5
        while time.monotonic() < end and link.view(["hotspot"]).get("hostlink") != "idle":
            time.sleep(0.2)


@pytest.fixture(scope="module")
def net(radio):
    # Its own debug link, not the session's `radio`: the per-test restore (conftest) needs the serial port, which is
    # gone while the radio is a network adapter. The session's link is closed meanwhile and reopened afterwards (the
    # port re-enumerates: a link kept open would be dead for the tests after this module)
    with radio.released() as port:
        serial_link = DebugLink(port, timeout=1.5)
        feats = set(serial_link.version().get("features", "").split(","))
        if "DMR_IP_GATEWAY" not in feats:
            serial_link.close()
            skip("not_built", "built without CONFIG_DMR_IP_GATEWAY")
        own_id = int(serial_link.view(["dmr"]).get("dmr.own_id", "0"))
        host_ip = ipaddress.IPv4Address(INDIVIDUAL_NET | own_id)
        radio_ip = str(ipaddress.IPv4Address(int(host_ip) ^ 1))
        serial_link.usb_network(True)
        serial_link.close()
        n = NetRadio(radio_ip)
        n.features = feats
        try:
            n.open()
            yield n
        finally:
            try:
                n.open().usb_network(False)
            except Exception:
                pass
            n.close()
            assert _serial_back(30), "the radio didn't come back as a serial port"
            time.sleep(2.0)


@pytest.mark.feature("split_duplex", optional=True)
@pytest.mark.usb_switch
def test_net_split_protocol(net, pytestconfig):
    if "HOTSPOT_SPLIT" not in net.features:
        skip("not_built", "built without the split duplex extension")
    import split_duplex_host as sdh
    hz = round(pytestconfig.getoption("--dmr-freq") * 1e6)
    net.close()
    r = sdh.Radio(net.port, "net")
    try:
        ver, info = sdh.require_split(r)
        assert ver == 1 and info["offered"] & 3 == 3, info
        sdh.setup_half(r, hz, hz, 0, 1)
        assert sdh.set_role(r, sdh.ROLE_RX)["role"] == sdh.ROLE_RX
        st = sdh.get_status(r)
        assert st["role"] == sdh.ROLE_RX, st
        assert sdh.set_role(r, sdh.ROLE_NORMAL)["role"] == sdh.ROLE_NORMAL
        print(f"over UDP: INFO {info}, STATUS {st}")
    finally:
        r.close()
    net.leave_hotspot()


@pytest.mark.feature("split_duplex", optional=True)
@pytest.mark.usb_switch
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_net_split_rx_half_labels(net, sdr_bench, pytestconfig):
    if "HOTSPOT_SPLIT" not in net.features:
        skip("not_built", "built without the split duplex extension")
    freq, cc, tg = (pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc"),
                    pytestconfig.getoption("--dmr-tg"))
    net.close()
    with sdr_bench.lease():
        got = rx_half_labels(net.port, sdr_bench, freq, cc, tg)
    net.leave_hotspot()
    check_rx_labels(*got)


def _nmea(body):
    cs = 0
    for c in body.encode("ascii"):
        cs ^= c
    return f"${body}*{cs:02X}\r\n".encode("ascii")


def _kiss(cmd, payload=b""):
    esc = payload.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc")
    return b"\xc0" + bytes([cmd]) + esc + b"\xc0"


@pytest.mark.feature("aprs")
@pytest.mark.usb_switch
def test_net_kiss(net):
    if "APRS" not in net.features:
        skip("not_built", "built without CONFIG_APRS")
    link = net.open()
    s = link.view(["aprs_set"])
    if s.get("set.aprs_tx", "0") != "0":
        skip("state", "APRS TX is on: not feeding it a position (it would beacon)")
    old_kiss, old_src = int(s.get("set.aprs_kiss", "0")), int(s.get("set.aprs_pos_src", "0"))
    from rfoundry_debug import APRS_POS_SRCS
    link.aprs_set("kiss", 1)
    link.aprs_set("pos_src", APRS_POS_SRCS["host"])
    net.close()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        now = datetime.now(timezone.utc)
        rmc = _nmea(f"GPRMC,{now:%H%M%S}.00,A,3345.0000,N,08423.0000,W,0.0,0.0,{now:%d%m%y},,,A")
        sock.sendto(_kiss(0x00), (net.ip, 3334))            # an empty DATA frame: TNC mode, nothing to send
        time.sleep(0.3)
        sock.sendto(_kiss(0x06, b"N" + rmc), (net.ip, 3334))   # SetHardware 'N': an NMEA sentence
        time.sleep(1.0)
    finally:
        sock.close()
    link = net.open()
    hs, a = link.view(["hotspot"]), link.view(["aprs"])
    print(f"after KISS over UDP: hostlink {hs.get('hostlink')} (left {hs.get('hostlink.left')}), "
          f"aprs.pos_src {a.get('aprs.pos_src')} valid {a.get('aprs.pos_valid')} age {a.get('aprs.pos_age_s')}")
    try:
        assert a.get("aprs.pos_src") == "host" and a.get("aprs.pos_valid") == "1", a
        assert int(a.get("aprs.pos_age_s", "99")) < 10, a
    finally:
        link.aprs_set("kiss", old_kiss)
        link.aprs_set("pos_src", old_src)
