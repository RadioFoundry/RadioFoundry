# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The radio's TCP services in the USB network personality (opt-in: --allow-usb-switch; firmware with CONFIG_NET_TCP,
docs/spec/tcp-services.md): the HTTP status page and JSON, KISS over TCP, rigctld, and (only with
RFOUNDRY_HIL_APRSIS_SERVER=a.b.c.d[:port] and NAT on the host, spec 7.1) the APRS-IS client.

The radio switches to the network adapter for the module and back to serial at the end, as test_usb_network does.
Nothing is transmitted: the KISS frame sent has a foreign source callsign, which the APRS engine refuses (counted, not
sent); rigctld only reads and sets the VFO (the radio's channel / VFO state is restored); PTT is not touched. An
own-callsign KISS frame on air is test_aprs.py's test_aprs_tx_link_answers[kiss_tcp]."""
import gzip
import ipaddress
import json
import os
import socket
import struct
import subprocess
import time
import urllib.error
import urllib.request

import pytest

from rfoundry_hil import DebugLink, DebugLinkError, find_ports, skip

INDIVIDUAL_NET = 0x0C000000      # 12.0.0.0/8, IPGW_INDIVIDUAL_NET
FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD


def _udp_link(ip, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            link = DebugLink(f"udp:{ip}", timeout=1.0)
            link.ping()
            return link
        except (DebugLinkError, OSError):
            time.sleep(0.5)
    return None


def _serial_back(timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if find_ports():
            return True
        time.sleep(0.5)
    return False


@pytest.fixture(scope="module")
def net(radio, request):
    if not request.config.getoption("--allow-usb-switch"):
        skip("opt_in", "needs --allow-usb-switch")
    feats = radio.features()
    if "NET_TCP" not in feats:
        skip("not_built", "built without CONFIG_NET_TCP")
    own_id = int(radio.view(["dmr"]).get("dmr.own_id", "0"))
    assert 0 < own_id < (1 << 24), "no DMR ID: the gateway can't make an address"
    radio_ip = str(ipaddress.IPv4Address((INDIVIDUAL_NET | own_id) ^ 1))
    radio.link.usb_network(True)
    radio.link.close()
    udp = _udp_link(radio_ip, 30)
    try:
        assert udp is not None, f"no debug link over UDP at {radio_ip} within 30 s (DHCP on the host's adapter?)"
        yield {"ip": radio_ip, "link": udp, "features": feats}
    finally:
        if udp is not None:
            try:
                udp.usb_network(False)
            except (DebugLinkError, OSError):
                pass
            udp.close()
        if not _serial_back(20):
            again = _udp_link(radio_ip, 10)
            if again is not None:
                again.usb_network(False)
                again.close()
            assert _serial_back(20), "the radio didn't come back as a serial port"
        for _ in range(40):
            try:
                radio.link = DebugLink(radio.port, timeout=1.5)
                break
            except DebugLinkError:
                time.sleep(0.25)


def _get(ip, path, timeout=10):
    with urllib.request.urlopen(f"http://{ip}{path}", timeout=timeout) as r:
        body = r.read()
        if r.headers.get("Content-Encoding", "") == "gzip":     # the static files (CONFIG_NET_HTTP_GZIP)
            body = gzip.decompress(body)
        return r.status, r.headers.get("Content-Type", ""), body


def _net_view(link):
    return link.view(["net"])


@pytest.mark.feature("net_tcp", "net_http")
@pytest.mark.usb_switch
def test_http_status(net):
    if "NET_HTTP" not in net["features"]:
        skip("not_built", "built without CONFIG_NET_HTTP")
    ip, link = net["ip"], net["link"]
    st, ct, body = _get(ip, "/")
    assert st == 200 and ct.startswith("text/html") and body.startswith(b"<!DOCTYPE html>")
    st, ct, body = _get(ip, "/status")
    j = json.loads(body)
    assert ct == "application/json" and j["radio"] and j["tcp_open"] >= 1, j
    assert j["state"] in ("idle", "rx", "tx", "tx_denied"), j
    # The view sections as JSON agree with the debug link's
    st, ct, body = _get(ip, "/api/view?section=net")
    v = json.loads(body)
    assert v["section"] == "net" and v["tcp.accepted"] >= 3, v
    names = json.loads(_get(ip, "/api/view")[2])["sections"]
    assert "core" in names and "net" in names
    # The screen as a BMP of the frame buffer's size, at the panel's own depth (core/net/http.c): a palette after the
    # 54 header octets for 1 and 4 bpp, the three RGB565 masks for 16; rows padded to 4 octets
    st, ct, body = _get(ip, "/api/screen.bmp", timeout=20)
    assert ct == "image/bmp" and body[:2] == b"BM"
    data_at, = struct.unpack("<I", body[10:14])
    width, height = struct.unpack("<ii", body[18:26])
    bpp, = struct.unpack("<H", body[28:30])
    assert bpp in (1, 4, 16) and data_at == 54 + (12 if bpp == 16 else 4 << bpp), (bpp, data_at)
    assert len(body) == data_at + (width * bpp + 31) // 32 * 4 * abs(height), (width, height, bpp, len(body))
    # Errors
    try:
        _get(ip, "/nothing")
        assert False, "404 expected"
    except urllib.error.HTTPError as e:
        assert e.code == 404
    assert int(_net_view(link)["http.not_found"]) >= 1


def _kiss(data):
    out = bytearray([FEND, 0x00])
    for b in data:
        out += bytes([FESC, TFEND]) if b == FEND else bytes([FESC, TFESC]) if b == FESC else bytes([b])
    out.append(FEND)
    return bytes(out)


def _ax25_addr(call, ssid, last):
    call = call.upper().ljust(6)[:6]
    return bytes(ord(c) << 1 for c in call) + bytes([0x60 | (ssid << 1) | (1 if last else 0)])


def _ui_frame(src, ssid, info):
    return _ax25_addr("APZRDF", 0, False) + _ax25_addr(src, ssid, True) + b"\x03\xf0" + info


@pytest.mark.feature("net_kiss")
@pytest.mark.usb_switch
def test_kiss_tcp(net):
    if "NET_KISS" not in net["features"]:
        skip("not_built", "built without CONFIG_NET_KISS")
    ip, link = net["ip"], net["link"]
    aset = link.view(["aprs_set"])
    kiss_was = aset.get("set.aprs_kiss", "0") == "1"
    if not kiss_was:
        link.aprs_set("kiss", 1)
    try:
        before = link.view(["aprs"])
        with socket.create_connection((ip, 8001), timeout=5) as s:
            time.sleep(0.3)
            assert _net_view(link)["kiss.client"] == "1"
            # A frame from a foreign callsign: taken by the TNC, refused by the APRS engine (nothing transmitted)
            s.sendall(_kiss(_ui_frame("N0CALL", 0, b">RFOUNDRY HIL KISS-TCP")))
            time.sleep(0.5)
            after = link.view(["aprs"])
            assert int(after["aprs.host_in"]) == int(before["aprs.host_in"]) + 1, (before, after)
            assert int(after["aprs.host_dropped"]) == int(before["aprs.host_dropped"]) + 1
            assert int(_net_view(link)["kiss.frames_in"]) >= 1
            # An own-callsign frame going out on air: test_aprs.py test_aprs_tx_link_answers[kiss_tcp] (the 2 m test
            # frequency at the lowest power; at 1 W on the bench the transmission can knock the USB link off)
        time.sleep(0.5)
        assert _net_view(link)["kiss.client"] == "0"
    finally:
        if not kiss_was:
            link.aprs_set("kiss", 0)


def _rig(s, line):
    s.sendall(line.encode() + b"\n")
    time.sleep(0.15)
    return s.recv(4096).decode()


@pytest.mark.feature("net_rigctl")
@pytest.mark.usb_switch
def test_rigctl(net):
    if "NET_RIGCTL" not in net["features"]:
        skip("not_built", "built without CONFIG_NET_RIGCTL")
    ip, link = net["ip"], net["link"]
    core = link.view(["core", "channel"])
    was_vfo = core.get("vfo") == "1"
    with socket.create_connection((ip, 4532), timeout=5) as s:
        rx_hz = int(_rig(s, "f").strip())
        assert rx_hz == int(core["ch.rx_hz"]), (rx_hz, core)
        assert _rig(s, "m").split()[0] == "FM"
        assert _rig(s, "t").strip() in ("0", "1")
        assert _rig(s, "\\get_info").startswith("RadioFoundry")
        assert _rig(s, "+f").startswith("get_freq:\nFrequency: ")
        assert _rig(s, "xyz").strip() == "RPRT -4"
        # Set the VFO 12.5 kHz up and read it back, then return
        target = rx_hz + 12500
        assert _rig(s, f"F {target}").strip() == "RPRT 0"
        end = time.monotonic() + 3
        while time.monotonic() < end and int(link.view(["channel"]).get("ch.rx_hz", "0")) != target:
            time.sleep(0.1)
        assert int(link.view(["channel"])["ch.rx_hz"]) == target
        assert int(_rig(s, "f").strip()) == target
        assert _rig(s, f"F {rx_hz}").strip() == "RPRT 0"
        time.sleep(0.3)
        ds = _rig(s, "\\dump_state")
        assert ds.startswith("0\n2\n2\n"), ds
    if not was_vfo:
        link.vfo(False)
        time.sleep(0.3)
    assert int(_net_view(link)["rigctl.commands"]) >= 8
    # Hamlib's own client, where it is installed: the dump_state parse and a frequency read
    try:
        r = subprocess.run(["rigctl", "-m", "2", "-r", f"{ip}:4532", "f"], capture_output=True, text=True, timeout=15)
    except (FileNotFoundError, OSError):
        return
    assert r.returncode == 0 and r.stdout.strip().isdigit(), (r.stdout, r.stderr)


@pytest.mark.feature("net_aprsis")
@pytest.mark.usb_switch
def test_aprsis(net):
    server = os.environ.get("RFOUNDRY_HIL_APRSIS_SERVER")
    why = "needs CONFIG_NET_APRSIS, RFOUNDRY_HIL_APRSIS_SERVER=a.b.c.d[:port] and NAT on the host (spec 7.1)"
    if "NET_APRSIS" not in net["features"]:
        skip("not_built", why)
    if not server:
        skip("opt_in", why)
    ip, link = net["ip"], net["link"]
    st, ct, body = _get(ip, f"/api/aprsis?server={server}")
    assert st == 200
    try:
        end = time.monotonic() + 60
        state = ""
        while time.monotonic() < end:
            state = _net_view(link)["aprsis.state"]
            if state == "online":
                break
            time.sleep(1)
        assert state == "online", _net_view(link)
    finally:
        _get(ip, "/api/aprsis?server=off")
