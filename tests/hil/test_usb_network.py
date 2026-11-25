# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""USB network personality (opt-in: --allow-usb-switch; firmware built with CONFIG_DMR_IP_GATEWAY): the radio
re-enumerates as a CDC-NCM adapter, gives the host its address by DHCP (13.<DMR ID>), answers ping at its own
address 12.<DMR ID>, and its debug link answers over UDP port 3334 there; then it switches back to the serial port.
test_usb_network_own_address: UDP to the radio's address on a port no service has gets an ICMP port unreachable and
never goes over the air (usb-network.md 3).

Nothing is transmitted (no IP goes to another radio). Needs the host to bring the adapter up with DHCP (macOS and
Linux do by default); on a cold first plug macOS may take up to 10 s. If the host never configures the adapter, the
radio falls back to serial after 10 s (firmware from 5a13814 on; older firmware stays a network adapter until it is
powered off and on)."""
import ipaddress
import socket
import subprocess
import sys
import time

import pytest

from rfoundry_hil import DebugLink, DebugLinkError, skip, usb_network

INDIVIDUAL_NET = 0x0C000000      # 12.0.0.0/8, IPGW_INDIVIDUAL_NET
PERIPHERAL_NET = 0x0D000000      # 13.0.0.0/8, IPGW_PERIPHERAL_NET


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


@pytest.mark.feature("usb_network")
@pytest.mark.usb_switch
def test_usb_network_roundtrip(radio):
    feats = radio.features()
    if "DMR_IP_GATEWAY" not in feats:
        skip("not_built", "built without CONFIG_DMR_IP_GATEWAY")
    if "USB_NETWORK" not in feats:      # a radio with PPP only: the switch is accepted, but no adapter comes up
        skip("not_built", "built without CONFIG_USB_NETWORK (PPP is test_net_services' and the others')")
    own_id = int(radio.view(["dmr"]).get("dmr.own_id", "0"))
    assert 0 < own_id < (1 << 24), "no DMR ID: the gateway can't make an address"
    host_ip = ipaddress.IPv4Address(PERIPHERAL_NET | own_id)
    radio_ip = ipaddress.IPv4Address(INDIVIDUAL_NET | own_id)
    radio.link.usb_network(True)
    radio.link.close()
    udp = None
    try:
        udp = _udp_link(str(radio_ip), 30)
        assert udp is not None, f"no debug link over UDP at {radio_ip} within 30 s (DHCP on the host's adapter?)"
        v = udp.view(["hotspot"])
        assert v.get("usb.network") == "1" and v.get("usb.net_up") == "1", v
        assert int(v["net.radio_ip"], 16) == int(radio_ip), v
        count = "-c" if sys.platform != "win32" else "-n"
        r = subprocess.run(["ping", count, "3", str(radio_ip)], capture_output=True, text=True, timeout=20)
        assert r.returncode == 0, f"ping {radio_ip}: {r.stdout} {r.stderr}"
        v2 = udp.view(["hotspot"])
        assert int(v2["net.frames_in"]) > int(v["net.frames_in"]), v2
        udp.usb_network(False)
    finally:
        if udp is not None:
            udp.close()
        if not radio.serial_back(20):
            # Still a network adapter: one more try over UDP, so the bench isn't left without its serial port
            again = _udp_link(str(radio_ip), 10)
            if again is not None:
                again.usb_network(False)
                again.close()
            assert radio.serial_back(20), "the radio didn't come back as a serial port"
    assert radio.view(["hotspot"]).get("usb.network") == "0"


UNBOUND_PORT = 9        # discard: no radio service binds it


@pytest.mark.feature("usb_network")
@pytest.mark.usb_switch
def test_usb_network_own_address(radio):
    feats = radio.features()
    if "DMR_IP_GATEWAY" not in feats or "USB_NETWORK" not in feats:
        skip("not_built", "built without CONFIG_DMR_IP_GATEWAY or CONFIG_USB_NETWORK")
    own_id = int(radio.view(["dmr"]).get("dmr.own_id", "0"))
    assert 0 < own_id < (1 << 24), "no DMR ID: the gateway can't make an address"
    with usb_network(radio, own_id) as (udp, host_ip, radio_ip):
        before = udp.view(["hotspot", "net_svc"])
        assert "net_svc.unreachable" in before, "firmware without the own-address check"
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind((host_ip, 0))
            s.connect((radio_ip, UNBOUND_PORT))
            s.settimeout(2.0)
            refused = sent = 0
            for _ in range(3):
                try:
                    s.send(b"probe")
                except ConnectionRefusedError:      # the try before's ICMP error, after its recv timed out
                    refused += 1
                    continue
                sent += 1
                try:
                    s.recv(64)
                except ConnectionRefusedError:      # the ICMP port unreachable, on a connected socket
                    refused += 1
                except socket.timeout:
                    pass
        finally:
            s.close()
        time.sleep(0.5)
        after = udp.view(["hotspot", "net_svc"])
    assert refused >= 1, "no ICMP port unreachable back"
    assert int(after["net_svc.unreachable"]) - int(before["net_svc.unreachable"]) == sent, after
    for k in ("net.to_air", "net.to_air_refused"):     # never handed to the air
        assert after[k] == before[k], (k, before, after)
