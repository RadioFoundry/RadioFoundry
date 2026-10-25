# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The radio's own services in USB network mode (opt-in: --allow-usb-switch): mDNS / DNS-SD (docs/spec/mdns.md) and
TFTP (docs/spec/tftp.md). Each test switches the radio to the network adapter, works over UDP at its address, and
switches it back to serial, as test_usb_network.py does.

  test_mdns          rfoundry-<id>.local resolves to the radio's address (the system resolver: mDNSResponder on macOS,
                     nss-mdns on Linux), and the _rfoundry-debug._udp instance is browsed and resolved to port 3334 with
                     its TXT record: with dns-sd (macOS) or python zeroconf, whichever is there; skips without both
  test_tftp_read     info.txt, two reads of codeplug.bin identical, the current channel's record matching the view
  test_tftp_write    with --allow-codeplug-write too: back up over TFTP, rename one channel, read it back, write the
                     backup again and verify it byte for byte, as test_cps.py does
                     over the CPS. The backup also goes to --codeplug-backup-dir

Nothing is transmitted. Firmware with CONFIG_MDNS / CONFIG_TFTP (the features list says)."""
import contextlib
import ipaddress
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time

import pytest

from rfoundry_hil import DebugLink, as_int, skip
import cps_client
import tftp_client

from test_usb_network import INDIVIDUAL_NET, _serial_back, _udp_link

QDMR = cps_client.REGIONS["qdmr"]
IMAGE = 0xAEE60
BLKSIZE = 554           # the largest the radio takes (docs/spec/tftp.md 1)


@contextlib.contextmanager
def network_mode(radio):
    """The radio as a network adapter for the duration: yields (debug link over UDP, radio address, DMR ID)"""
    own_id = int(radio.view(["dmr"]).get("dmr.own_id", "0"))
    assert 0 < own_id < (1 << 24), "no DMR ID: the gateway can't make an address"
    host_ip = ipaddress.IPv4Address(INDIVIDUAL_NET | own_id)
    radio_ip = str(ipaddress.IPv4Address(int(host_ip) ^ 1))
    radio.link.usb_network(True)
    radio.link.close()
    udp = None
    try:
        udp = _udp_link(radio_ip, 30)
        assert udp is not None, f"no debug link over UDP at {radio_ip} within 30 s (DHCP on the host's adapter?)"
        yield udp, radio_ip, own_id
    finally:
        if udp is not None:
            try:
                udp.usb_network(False)
            except Exception:
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
            except Exception:
                time.sleep(0.25)


def need(radio, feature):
    if feature not in radio.features():
        skip("not_built", f"built without CONFIG_{feature}")


def _run_for(cmd, seconds, until=None):
    """Output of a command that never ends by itself (dns-sd), after seconds or once `until` matches it"""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = ""
    end = time.monotonic() + seconds
    fd = p.stdout.fileno()
    os.set_blocking(fd, False)     # raw reads: a text-mode read of an empty non-blocking pipe fails on Python 3.12
    try:
        while time.monotonic() < end:
            try:
                chunk = os.read(fd, 65536).decode(errors="replace")
            except BlockingIOError:
                chunk = ""
            if chunk:
                out += chunk
                if until is not None and re.search(until, out):
                    break
            time.sleep(0.1)
    finally:
        p.kill()
        p.wait()
    return out


def _legacy_query(radio_ip, host):
    """The A record for host, asked of the radio with a legacy unicast mDNS query: the address, None if no answer"""
    q = struct.pack(">6H", 0x4f44, 0, 1, 0, 0, 0)
    for label in host.split("."):
        q += bytes([len(label)]) + label.encode()
    q += b"\0" + struct.pack(">HH", 1, 1)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(2)
        for _ in range(3):
            s.sendto(q, (radio_ip, 5353))
            try:
                d, _a = s.recvfrom(1500)
            except socket.timeout:
                continue
            if len(d) >= 16 and d[:2] == q[:2] and struct.unpack(">H", d[6:8])[0] > 0:
                return socket.inet_ntoa(d[-4:])     # the answer's A record data ends the message
    return None


def _browse_dns_sd(service):
    """(instance, port, txt dict) with macOS dns-sd, None if not found"""
    out = _run_for(["dns-sd", "-B", service, "local."], 8, r"Add\s.*" + re.escape(service))
    m = re.search(r"Add\s+\S+\s+\S+\s+local\.\s+" + re.escape(service) + r"\.\s+(.+)", out)
    if not m:
        return None, out
    name = m.group(1).strip()
    out2 = _run_for(["dns-sd", "-L", name, service, "local."], 8, r"can be reached at")
    m2 = re.search(r"can be reached at\s+(\S+):(\d+)", out2)
    if not m2:
        return None, out + out2
    txt = dict(re.findall(r"(\w+)=(\S+)", out2.split("can be reached at", 1)[1]))
    return (name, int(m2.group(2)), txt, m2.group(1)), out + out2


def _browse_zeroconf(service):
    try:
        from zeroconf import ServiceBrowser, Zeroconf
    except ImportError:
        return None, "zeroconf not installed"
    zc = Zeroconf()
    found = []

    class Listener:
        def add_service(self, z, t, name):
            found.append(name)

        def update_service(self, z, t, name):
            pass

        def remove_service(self, z, t, name):
            pass

    try:
        ServiceBrowser(zc, service + ".local.", Listener())
        end = time.monotonic() + 8
        while not found and time.monotonic() < end:
            time.sleep(0.2)
        if not found:
            return None, "zeroconf found nothing"
        info = zc.get_service_info(service + ".local.", found[0], timeout=5000)
        if info is None:
            return None, f"zeroconf could not resolve {found[0]}"
        txt = {k.decode(): (v or b"").decode() for k, v in info.properties.items()}
        name = found[0][:-len(service + ".local.") - 1]
        return (name, info.port, txt, info.server), "zeroconf"
    finally:
        zc.close()


@pytest.mark.feature("mdns")
@pytest.mark.usb_switch
def test_mdns(radio):
    need(radio, "MDNS")
    have_dns_sd = shutil.which("dns-sd") is not None
    with network_mode(radio) as (udp, radio_ip, own_id):
        host = f"rfoundry-{own_id}.local"
        v = udp.view(["net"])
        # probing and announcing take about 3 s after the link comes up
        end = time.monotonic() + 10
        while v.get("mdns.state") != "3" and time.monotonic() < end:
            time.sleep(0.5)
            v = udp.view(["net"])
        assert v.get("mdns.state") == "3" and v.get("mdns.name") == f"rfoundry-{own_id}", v

        addr = None
        for _ in range(10):
            try:
                addr = socket.gethostbyname(host)
                break
            except OSError:
                time.sleep(1)
        if addr is None and sys.platform.startswith("linux"):
            print(f"{host} doesn't resolve through the system resolver (no nss-mdns?)")
        else:
            assert addr == radio_ip, f"{host} -> {addr}, expected {radio_ip}"

        if have_dns_sd:
            res, log = _browse_dns_sd("_rfoundry-debug._udp")
        else:
            res, log = _browse_zeroconf("_rfoundry-debug._udp")
            if res is None and log == "zeroconf not installed":
                skip("bench", "neither dns-sd nor python zeroconf: the name resolved, browsing not checked")
        assert res is not None, f"_rfoundry-debug._udp not browsed or resolved: {log}"
        name, port, txt, target = res
        assert port == 3334, res
        assert target.rstrip(".").lower() == host.lower(), res
        assert f"({own_id})" in name, res
        assert txt.get("id") == str(own_id) and txt.get("board") and txt.get("fw"), res
        # The host's resolver may answer from what the radio announced and never ask it: one query of our own (a
        # legacy unicast one, RFC 6762 6.7) checks that the radio answers and counts it
        assert _legacy_query(radio_ip, host) == radio_ip, f"no legacy unicast answer for {host} from {radio_ip}"
        v2 = udp.view(["net"])
        assert as_int(v2.get("mdns.queries"), 0) > 0 and as_int(v2.get("mdns.conflicts"), 0) == 0, v2
        print(f"mDNS: {host} = {addr}, instance {name!r}, TXT {txt}, {v2.get('mdns.queries')} queries")


def _read_image(radio_ip):
    d = tftp_client.get(radio_ip, "codeplug.bin", blksize=BLKSIZE)
    assert len(d) == IMAGE, f"codeplug.bin is {len(d)} bytes"
    return bytearray(d) + bytearray(b"\xff" * (cps_client.IMAGE_SIZE - IMAGE))


@pytest.mark.feature("tftp")
@pytest.mark.usb_switch
def test_tftp_read(radio):
    need(radio, "TFTP")
    core = radio.core()
    with network_mode(radio) as (udp, radio_ip, own_id):
        info = tftp_client.get(radio_ip, "info.txt").decode()
        fields = dict(line.split(": ", 1) for line in info.strip().splitlines())
        assert fields.get("dmr_id") == str(own_id) and fields.get("radio_ip") == radio_ip, info
        t0 = time.monotonic()
        a = _read_image(radio_ip)
        took = time.monotonic() - t0
        b = _read_image(radio_ip)
        d = cps_client.compare(a, b, QDMR, skip=())
        assert not d, f"two reads differ at {len(d)} bytes, first {d[:4]}"
        v = udp.view(["net"])
        assert as_int(v.get("tftp.reads"), 0) >= 3 and as_int(v.get("tftp.timeouts"), 0) == 0, v
        print(f"codeplug.bin: {IMAGE} bytes in {took:.1f} s; info.txt:\n{info}")
    if core.get("codeplug") == "open-firmware" and core.get("vfo") != "1" and as_int(core.get("channel_number"), 0):
        ch = cps_client.decode_channel(a, as_int(core["channel_number"]))
        assert ch is not None and ch["name"] == core["channel_name"], (ch, core.get("channel_name"))


def _pick_channel(image, core):
    number = as_int(core.get("channel_number"), 0) if core.get("vfo") != "1" else 0
    if number == 0 or cps_client.decode_channel(image, number) is None:
        number = next((n for n in range(1, 1025) if cps_client.decode_channel(image, n)), 0)
    return number


@pytest.mark.feature("tftp")
@pytest.mark.usb_switch
@pytest.mark.codeplug_write
def test_tftp_write(radio, backup_dir):
    need(radio, "TFTP")
    core = radio.core()
    with network_mode(radio) as (udp, radio_ip, own_id):
        if as_int(udp.view(["net"]).get("tftp.write_allowed"), 1) == 0:
            skip("state", "TFTP codeplug writes are off on the radio (Settings > Connections)")
        backup = _read_image(radio_ip)
        again = _read_image(radio_ip)
        d = cps_client.compare(backup, again, QDMR, skip=())
        assert not d, f"two codeplug reads differ at {len(d)} bytes: not writing (first {d[:4]})"
        os.makedirs(backup_dir, exist_ok=True)
        path = os.path.join(str(backup_dir), f"codeplug-tftp-{time.strftime('%Y%m%d-%H%M%S')}.bin")
        with open(path, "wb") as f:
            f.write(backup[:IMAGE])
        print(f"codeplug backup: {path} (restore: tools/tftp_client.py {radio_ip} put {path} codeplug.bin, or "
              f"tools/cps_client.py write {path} in serial mode)")
        number = _pick_channel(backup, core)
        if number == 0:
            skip("state", "no stored channel to edit")
        old = cps_client.decode_channel(backup, number)["name"]
        edited = bytearray(backup)
        cps_client.set_channel_name(edited, number, "RadioFoundry TFTP" if old != "RadioFoundry TFTP" else "RadioFoundry TFTP 2")
        changed = cps_client.compare(backup, edited, QDMR, skip=())
        assert changed
        try:
            tftp_client.put(radio_ip, "codeplug.bin", bytes(edited[:IMAGE]), blksize=BLKSIZE)
            back = _read_image(radio_ip)
            d = cps_client.compare(edited, back, QDMR)
            assert not d, f"read back differs at {len(d)} bytes, first {[(hex(x), y, z) for x, y, z in d[:4]]}"
            v = udp.view(["net"])
            assert as_int(v.get("tftp.writes"), 0) >= 1, v
        finally:
            tftp_client.put(radio_ip, "codeplug.bin", bytes(backup[:IMAGE]), blksize=BLKSIZE)
            restored = _read_image(radio_ip)
            d = cps_client.compare(backup, restored, QDMR)
            assert not d, f"RESTORE FAILED at {len(d)} bytes: write {path} back"
            print(f"codeplug restored and verified ({len(changed)} bytes had been changed)")
        # a wrong file is refused and changes nothing
        with pytest.raises(tftp_client.TftpError):
            tftp_client.put(radio_ip, "codeplug.bin", b"\x00" * 1000, blksize=BLKSIZE)
        assert not cps_client.compare(backup, _read_image(radio_ip), QDMR)
