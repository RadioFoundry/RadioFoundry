# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""IP over DMR on air (docs/spec/dmr-data.md 11, 12): our UDP packets with the compressed UDP/IPv4 header (SAP 3),
with the full one when compression is off, and as confirmed data with its retries, heard by the SDR bench's modem.

Opt-in RF: radio_tx + bench_service (--allow-rf --allow-service-stop), firmware with CONFIG_DMR_IP_GATEWAY. The radio
sends through the IP gateway's air path: the debug link's DMR_DATA IP_SEND puts a UDP datagram where one from the host
on the USB network goes (hostlink_net_air's mailbox, then the data service), so no USB switch is needed. The last test
(also usb_switch: --allow-usb-switch) sends a real datagram from this host over the USB network instead.

What the bench modem heard is reassembled and decoded on this host by tools/bench/dmr_packet.py, written from the
standards apart from the firmware's code: the data header, the blocks, the CRC-32, the CRC-9 of confirmed blocks, the
compressed header. The bench modem sits next to the radio and misses or garbles some bursts (test_dmr_data.py's
notes): a packet it didn't hear whole is sent again, up to 3 times; the radio's own counters (the dmr_ip view section)
are the pass criteria for what it sent. The bench doesn't answer confirmed data, so the confirmed test checks the
retries and the failure, not an ACK (an ACK and a selective retry are covered on the host: dmrDataServiceTest)."""
import ipaddress
import socket
import time
import warnings

import pytest

import dmr_packet

BENCH_ID = 9990301
SRC_PORT, DST_PORT = 50000, 161            # an SNMP request: no port IDs, both ports in the extended headers
PAYLOAD = bytes(range(0x30, 0x30 + 40))
INDIVIDUAL_NET = 0x0C000000                # 12.0.0.0/8, IPGW_INDIVIDUAL_NET


def _ip_view(radio):
    out = {}
    for k, v in radio.view(["dmr_ip"]).items():
        try:
            out[k] = int(v)
        except ValueError:
            out[k] = v
    return out


def _setup(radio, pytestconfig, tx=True):
    feats = radio.features()
    if "DMR_IP_GATEWAY" not in feats:
        pytest.skip("built without CONFIG_DMR_IP_GATEWAY")
    if _ip_view(radio).get("ip.built") != 1:
        pytest.skip("firmware without the dmr_ip view section (before SAP 3 transmit)")
    freq, cc = pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc")
    radio.go_home()
    radio.find_channel(round(freq * 1e6), "dmr")
    if tx:
        radio.lowest_power()
    own = int(radio.view(["dmr"])["dmr.own_id"])
    if own == 0:
        pytest.skip("the radio has no DMR ID (dmr.own_id=0)")
    time.sleep(1.0)
    return freq, cc, own, feats


def _rule(text):
    if text.startswith("port:"):
        return ("port", int(text[5:]))
    if text.startswith("id:"):
        return int(text[3:])
    return None


@pytest.fixture
def ip_settings(radio):
    """Puts the IP settings back after the test (they are saved in the radio's settings store)."""
    v = _ip_view(radio)
    yield
    if v.get("ip.built") == 1:
        rules = [r for r in (_rule(str(v.get("ip.rule0", "none"))), _rule(str(v.get("ip.rule1", "none")))) if r]
        radio.link.ip_settings(v["ip.compress"], v["ip.confirmed"], v["ip.tries"], rules)


def _send_and_listen(radio, sdr_bench, freq, cc, send, seconds):
    """The bench listens while send() runs and the radio's IP packet is over (ip.busy 0, ip.packets moved)."""
    before = _ip_view(radio)
    with sdr_bench.lease():
        lis = sdr_bench.listen_start(freq, cc, seconds=seconds)
        send()
        v = radio.wait_for(lambda x: int(x.get("ip.packets", 0)) > before["ip.packets"] and x.get("ip.busy") == "0",
                           timeout=seconds - 2, sections=("dmr_ip",), what="the IP packet done")
        heard = sdr_bench.listen_result(lis)
    for r in heard:
        if r.get("dt") in (2, 6, 7):
            print("bench heard:", {k: r[k] for k in r if k in ("dt", "info", "crc_ok")})
    return before, _ip_view(radio), dmr_packet.packets(heard), heard


def _ours(pkts, own):
    return [p for p in pkts if p.get("src") == own and p.get("dst") == BENCH_ID]


def test_ip_settings_view(radio, pytestconfig, ip_settings):
    """Radio only: the dmr_ip section is there and the IP settings request changes what it shows."""
    _setup(radio, pytestconfig, tx=False)
    radio.link.ip_settings("all", "rules", 3, [BENCH_ID, ("port", 161)])
    v = radio.wait_for(lambda x: x.get("ip.compress") == "all" and x.get("ip.tries") == "3", sections=("dmr_ip",),
                       what="IP settings applied")
    assert v["ip.rule0"] == f"id:{BENCH_ID}" and v["ip.rule1"] == "port:161" and v["ip.confirmed"] == "rules", v
    radio.link.ip_settings("off", "off")
    v = radio.wait_for(lambda x: x.get("ip.compress") == "off", sections=("dmr_ip",), what="IP settings off")
    assert v["ip.confirmed"] == "off" and v["ip.rule0"] == "none", v


def _udp_until_heard(radio, sdr_bench, freq, cc, own, want_sap, what, tries=3):
    for attempt in range(1, tries + 1):
        before, after, pkts, _ = _send_and_listen(
            radio, sdr_bench, freq, cc, lambda: radio.link.ip_send(BENCH_ID, SRC_PORT, DST_PORT, PAYLOAD), 12)
        assert after["ip.result"] == "ok" and after["ip.sent"] - before["ip.sent"] == 1, (before, after)
        key = "ip.sap3" if want_sap == dmr_packet.SAP_UDPIP_COMPRESSION else "ip.sap4"
        assert after[key] - before[key] == 1, (key, before, after)
        mine = _ours(pkts, own)
        wrong = [p for p in mine if p["dpf"] != dmr_packet.DPF_UNCONFIRMED or p["sap"] != want_sap]
        assert not wrong, f"{what}: a packet from us with the wrong header: {wrong}"
        whole = [p for p in mine if p.get("complete") and p.get("crc_ok")]
        if whole:
            if attempt > 1:
                warnings.warn(f"{what}: heard whole by the bench modem on send {attempt} of {tries}")
            return whole[0], before, after
        print(f"{what}: send {attempt}: the bench heard {len(mine)} header(s) from us, no whole packet")
    raise AssertionError(f"{what}: the bench modem heard none of {tries} sends whole (the radio counted each as sent)")


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_udp_compressed_tx(radio, sdr_bench, pytestconfig, ip_settings):
    """A UDP datagram (50000 to 161, 40 octets) in auto mode: one unconfirmed SAP 3 packet with the 9 octet header
    (IPv4 identification, SAID / DAID 0, no port IDs, both ports in the extended headers), 6 blocks instead of 7."""
    freq, cc, own, _ = _setup(radio, pytestconfig)
    radio.link.ip_settings("auto", "rules", 0, [])
    p, before, after = _udp_until_heard(radio, sdr_bench, freq, cc, own, dmr_packet.SAP_UDPIP_COMPRESSION, "SAP 3")
    user = bytes.fromhex(p["user"])
    assert p["header_octets"] == 9 and user[2] == 0x00 and user[3] == 0 and user[4] == 0, p
    assert p["src_port"] == SRC_PORT and p["dst_port"] == DST_PORT and bytes.fromhex(p["payload"]) == PAYLOAD, p
    assert p["blocks"] == 5, p                   # 9 + 40 + 4 = 53 octets: 5 blocks (the full header: 72, 6)
    assert after["ip.octets_saved"] - before["ip.octets_saved"] == 19, (before, after)


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_udp_full_header_tx(radio, sdr_bench, pytestconfig, ip_settings):
    """The same datagram with compression off: SAP 4, the full IPv4 / UDP header (28 octets)."""
    freq, cc, own, _ = _setup(radio, pytestconfig)
    radio.link.ip_settings("off", "rules", 0, [])
    p, _, _ = _udp_until_heard(radio, sdr_bench, freq, cc, own, dmr_packet.SAP_IP, "SAP 4")
    assert p["header_octets"] == 28 and p["src_port"] == SRC_PORT and bytes.fromhex(p["payload"]) == PAYLOAD, p
    assert p["blocks"] == 6, p


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_confirmed_tx_unanswered(radio, sdr_bench, pytestconfig, ip_settings):
    """Confirmed for the bench's ID, 2 tries: the C_HEAD (DPF 3, A set, F set, an N(S), FSN 1000) with every block
    (each with its serial number and a CRC-9 the bench's independent check accepts), no response, the whole packet
    again with F clear and the same N(S), then failed. In direct mode each ends with a data terminator LC."""
    freq, cc, own, _ = _setup(radio, pytestconfig)
    radio.link.ip_settings("auto", "rules", 2, [BENCH_ID])
    before, after, pkts, heard = _send_and_listen(
        radio, sdr_bench, freq, cc, lambda: radio.link.ip_send(BENCH_ID, SRC_PORT, DST_PORT, PAYLOAD), 16)
    d = {k: after[k] - before[k] for k in ("ip.cnf", "ip.cnf_tx", "ip.cnf_retries", "ip.cnf_timeouts", "ip.cnf_failed",
                                           "ip.failed", "ip.sap3")}
    print("radio deltas:", d)
    assert after["ip.result"] == "failed", after
    assert d == {"ip.cnf": 1, "ip.cnf_tx": 2, "ip.cnf_retries": 1, "ip.cnf_timeouts": 2, "ip.cnf_failed": 1,
                 "ip.failed": 1, "ip.sap3": 1}, d
    mine = _ours(pkts, own)
    assert all(p["dpf"] == dmr_packet.DPF_CONFIRMED and p["sap"] == dmr_packet.SAP_UDPIP_COMPRESSION and
               p["response_requested"] and p["fsn"] == 0x8 for p in mine), mine
    assert 1 <= len(mine) <= 2, f"the radio sent 2 transmissions, the bench heard {len(mine)} headers: {mine}"
    if len(mine) < 2:
        warnings.warn(f"the bench modem heard {len(mine)} of the 2 transmissions")
    else:
        assert mine[0]["first_try"] and not mine[1]["first_try"] and mine[0]["ns"] == mine[1]["ns"], mine
    whole = [p for p in mine if p.get("first_try") and p.get("complete")]
    for p in whole:
        assert p["crc_ok"] and not p["bad_blocks"] and p["src_port"] == SRC_PORT, p
        assert bytes.fromhex(p["payload"]) == PAYLOAD, p
    if not whole:
        warnings.warn("the bench modem heard no first try whole: the CRC-9 / CRC-32 not checked on air")
    if not any(r.get("dt") == 2 for r in heard):
        warnings.warn("no data terminator LC heard after the confirmed packets")


@pytest.mark.usb_switch
@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_usb_network_udp_to_air(radio, sdr_bench, pytestconfig, ip_settings):
    """The real host path: the radio as a USB network adapter, a UDP datagram from this host to the bench's address
    (12.x.y.z) goes on air as a SAP 3 packet the bench decodes; then back to the serial port."""
    from odr_hil import DebugLink, DebugLinkError, find_ports
    freq, cc, own, _ = _setup(radio, pytestconfig)
    radio.link.ip_settings("auto", "rules", 0, [])
    host_ip = ipaddress.IPv4Address(INDIVIDUAL_NET | own)
    radio_ip = str(ipaddress.IPv4Address(int(host_ip) ^ 1))
    bench_ip = str(ipaddress.IPv4Address(INDIVIDUAL_NET | (BENCH_ID & 0xFFFFFF)))
    radio.link.usb_network(True)
    radio.link.close()
    udp = None
    try:
        end = time.monotonic() + 30
        while udp is None and time.monotonic() < end:
            try:
                udp = DebugLink(f"udp:{radio_ip}", timeout=1.0)
                udp.ping()
            except (DebugLinkError, OSError):
                udp = None
                time.sleep(0.5)
        assert udp is not None, f"no debug link over UDP at {radio_ip} (DHCP on the host's adapter?)"
        before = {k: int(v) for k, v in udp.view(["dmr_ip"]).items() if str(v).isdigit()}
        heard_pkts = []
        with sdr_bench.lease():
            lis = sdr_bench.listen_start(freq, cc, seconds=10)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.bind((str(host_ip), SRC_PORT))
                s.sendto(PAYLOAD, (bench_ip, DST_PORT))
            time.sleep(4)
            heard_pkts = dmr_packet.packets(sdr_bench.listen_result(lis))
        after = {k: int(v) for k, v in udp.view(["dmr_ip"]).items() if str(v).isdigit()}
        assert after["ip.sap3"] - before["ip.sap3"] == 1 and after["ip.sent"] - before["ip.sent"] == 1, (before, after)
        mine = _ours(heard_pkts, own)
        assert all(p["sap"] == dmr_packet.SAP_UDPIP_COMPRESSION for p in mine), mine
        whole = [p for p in mine if p.get("complete") and p.get("crc_ok")]
        if whole:
            assert whole[0]["dst_port"] == DST_PORT and bytes.fromhex(whole[0]["payload"]) == PAYLOAD, whole[0]
        else:
            warnings.warn("the bench modem didn't hear the packet whole")
        udp.usb_network(False)
    finally:
        if udp is not None:
            udp.close()
        end = time.monotonic() + 20
        while not find_ports() and time.monotonic() < end:
            time.sleep(0.5)
        for _ in range(40):
            try:
                radio.link = DebugLink(radio.port, timeout=1.5)
                break
            except DebugLinkError:
                time.sleep(0.25)
