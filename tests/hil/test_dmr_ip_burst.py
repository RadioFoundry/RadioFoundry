# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""UDP over DMR in bursts and at its largest (docs/spec/dmr-data.md 12.6, 12.8; usb-network.md 3.2): datagrams back to
back with no delay, each way, and the longest datagram one transmission carries, each way, with one octet more.

Opt-in RF as test_dmr_ip.py: radio_tx + bench_service (--allow-rf --allow-service-stop), firmware with
CONFIG_DMR_IP_GATEWAY. The tests that use this host's socket over the radio's IP link are also usb_switch
(--allow-usb-switch): the USB network adapter, or on a radio without one PPP on its serial host port (pppd, as root or
with passwordless sudo; rfoundry_hil.host_network). The bench sends UDP with tools/bench/bench_tx.py's `data --udp-port` (run
tools/bench/dmr_bench.py sync first: an older copy on the bench has no UDP mode, and those tests skip).

Pass criteria are the radio's own counters (the dmr_ip, air and hotspot view sections) and, from the air, what this
host's socket got. What the bench modem heard of the radio is decoded by tools/bench/dmr_packet.py; the bench modem
misses or garbles some bursts (test_dmr_ip.py), so a packet it didn't hear whole is a warning, never a failure; a
packet it heard whole must be right (payload, order, header). The debug link's IP_SEND carries at most 64 octets of
payload (DL_REQ_MAX), so the largest datagrams come from this host over that link.

The air MTU (DMR_DATA_IP_MTU, 596) is derived below from the firmware's limits and checked against the radio's
net.air_mtu. It is above the link's MTU (net.mtu, 586; over PPP ppp.mru), so the largest datagram from this host is
the link's, and it always fits the air: the ICMP fragmentation needed tests run only on firmware built with a narrower
IPGW_AIR_MTU (the host test ipGatewayTestAir440 covers that path on every build). Receiving, the radio reassembles up
to DMR_DATA_MAX_PACKET octets of blocks (51 blocks: 608 octets of IPv4, a UDP payload of 580, what another make's
radio may send); this host gets what fits the link (558 octets of UDP payload), and a longer one is dropped and
counted (net.up_too_big)."""
import ipaddress
import os
import socket
import subprocess
import sys
import time
import warnings

import pytest

from rfoundry_hil import DebugLinkError, skip, host_network, host_network_kind, INDIVIDUAL_NET
import dmr_packet
# Shared with test_dmr_ip.py: its setup, views and fixtures (ip_settings and ars_off put the settings back)
from test_dmr_ip import BENCH_ID, _ip_view, _ours, _setup, ars_off, ip_settings  # noqa: F401

BURST = 5                                  # datagrams in a row
SRC_PORT, DST_PORT = 50000, 50001          # outside 4000-4099: compressed in auto, both ports in extended headers
UDPC_HEADER = 9                            # that compressed header: 5 octets and the 2 ports (dmr-data.md 11.1)
FULL_HEADER = 20 + 8                       # IPv4 and UDP (SAP 4)
DMR_DATA_MAX_PACKET = 612                  # core/dmr/dmrData.h: one packet's octets of blocks
RATE12_OCTETS = 12
CRC32_OCTETS = 4
AIR_MTU = DMR_DATA_MAX_PACKET - 16         # DMR_DATA_IP_MTU (596): 50 blocks unconfirmed, 60 confirmed
RX_MAX_BLOCKS = DMR_DATA_MAX_PACKET // RATE12_OCTETS                                     # 51: the most it reassembles
RX_MAX_FULL = RX_MAX_BLOCKS * RATE12_OCTETS - CRC32_OCTETS - FULL_HEADER                  # 580
LINK_MTU = 600 - 14                        # IPGW_LINK_MTU: the gateway's frames, DHCP option 26
UP_MAX_FULL = LINK_MTU - FULL_HEADER       # 558: the largest UDP payload from the air the link carries to this host
DL_PAYLOAD_MAX = 64                        # the debug link's IP_SEND payload (DL_REQ_MAX)
# Oversize datagrams go to another ID than the bench's: the host keeps the path MTU an ICMP reply taught it per
# destination, and the bench's address must stay at the link MTU for the largest compressed datagram (more IPv4 than
# the air MTU: the compressed header is 19 octets shorter)
OVERSIZE_ID = BENCH_ID + 1
HOST_PORT = 50002                          # this host's port for datagrams from the air
ICMP_UNREACHABLE, ICMP_FRAG_NEEDED = 3, 4


def _payload(index, n):
    """The payload of datagram index: as bench_tx.py builds it (filler (i * 7 + 1) & 0xFF, the first 2 octets the
    index, big endian), so the same rule serves both ways."""
    base = bytes((i * 7 + 1) & 0xFF for i in range(n))
    return index.to_bytes(2, "big") + base[2:] if n >= 2 else base


def _ints(view):
    out = {}
    for k, v in view.items():
        try:
            out[k] = int(v)
        except ValueError:
            out[k] = v
    return out


def _net_link(link):
    """The net_link section (the MTUs and their drops), empty on firmware from before it (view version 4)."""
    try:
        return link.view(["net_link"])
    except DebugLinkError:
        return {}


def _views(link):
    """The counters these tests read: the IP service (dmr_ip), the air queue (air), the gateway (hotspot, net_link),
    the data service's receiver (dmr_data_stats: data.rx_packets, data.rx_crc_errors; the dmr_data section has none)."""
    v = link.view(["dmr_ip", "air", "hotspot", "dmr_data_stats"])
    v.update(_net_link(link))
    return _ints(v)


def _delta(before, after, keys):
    return {k: after.get(k, 0) - before.get(k, 0) for k in keys}


def _wait(link, pred, timeout, what):
    end = time.monotonic() + timeout
    v = _views(link)
    while not pred(v) and time.monotonic() < end:
        time.sleep(0.2)
        v = _views(link)
    assert pred(v), f"timed out waiting for {what}: {v}"
    return v


def _need_mtu_keys(v):
    """The MTU keys, checked: the link's is LINK_MTU, the air's AIR_MTU or (a build with a narrower IPGW_AIR_MTU) less."""
    if "net.air_mtu" not in v:
        skip("not_built", "firmware without the net.air_mtu view key (before the air MTU)")
    assert v["net.mtu"] == LINK_MTU and 0 < v["net.air_mtu"] <= AIR_MTU, (v["net.air_mtu"], v["net.mtu"])


def _need_narrow_air(v):
    """Skip unless the air MTU is below the link's: only then can a datagram from this host be too long for the air."""
    _need_mtu_keys(v)
    if v["net.air_mtu"] >= v["net.mtu"]:
        skip("not_built", f"the air MTU ({v['net.air_mtu']}) is not below the link's ({v['net.mtu']}): nothing from "
             "this host is too long for the air (firmware with a narrower IPGW_AIR_MTU tests it; ipGatewayTestAir440 "
             "covers it on the host)")


def _blocks(user_octets):
    """Rate 1/2 blocks of an unconfirmed packet of user_octets (the CRC-32 in the last)."""
    return -(-(user_octets + CRC32_OCTETS) // RATE12_OCTETS)


def _udp_socket(df):
    """A UDP socket with DF set (df True) or never set and never fragmented here (False), where the host can say.
    Linux: IP_MTU_DISCOVER (DO / DONT); macOS: IP_DONTFRAG. None when df is asked for and the host can't set it."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if sys.platform.startswith("linux"):
        s.setsockopt(socket.IPPROTO_IP, getattr(socket, "IP_MTU_DISCOVER", 10), 2 if df else 0)
        return s
    dontfrag = getattr(socket, "IP_DONTFRAG", 28 if sys.platform == "darwin" else None)
    if dontfrag is not None:
        try:
            s.setsockopt(socket.IPPROTO_IP, dontfrag, 1 if df else 0)
            return s
        except OSError:
            pass
    if df:
        s.close()
        return None
    return s


def _bench_udp(sdr_bench, freq, cc, dst, payload_len, repeat=1, udp_port=HOST_PORT):
    """The bench sends UDP (SAP 4) to dst: repeat datagrams of payload_len octets back to back (bench_tx.py data
    --udp-port --repeat)."""
    try:
        return sdr_bench.data(freq, cc, dst, udp_port=udp_port, udp_src_port=SRC_PORT, octets=payload_len,
                              repeat=repeat, src=BENCH_ID)
    except RuntimeError as e:
        if "unrecognized arguments" in str(e) or "invalid choice" in str(e):
            skip("bench", "the bench's bench_tx.py has no UDP mode: run tools/bench/dmr_bench.py sync")
        raise


def _check_heard(pkts, own, sent, sap, what):
    """What the bench heard of ours against the payloads sent, in order: a packet heard whole must be one of them, with
    the header, ports and payload right, each once, in the order sent; a missing one is a warning."""
    mine = _ours(pkts, own)
    wrong = [p for p in mine if p["dpf"] != dmr_packet.DPF_UNCONFIRMED or p["sap"] != sap]
    assert not wrong, f"{what}: a packet from us with the wrong header: {wrong}"
    whole = [p for p in mine if p.get("complete") and p.get("crc_ok")]
    got = []
    for p in whole:
        payload = bytes.fromhex(p["payload"])
        assert p["src_port"] == SRC_PORT and p["dst_port"] == DST_PORT, p
        assert payload in sent, f"{what}: a payload we never sent: {payload[:8].hex()}... ({len(payload)} octets)"
        got.append(sent.index(payload))
    assert got == sorted(got) and len(set(got)) == len(got), f"{what}: heard out of order or twice: {got}"
    if len(got) < len(sent):
        warnings.warn(f"{what}: the bench modem heard {len(got)} of {len(sent)} whole ({len(mine)} headers)")
    return whole


# ── bursts ──────────────────────────────────────────────────────────────────

@pytest.mark.feature("dmr_ip_gateway")
@pytest.mark.feature("dmr_udp_compress", optional=True)
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_udp_burst_radio_to_air(radio, sdr_bench, pytestconfig, ip_settings, ars_off):
    """BURST datagrams (40 octets, each its index) put into the gateway's air path by the debug link's IP_SEND with no
    delay between them: the radio sends all of them (ip.sent + BURST, nothing failed, refused, given up or aged; queued
    back to back, they may share transmissions: ip.chains), and what the bench heard whole is ours, SAP 3, in order."""
    freq, cc, own, _ = _setup(radio, pytestconfig)
    radio.link.ip_settings("auto", "rules", 0, [])
    sent = [_payload(k, 40) for k in range(BURST)]
    before = _views(radio.link)
    with sdr_bench.lease():
        lis = sdr_bench.listen_start(freq, cc, seconds=20)
        for p in sent:
            radio.link.ip_send(BENCH_ID, SRC_PORT, DST_PORT, p)
        after = _wait(radio.link, lambda v: (v["ip.sent"] + v["ip.failed"] - before["ip.sent"] - before["ip.failed"]
                                             >= BURST) and v.get("airq.depth", 0) == 0, 16, "the burst sent")
        heard = sdr_bench.listen_result(lis)
    d = _delta(before, after, ["ip.sent", "ip.failed", "ip.sap3", "ip.chains", "ip.chained", "airq.full",
                               "airq.gave_up", "airq.aged", "net.air_busy", "net.air_dropped", "net.air_full"])
    print("radio deltas:", d)
    assert d["ip.sent"] == BURST and d["ip.failed"] == 0 and d["ip.sap3"] == BURST, d
    assert d["airq.full"] == d["airq.gave_up"] == d["airq.aged"] == 0, d
    assert d["net.air_busy"] == d["net.air_dropped"] == d["net.air_full"] == 0, d
    _check_heard(dmr_packet.packets(heard), own, sent, dmr_packet.SAP_UDPIP_COMPRESSION, "radio to air")


@pytest.mark.feature("dmr_ip_gateway")
@pytest.mark.feature("dmr_udp_compress", optional=True)
@pytest.mark.usb_switch
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_udp_burst_pc_to_air(radio, sdr_bench, pytestconfig, ip_settings, ars_off):
    """The same BURST datagrams from this host's socket over the USB network, back to back: all on air, in order."""
    freq, cc, own, _ = _setup(radio, pytestconfig)
    radio.link.ip_settings("auto", "rules", 0, [])
    bench_ip = str(ipaddress.IPv4Address(INDIVIDUAL_NET | (BENCH_ID & 0xFFFFFF)))
    sent = [_payload(k, 40) for k in range(BURST)]
    with host_network(radio, own) as (udp, host_ip, _):
        before = _views(udp)
        with sdr_bench.lease():
            lis = sdr_bench.listen_start(freq, cc, seconds=20)
            s = _udp_socket(df=False)
            with s:
                s.bind((host_ip, SRC_PORT))
                for p in sent:
                    s.sendto(p, (bench_ip, DST_PORT))
            after = _wait(udp, lambda v: (v["ip.sent"] + v["ip.failed"] - before["ip.sent"] - before["ip.failed"]
                                          >= BURST) and v.get("airq.depth", 0) == 0, 16, "the burst sent")
            heard = sdr_bench.listen_result(lis)
    d = _delta(before, after, ["net.frames_in", "net.to_air", "net.to_air_refused", "ip.sent", "ip.failed", "ip.sap3",
                               "ip.chains", "airq.full", "airq.gave_up", "airq.aged", "net.air_too_big"])
    print("radio deltas:", d)
    assert d["net.to_air"] == BURST and d["net.to_air_refused"] == 0, d
    assert d["ip.sent"] == BURST and d["ip.failed"] == 0 and d["ip.sap3"] == BURST, d
    assert d["airq.full"] == d["airq.gave_up"] == d["airq.aged"] == 0 and d.get("net.air_too_big", 0) == 0, d
    _check_heard(dmr_packet.packets(heard), own, sent, dmr_packet.SAP_UDPIP_COMPRESSION, "PC to air")


def _receive(s, count, timeout):
    """Up to count datagrams from s within timeout: [(payload, source address)]."""
    got, end = [], time.monotonic() + timeout
    while len(got) < count and time.monotonic() < end:
        s.settimeout(max(0.05, end - time.monotonic()))
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            break
        got.append((data, addr[0]))
    return got


@pytest.mark.feature("dmr_ip_gateway")
@pytest.mark.usb_switch
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_udp_burst_air_to_pc(radio, sdr_bench, pytestconfig):
    """The bench sends BURST UDP datagrams (SAP 4, 40 octets, each its index) to our DMR ID back to back, each its own
    packet with its preambles in one key-up: this host's socket on our address gets every one, intact and in order,
    from the bench's address."""
    freq, cc, own, _ = _setup(radio, pytestconfig, tx=False)
    bench_ip = str(ipaddress.IPv4Address(INDIVIDUAL_NET | (BENCH_ID & 0xFFFFFF)))
    with host_network(radio, own) as (udp, host_ip, _):
        before = _views(udp)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.bind((host_ip, HOST_PORT))
            with sdr_bench.lease():
                _bench_udp(sdr_bench, freq, cc, own, 40, repeat=BURST)
            got = _receive(s, BURST, 5)
        after = _views(udp)
    d = _delta(before, after, ["data.rx_packets", "data.rx_crc_errors", "net.packets_air", "net.packets_up",
                               "net.packets_dropped"])
    print("radio deltas:", d, "got:", [(p[:2].hex(), len(p), a) for p, a in got])
    assert d["data.rx_packets"] >= BURST and d["data.rx_crc_errors"] == 0, d
    assert d["net.packets_dropped"] == 0, d
    assert [p for p, _ in got] == [_payload(k, 40) for k in range(BURST)], "not every datagram, intact, in order"
    assert all(a == bench_ip for _, a in got), got


# ── the largest datagram ────────────────────────────────────────────────────

@pytest.mark.feature("dmr_ip_gateway")
def test_air_mtu_view(radio):
    """Radio only: the advertised MTUs (net_link section): net.mtu the link's (DHCP option 26) and net.air_mtu the
    air's, as this file derives it from the firmware's limits, not below the link's. Reading them needs no USB switch."""
    if "DMR_IP_GATEWAY" not in radio.features():
        skip("not_built", "built without CONFIG_DMR_IP_GATEWAY")
    v = _ints(_net_link(radio.link))
    _need_mtu_keys(v)
    if v["net.air_mtu"] != AIR_MTU:
        warnings.warn(f"net.air_mtu is {v['net.air_mtu']}, not {AIR_MTU}: a build with a narrower IPGW_AIR_MTU")


@pytest.mark.feature("dmr_ip_gateway")
@pytest.mark.feature("dmr_udp_compress", optional=True)
@pytest.mark.usb_switch
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.parametrize("compress,sap", [("auto", dmr_packet.SAP_UDPIP_COMPRESSION), ("off", dmr_packet.SAP_IP)],
                         ids=["sap3", "sap4"])
def test_udp_max_pc_to_air(radio, sdr_bench, pytestconfig, ip_settings, ars_off, compress, sap):
    """The largest datagram this host can send, the link's MTU (net.mtu, over PPP ppp.mru) or the air's when that is
    smaller: one transmission, its data header the blocks that user data takes (50 at most: the host test
    dmrDataServiceTest checks the air MTU burst by burst); the bench decodes the whole payload."""
    freq, cc, own, _ = _setup(radio, pytestconfig)
    radio.link.ip_settings(compress, "rules", 0, [])
    bench_ip = str(ipaddress.IPv4Address(INDIVIDUAL_NET | (BENCH_ID & 0xFFFFFF)))
    header = UDPC_HEADER if sap == dmr_packet.SAP_UDPIP_COMPRESSION else FULL_HEADER
    with host_network(radio, own) as (udp, host_ip, _):
        before = _views(udp)
        _need_mtu_keys(before)
        link = before["ppp.mru"] if host_network_kind(radio) == "ppp" else before["net.mtu"]
        payload_len = min(link - FULL_HEADER, before["net.air_mtu"] - header)
        sent = [_payload(0, payload_len)]
        with sdr_bench.lease():
            lis = sdr_bench.listen_start(freq, cc, seconds=12)
            s = _udp_socket(df=False)
            with s:
                s.bind((host_ip, SRC_PORT))
                s.sendto(sent[0], (bench_ip, DST_PORT))
            after = _wait(udp, lambda v: v["ip.sent"] + v["ip.failed"] > before["ip.sent"] + before["ip.failed"] and
                          v.get("airq.depth", 0) == 0, 10, "the datagram sent")
            heard = sdr_bench.listen_result(lis)
    d = _delta(before, after, ["ip.sent", "ip.failed", "ip.sap3", "ip.sap4", "net.air_too_big", "net.frag_needed"])
    print("radio deltas:", d)
    key = "ip.sap3" if sap == dmr_packet.SAP_UDPIP_COMPRESSION else "ip.sap4"
    assert d["ip.sent"] == 1 and d["ip.failed"] == 0 and d[key] == 1 and d["net.air_too_big"] == 0, d
    pkts = dmr_packet.packets(heard)
    mine = _ours(pkts, own)
    assert all(p["blocks"] == _blocks(header + payload_len) for p in mine), [p["blocks"] for p in mine]
    if not mine:
        warnings.warn("the bench modem heard no header of ours: the block count not checked on air")
    whole = _check_heard(pkts, own, sent, sap, f"the largest datagram ({compress})")
    for p in whole:
        assert p["header_octets"] == header, p


@pytest.mark.feature("dmr_ip_gateway")
@pytest.mark.usb_switch
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.parametrize("payload_len", [UP_MAX_FULL, RX_MAX_FULL], ids=["link_mtu", "receive_limit"])
def test_udp_max_air_to_pc(radio, sdr_bench, pytestconfig, payload_len):
    """The largest datagrams from the air: one at the link's MTU (558 octets with the full header) delivered intact to
    this host; one at the receiver's limit (DMR_DATA_MAX_PACKET: 51 blocks, 580 octets, what another make's radio may
    send) reassembled whole but too long for the link: dropped and counted (net.up_too_big), nothing reaches the host."""
    freq, cc, own, _ = _setup(radio, pytestconfig, tx=False)
    with host_network(radio, own) as (udp, host_ip, _):
        before = _views(udp)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.bind((host_ip, HOST_PORT))
            with sdr_bench.lease():
                _bench_udp(sdr_bench, freq, cc, own, payload_len)
            got = _receive(s, 1, 5)
        after = _views(udp)
    d = _delta(before, after, ["data.rx_packets", "data.rx_crc_errors", "net.packets_up", "net.packets_dropped",
                               "net.up_too_big"])
    print("radio deltas:", d, "got:", [len(p) for p, _ in got])
    assert d["data.rx_crc_errors"] == 0 and d["net.packets_dropped"] == 0, d
    if payload_len <= UP_MAX_FULL:
        assert [p for p, _ in got] == [_payload(0, payload_len)], "the datagram didn't arrive intact"
        assert d.get("net.up_too_big", 0) == 0, d
    else:
        assert not got, f"a datagram longer than the link reached the host: {[len(p) for p, _ in got]}"
        assert d["data.rx_packets"] >= 1, d
        if "net.up_too_big" in after:
            assert d["net.up_too_big"] == 1, d


@pytest.mark.feature("dmr_ip_gateway")
@pytest.mark.usb_switch
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_udp_over_receive_limit_air_to_pc(radio, sdr_bench, pytestconfig):
    """One block more than the receiver reassembles (52 blocks): dropped and counted (data.rx_crc_errors), nothing of
    it reaches this host, and a datagram right after it still does."""
    freq, cc, own, _ = _setup(radio, pytestconfig, tx=False)
    with host_network(radio, own) as (udp, host_ip, _):
        before = _views(udp)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.bind((host_ip, HOST_PORT))
            with sdr_bench.lease():
                _bench_udp(sdr_bench, freq, cc, own, RX_MAX_FULL + 1)
                over = _receive(s, 1, 3)
                _bench_udp(sdr_bench, freq, cc, own, 40)
            after_ok = _receive(s, 1, 5)
        after = _views(udp)
    d = _delta(before, after, ["data.rx_packets", "data.rx_crc_errors", "net.packets_up"])
    print("radio deltas:", d, "over:", [len(p) for p, _ in over], "after:", [len(p) for p, _ in after_ok])
    assert not over, f"a datagram past the receiver's limit reached the host: {[len(p) for p, _ in over]}"
    assert d["data.rx_crc_errors"] >= 1, d
    assert [p for p, _ in after_ok] == [_payload(0, 40)], "the next datagram didn't arrive: the receiver is stuck"


@pytest.mark.feature("dmr_ip_gateway")
@pytest.mark.feature("dmr_udp_compress", optional=True)
@pytest.mark.usb_switch
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.parametrize("compress,header", [("auto", UDPC_HEADER), ("off", FULL_HEADER)], ids=["sap3", "sap4"])
def test_udp_over_max_pc_to_air(radio, sdr_bench, pytestconfig, ip_settings, ars_off, compress, header):
    """Firmware with an air MTU below the link's only (_need_narrow_air). One octet more than the air MTU, from this
    host with DF set: never on air (no header of ours heard), counted
    (net.air_too_big) and answered with an ICMP fragmentation needed (net.frag_needed), nothing sent or failed in the
    IP service; the host may refuse it itself instead (EMSGSIZE: it knows the path MTU already), which is as good.
    Then a datagram of the air MTU still goes: no crash, the link still works."""
    _need_narrow_air(_ints(_net_link(radio.link)))      # before the USB switch: on most builds it skips
    freq, cc, own, _ = _setup(radio, pytestconfig)
    radio.link.ip_settings(compress, "rules", 0, [])
    s = _udp_socket(df=True)
    if s is None:
        skip("bench", "this host can't set DF on a UDP socket (IP_MTU_DISCOVER / IP_DONTFRAG)")
    s.close()
    over_ip = str(ipaddress.IPv4Address(INDIVIDUAL_NET | (OVERSIZE_ID & 0xFFFFFF)))
    with host_network(radio, own) as (udp, host_ip, _):
        before = _views(udp)
        _need_narrow_air(before)
        air_mtu = before["net.air_mtu"]
        payload_len = air_mtu - header + 1
        refused_here = False
        with sdr_bench.lease():
            lis = sdr_bench.listen_start(freq, cc, seconds=8)
            with _udp_socket(df=True) as s:
                s.bind((host_ip, SRC_PORT))
                try:
                    s.sendto(_payload(0, payload_len), (over_ip, DST_PORT))
                except OSError as e:
                    refused_here = e.errno == getattr(os, "EMSGSIZE", 40) or "too long" in str(e).lower()
                    if not refused_here:
                        raise
            time.sleep(3)
            heard = sdr_bench.listen_result(lis)
        mid = _views(udp)
        d = _delta(before, mid, ["ip.sent", "ip.failed", "net.to_air", "net.air_too_big", "net.frag_needed"])
        print("radio deltas:", d, "refused by this host:", refused_here)
        assert d["ip.sent"] == 0 and d["ip.failed"] == 0 and d["net.to_air"] == 0, d
        if refused_here:
            assert d["net.air_too_big"] == 0, d
        else:
            assert d["net.air_too_big"] == 1 and d["net.frag_needed"] == 1, d
        mine = [p for p in dmr_packet.packets(heard) if p.get("src") == own]
        assert not mine, f"something of ours went on air: {mine}"
        # The link still works: the air MTU's datagram to the bench goes
        bench_ip = str(ipaddress.IPv4Address(INDIVIDUAL_NET | (BENCH_ID & 0xFFFFFF)))
        with _udp_socket(df=False) as s:
            s.bind((host_ip, SRC_PORT))
            s.sendto(_payload(1, air_mtu - header), (bench_ip, DST_PORT))
        _wait(udp, lambda v: v["ip.sent"] > mid["ip.sent"] and v.get("airq.depth", 0) == 0, 10, "the next datagram sent")


# ── the host learns the air MTU ─────────────────────────────────────────────

def _icmp_socket():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP)
    except PermissionError:
        skip("bench", "receiving the ICMP reply needs a raw socket: run this test as root (sudo)")
    except OSError as e:
        skip("bench", f"no raw ICMP socket on this host: {e}")
    return s


def _host_mtu(host_ip):
    """The MTU of this host's interface with host_ip, None when it can't tell (best effort: ip on Linux, ifconfig
    elsewhere)."""
    try:
        if sys.platform.startswith("linux"):
            out = subprocess.run(["ip", "-o", "-4", "addr", "show"], capture_output=True, text=True, timeout=5).stdout
            for line in out.splitlines():
                if f" {host_ip}/" in line or f" {host_ip} peer " in line:   # peer: a point-to-point link (PPP)
                    name = line.split()[1]
                    with open(f"/sys/class/net/{name}/mtu") as f:
                        return int(f.read())
        else:
            out = subprocess.run(["ifconfig"], capture_output=True, text=True, timeout=5).stdout
            mtu = None
            for line in out.splitlines():
                if not line.startswith(("\t", " ")):
                    mtu = int(line.split("mtu")[1].split()[0]) if " mtu " in line else None
                elif f"inet {host_ip} " in line:
                    return mtu
    except (OSError, ValueError, IndexError, subprocess.SubprocessError):
        pass
    return None


@pytest.mark.feature("dmr_ip_gateway")
@pytest.mark.usb_switch
def test_dhcp_mtu_on_host(radio, pytestconfig):
    """The link MTU as the host took it. USB network: the radio says net.mtu (DHCP option 26); a host whose DHCP client
    applies the option has it on the adapter (many do, macOS may not: a warning, not a failure). PPP (no DHCP): the
    host's interface takes the MRU the radio asked for (ppp.mru, docs/spec/ppp.md 2), and pppd always applies it."""
    _, _, own, _ = _setup(radio, pytestconfig, tx=False)
    ppp = host_network_kind(radio) == "ppp"
    with host_network(radio, own) as (udp, host_ip, _):
        v = _views(udp)
        _need_mtu_keys(v)
        mtu = _host_mtu(host_ip)
    print("host interface MTU:", mtu, "over PPP, ppp.mru:" if ppp else "over USB", v.get("ppp.mru", "") if ppp else "")
    if ppp:
        assert mtu == v["ppp.mru"], f"the host's PPP interface MTU is {mtu}, not the radio's MRU {v['ppp.mru']}"
    elif mtu != LINK_MTU:
        warnings.warn(f"the host's adapter MTU is {mtu}, not the {LINK_MTU} DHCP gave (its client ignores option 26?)")


@pytest.mark.feature("dmr_ip_gateway")
@pytest.mark.usb_switch
def test_frag_needed_received(radio, pytestconfig, ip_settings):
    """Firmware with an air MTU below the link's only (_need_narrow_air). A DF datagram from this host too long for the
    air, to a radio's address: the radio sends back an ICMP
    destination unreachable, fragmentation needed (RFC 1191) from its own address with the air MTU as the next-hop MTU,
    quoting our IPv4 header. Nothing goes on air (the radio refuses it before the air queue), so no RF and no bench:
    a raw ICMP socket on this host (root) sees the reply."""
    _need_narrow_air(_ints(_net_link(radio.link)))      # before the USB switch: on most builds it skips
    _, _, own, _ = _setup(radio, pytestconfig, tx=False)
    radio.link.ip_settings("off", "rules", 0, [])
    probe = _udp_socket(df=True)
    if probe is None:
        skip("bench", "this host can't set DF on a UDP socket (IP_MTU_DISCOVER / IP_DONTFRAG)")
    probe.close()
    icmp = _icmp_socket()
    over_ip = str(ipaddress.IPv4Address(INDIVIDUAL_NET | ((OVERSIZE_ID + 1) & 0xFFFFFF)))
    # The sending socket stays open until the reply is in: Linux applies a UDP datagram's fragmentation needed through
    # the socket that sent it, and drops it when that socket is gone
    with icmp, host_network(radio, own) as (udp, host_ip, radio_ip), _udp_socket(df=True) as s:
        before = _views(udp)
        _need_narrow_air(before)
        air_mtu = before["net.air_mtu"]
        s.bind((host_ip, SRC_PORT))
        try:
            s.sendto(_payload(0, air_mtu - FULL_HEADER + 1), (over_ip, DST_PORT))
        except OSError as e:
            skip("state", f"this host refused the datagram itself ({e}): it already knows the path MTU to {over_ip}")
        reply, end = None, time.monotonic() + 3
        while reply is None and time.monotonic() < end:
            icmp.settimeout(max(0.05, end - time.monotonic()))
            try:
                pkt, addr = icmp.recvfrom(2048)
            except socket.timeout:
                break
            ihl = (pkt[0] & 0x0F) * 4
            m = pkt[ihl:]
            if addr[0] == radio_ip and len(m) >= 8 + 20 and m[0] == ICMP_UNREACHABLE:
                reply = m
        # What the host kept, while the link is up: Linux reports a connected socket's path MTU (IP_MTU) and locks it
        # at net.ipv4.route.min_pmtu (552 by default) when the reply is below that (usb-network.md 3.2): a warning
        kept = None
        if sys.platform.startswith("linux"):
            time.sleep(0.2)
            s.connect((over_ip, DST_PORT))
            kept = s.getsockopt(socket.IPPROTO_IP, getattr(socket, "IP_MTU", 14))
        after = _views(udp)
    d = _delta(before, after, ["net.air_too_big", "net.frag_needed", "net.to_air"])
    print("radio deltas:", d, "reply:", reply[:36].hex() if reply else None, "path MTU the host kept:", kept)
    assert d["net.air_too_big"] == 1 and d["net.frag_needed"] == 1 and d["net.to_air"] == 0, d
    assert reply is not None, "no ICMP destination unreachable from the radio"
    assert reply[1] == ICMP_FRAG_NEEDED and int.from_bytes(reply[6:8], "big") == air_mtu, reply[:8].hex()
    quoted = reply[8:28]
    assert str(ipaddress.IPv4Address(quoted[12:16])) == host_ip and str(ipaddress.IPv4Address(quoted[16:20])) == over_ip
    assert int.from_bytes(quoted[2:4], "big") == air_mtu + 1, quoted.hex()
    if kept is not None and kept != air_mtu:
        warnings.warn(f"the host kept a path MTU of {kept} to {over_ip}, not {air_mtu}: below net.ipv4.route.min_pmtu?")
