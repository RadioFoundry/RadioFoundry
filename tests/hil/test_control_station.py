# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Control station mode (docs/spec/usb-network.md 5; firmware built with CONFIG_DMR_CONTROL_STATION): the stored USB
network settings (radio op 27), booting as the network adapter, the fixed link pair 192.168.10.1 / .2, and
passthrough (IP data to the radio's ID goes to the PC, not to the radio's own services).

Opt-in: --allow-usb-switch for everything that re-enumerates, --allow-reboot for the boot test, --allow-rf
--allow-service-stop for the passthrough tests with the SDR bench (the bench sends; the radio only answers confirmed
data with its ETSI response). Every test leaves the settings all off and the radio on its serial port. Needs the
host to bring the adapter up with DHCP (macOS and Linux do by default)."""
import ipaddress
import socket
import subprocess
import sys
import time

import pytest

from rfoundry_hil import DebugLink, DebugLinkError, find_ports, need_radio_tx, skip

INDIVIDUAL_NET = 0x0C000000      # 12.0.0.0/8, IPGW_INDIVIDUAL_NET
FIXED_RADIO = "192.168.10.1"     # IPGW_FIXED_LINK_DEFAULT
FIXED_HOST = "192.168.10.2"
BENCH_SRC = 9990301


def _need(radio):
    if "DMR_IP_GATEWAY" not in radio.features():
        skip("not_built", "built without CONFIG_DMR_IP_GATEWAY")
    if "net.passthrough" not in radio.view(["hotspot"]):
        skip("not_built", "firmware without control station mode (no net.passthrough view key)")


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


def _reconnect(radio):
    for _ in range(40):
        try:
            radio.link = DebugLink(radio.port, timeout=1.5)
            return
        except DebugLinkError:
            time.sleep(0.25)
    raise AssertionError("no debug link on the serial port")


def _back_to_serial(radio, ips):
    """Settings all off and the serial personality again, over whichever UDP address answers."""
    if not _serial_back(5):
        for ip in ips:
            again = _udp_link(ip, 15)
            if again is not None:
                try:
                    again.usb_net_settings()
                    again.usb_network(False)
                finally:
                    again.close()
                break
        assert _serial_back(20), "the radio didn't come back as a serial port"
    _reconnect(radio)
    radio.link.usb_net_settings()


def _addresses(radio):
    own = int(radio.view(["dmr"]).get("dmr.own_id", "0"))
    assert 0 < own < (1 << 24), "no DMR ID: the gateway can't make an address"
    host = ipaddress.IPv4Address(INDIVIDUAL_NET | own)
    return own, str(host), str(ipaddress.IPv4Address(int(host) ^ 1))


@pytest.mark.feature("dmr_control_station")
def test_usb_settings_stored(radio):
    """Radio only: op 27 stores each setting, the view keys follow; nothing re-enumerates (the power-on setting acts
    at the next boot only)."""
    _need(radio)
    if "USB" not in radio.features():
        skip("no_hw", "no USB on this radio: no USB network settings")
    try:
        radio.link.usb_net_settings(boot_network=True)
        v = radio.view(["hotspot"])
        assert v["usb.boot_network"] == "1" and v["net.passthrough"] == "0" and v["usb.network"] == "0", v
        radio.link.usb_net_settings(passthrough=True, fixed_link=True, link_ip="10.9.8.1")
        v = radio.view(["hotspot"])
        assert v["usb.boot_network"] == "0" and v["net.passthrough"] == "1" and v["net.fixed_link"] == "1", v
        assert int(v["net.link_ip"], 16) == 0x0A090801 and int(v["net.link_in_force"], 16) == 0x0A090801, v
        # Passthrough is in force only while the network link is up: not on the serial port
        assert v["net.passthrough_active"] == "0", v
        assert radio.view(["dmr_ext"]).get("data.pt_active") == "0"
        radio.link.usb_net_settings(fixed_link=True, link_ip="12.0.0.1")    # inside the radio range: refused
        v = radio.view(["hotspot"])
        assert int(v["net.link_in_force"], 16) == 0, v
    finally:
        radio.link.usb_net_settings()
    v = radio.view(["hotspot"])
    assert v["usb.boot_network"] == "0" and v["net.passthrough"] == "0" and v["net.fixed_link"] == "0", v


@pytest.mark.feature("dmr_control_station")
@pytest.mark.usb_switch
def test_fixed_link(radio):
    """The fixed link: the host gets 192.168.10.2 by DHCP, pings 192.168.10.1, the debug link answers there."""
    _need(radio)
    _own, _host, radio_ip = _addresses(radio)
    radio.link.usb_net_settings(fixed_link=True)
    radio.link.usb_network(True)
    radio.link.close()
    udp = None
    try:
        udp = _udp_link(FIXED_RADIO, 40)
        assert udp is not None, f"no debug link over UDP at {FIXED_RADIO} within 40 s (DHCP on the host's adapter?)"
        v = udp.view(["hotspot"])
        assert v["usb.net_up"] == "1" and int(v["net.radio_ip"], 16) == 0xC0A80A01, v
        assert int(v["net.host_ip"], 16) == 0xC0A80A02, v
        count = "-c" if sys.platform != "win32" else "-n"
        r = subprocess.run(["ping", count, "3", FIXED_RADIO], capture_output=True, text=True, timeout=20)
        assert r.returncode == 0, f"ping {FIXED_RADIO}: {r.stdout} {r.stderr}"
        # Serial first: clearing the settings here would move the radio off 192.168.10.1 at once, and the host keeps
        # 192.168.10.2, which the DMR ID address doesn't take the debug link from. _back_to_serial clears them
        udp.usb_network(False)
    finally:
        if udp is not None:
            udp.close()
        _back_to_serial(radio, [FIXED_RADIO, radio_ip])
    assert radio.view(["hotspot"]).get("usb.network") == "0"


@pytest.mark.feature("dmr_control_station")
@pytest.mark.usb_switch
@pytest.mark.reboot
def test_boot_network(radio):
    """usb_mode 1: after a restart the radio is the network adapter without being switched; the setting is then
    cleared over UDP and the radio restarted back to serial."""
    _need(radio)
    _own, _host, radio_ip = _addresses(radio)
    radio.link.usb_net_settings(boot_network=True)
    time.sleep(2.5)                     # the settings store saves after 2 s without a change
    radio.link.reboot(0, radio.link.chip()["token"])
    radio.link.close()
    udp = None
    try:
        udp = _udp_link(radio_ip, 60)
        assert udp is not None, f"no debug link over UDP at {radio_ip} within 60 s of the restart"
        v = udp.view(["hotspot"])
        assert v["usb.network"] == "1" and v["usb.boot_network"] == "1", v
        udp.usb_net_settings()
        time.sleep(2.5)
        udp.reboot(0, udp.chip()["token"])
    finally:
        if udp is not None:
            udp.close()
        _serial_back(60)            # the restart takes a while; _back_to_serial tries UDP if it stays a network adapter
        _back_to_serial(radio, [radio_ip])
    assert radio.view(["hotspot"]).get("usb.boot_network") == "0"


def _passthrough_session(radio, pytestconfig):
    """Network mode with passthrough on, on the DMR test channel: (udp debug link, own ID, host IP, radio IP)."""
    _need(radio)
    freq = pytestconfig.getoption("--dmr-freq")
    radio.go_home()
    radio.find_channel(round(freq * 1e6), "dmr")
    own, host_ip, radio_ip = _addresses(radio)
    radio.link.usb_net_settings(passthrough=True)
    radio.link.usb_network(True)
    radio.link.close()
    udp = _udp_link(radio_ip, 40)
    if udp is None:
        _back_to_serial(radio, [radio_ip])
        raise AssertionError(f"no debug link over UDP at {radio_ip} within 40 s")
    return udp, own, host_ip, radio_ip


def _counters(udp):
    # One section per request: over UDP a long reply is cut short (truncated=1) and the later keys go missing
    v = {}
    for section in ("dmr_data_stats", "dmr_ext"):
        v.update(udp.view([section]))
    return {k: int(x) for k, x in v.items() if str(x).isdigit()}


def _listen(host_ip, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host_ip, port))
    s.settimeout(0.5)
    return s


def _received(sock, seconds):
    end = time.monotonic() + seconds
    got = []
    while time.monotonic() < end:
        try:
            got.append(sock.recvfrom(1500))
        except socket.timeout:
            pass
    return got


@pytest.mark.feature("dmr_control_station")
@pytest.mark.usb_switch
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_passthrough_text(radio, sdr_bench, pytestconfig):
    """Passthrough: an ETSI IP text and a TMS with an ACK request from the bench to the radio's ID reach the PC's
    sockets (UDP 5016, 4007) and only them: the inbox count stays, no TMS ACK goes out, data.pt_yielded counts both."""
    freq, cc = pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc")
    udp, own, host_ip, radio_ip = _passthrough_session(radio, pytestconfig)
    try:
        assert udp.view(["hotspot"])["net.passthrough_active"] == "1"
        before = _counters(udp)
        s5016, s4007 = _listen(host_ip, 5016), _listen(host_ip, 4007)
        try:
            text = f"RadioFoundry CS {int(time.time()) % 10000}"
            with sdr_bench.lease():
                sdr_bench.data(freq, cc, own, ip_text=text, src=BENCH_SRC)
                sdr_bench.data(freq, cc, own, tms=text, tms_seq=(int(time.time()) % 31) + 1, src=BENCH_SRC,
                               listen_after=3)
            got5016, got4007 = _received(s5016, 3), _received(s4007, 1)
        finally:
            s5016.close()
            s4007.close()
        after = _counters(udp)
        print("deltas:", {k: after.get(k, 0) - before.get(k, 0) for k in
                          ("data.sms_received", "data.tms_acks_sent", "data.pt_yielded", "data.ip_text_received")})
        assert any(p.decode("utf-16-be", "replace") == text for p, _a in got5016), got5016
        assert got4007, "the TMS didn't reach the PC's socket on 4007"
        assert after["data.sms_received"] == before["data.sms_received"], (before, after)
        assert after["data.tms_acks_sent"] == before["data.tms_acks_sent"], (before, after)
        assert after["data.pt_yielded"] >= before["data.pt_yielded"] + 2, (before, after)
        udp.usb_net_settings()
        udp.usb_network(False)
    finally:
        udp.close()
        _back_to_serial(radio, [radio_ip])


@pytest.mark.feature("dmr_control_station")
@pytest.mark.usb_switch
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_passthrough_reply(radio, sdr_bench, pytestconfig):
    """The PC's datagram to the bench's ID goes over the air from the radio's own DMR ID; a confirmed packet from the
    bench still gets the radio's ETSI response in passthrough."""
    need_radio_tx(radio)
    freq, cc = pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc")
    udp, own, host_ip, radio_ip = _passthrough_session(radio, pytestconfig)
    try:
        bench_ip = str(ipaddress.IPv4Address(INDIVIDUAL_NET | BENCH_SRC))
        before = _counters(udp)
        with sdr_bench.lease():
            p = sdr_bench.listen_start(freq, cc, 8)
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind((host_ip, 0))
            s.sendto("from the dispatcher".encode("utf-16-be"), (bench_ip, 5016))
            s.close()
            heard = sdr_bench.listen_result(p)
            sdr_bench.data(freq, cc, own, octets=40, confirmed=True, src=BENCH_SRC, listen_after=3)
        after = _counters(udp)
        headers = [bytes.fromhex(r["info"]) for r in heard if r.get("dt") == 6 and r.get("info")
                   and r.get("crc_ok", True)]
        print("headers heard:", [h.hex() for h in headers])
        assert any(int.from_bytes(h[2:5], "big") == BENCH_SRC and int.from_bytes(h[5:8], "big") == own
                   for h in headers), "no data header to the bench from the radio's ID heard"
        assert after["data.responses_sent"] == before["data.responses_sent"] + 1, (before, after)
        udp.usb_net_settings()
        udp.usb_network(False)
    finally:
        udp.close()
        _back_to_serial(radio, [radio_ip])
