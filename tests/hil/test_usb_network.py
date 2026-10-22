# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""USB network personality (opt-in: --allow-usb-switch; firmware built with CONFIG_DMR_IP_GATEWAY): the radio
re-enumerates as a CDC-NCM adapter, gives the host its address by DHCP (12.<DMR ID>), answers ping at the host's
address xor 1, and its debug link answers over UDP port 3334 there; then it switches back to the serial port.

Nothing is transmitted (no IP goes to another radio). Needs the host to bring the adapter up with DHCP (macOS and
Linux do by default); on a cold first plug macOS may take up to 10 s. If the host never configures the adapter, the
radio falls back to serial after 10 s (firmware from 5a13814 on; older firmware stays a network adapter until it is
powered off and on)."""
import ipaddress
import subprocess
import sys
import time

import pytest

from odr_hil import DebugLink, DebugLinkError, find_ports, skip

INDIVIDUAL_NET = 0x0C000000      # 12.0.0.0/8, IPGW_INDIVIDUAL_NET


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


@pytest.mark.feature("usb_network")
@pytest.mark.usb_switch
def test_usb_network_roundtrip(radio):
    if "DMR_IP_GATEWAY" not in radio.features():
        skip("not_built", "built without CONFIG_DMR_IP_GATEWAY")
    own_id = int(radio.view(["dmr"]).get("dmr.own_id", "0"))
    assert 0 < own_id < (1 << 24), "no DMR ID: the gateway can't make an address"
    host_ip = ipaddress.IPv4Address(INDIVIDUAL_NET | own_id)
    radio_ip = ipaddress.IPv4Address(int(host_ip) ^ 1)
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
        if not _serial_back(20):
            # Still a network adapter: one more try over UDP, so the bench isn't left without its serial port
            again = _udp_link(str(radio_ip), 10)
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
    assert radio.view(["hotspot"]).get("usb.network") == "0"
