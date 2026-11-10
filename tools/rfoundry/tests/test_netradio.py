# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""A radio in USB network mode (debuglink.py): where it is from the host's side of the link, the serial byte stream
over UDP, and the search that falls back to it."""
import ipaddress
import os
import socket
import sys
import threading
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from rfoundry import debuglink  # noqa: E402

ip = ipaddress.IPv4Address


class Candidates(unittest.TestCase):
    def test_individual_addresses_pair_by_the_last_bit(self):
        self.assertEqual(debuglink.radio_address_candidates([], [ip("12.47.131.134")]), [ip("12.47.131.135")])
        self.assertEqual(debuglink.radio_address_candidates([], [ip("12.47.131.135")]), [ip("12.47.131.134")])

    def test_fixed_link_is_the_host_minus_one(self):
        self.assertEqual(debuglink.radio_address_candidates([], [ip("192.168.10.2"), ip("10.9.8.2")]),
                         [ip("192.168.10.1"), ip("10.9.8.1")])

    def test_a_host_without_a_radio_adapter_sends_nothing(self):
        # the system names the radio's adapter and there is none: no other network is probed, 12.x or the fixed
        # link's default range included
        with mock.patch.object(debuglink, "host_addresses", side_effect=AssertionError("not asked")):
            self.assertEqual(debuglink.radio_address_candidates(None, []), [])
        self.assertEqual(debuglink.radio_address_candidates([ip("192.168.10.2"), ip("12.0.0.9")], []), [])

    def test_where_the_adapter_is_unknown_only_the_radio_range_counts(self):
        hosts = [ip("192.168.3.136"), ip("192.168.10.2"), ip("100.95.94.3"), ip("12.0.39.21"), ip("12.0.39.21")]
        with mock.patch.object(debuglink, "network_adapter_addresses", return_value=None):
            self.assertEqual(debuglink.radio_address_candidates(hosts), [ip("12.0.39.20")])
            self.assertEqual(debuglink.radio_address_candidates([ip("192.168.10.2"), ip("10.0.0.5")]), [])


class Udp(unittest.TestCase):
    def setUp(self):
        self.radio = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.radio.bind(("127.0.0.1", 0))
        self.radio.settimeout(2)
        self.addr = "127.0.0.1:%d" % self.radio.getsockname()[1]
        self.addCleanup(self.radio.close)

    def test_bytes_both_ways_like_a_serial_port(self):
        s = debuglink.UdpSerial(self.addr, timeout=0.5)
        self.addCleanup(s.close)
        self.assertEqual(s.write(b"\xA5\x5Aabc"), 5)
        data, src = self.radio.recvfrom(64)
        self.assertEqual(data, b"\xA5\x5Aabc")
        self.radio.sendto(b"12345", src)
        self.radio.sendto(b"678", src)
        self.assertEqual(s.read(2), b"12")      # a datagram is a piece of the stream, not a message
        self.assertEqual(s.read(16), b"345678")
        self.assertEqual(s.read(1), b"")        # nothing more: empty after the timeout, as pyserial
        self.radio.sendto(b"stale", src)
        s.timeout = 0.2
        s.read(0)
        s.reset_input_buffer()
        self.assertEqual(s.in_waiting, 0)

    def test_link_speaks_to_a_radio_over_udp(self):
        def answer():
            data, src = self.radio.recvfrom(256)
            seq = data[3]   # SOF SOF cmd seq len(2) payload crc(2): docs/spec/debuglink.md 2
            self.radio.sendto(debuglink.frame(debuglink.CMD_VERSION | 0x80, seq, b"\0board=XR-100\ngit=abc\n"), src)
        t = threading.Thread(target=answer)
        t.start()
        link = debuglink.Link("udp:" + self.addr)
        try:
            self.assertEqual(link.version()["board"], "XR-100")
        finally:
            link.close()
            t.join()


class Search(unittest.TestCase):
    def test_only_radios_that_answer_are_found(self):
        class Fake:
            def __init__(self, port, timeout=1.0):
                self.port = port

            def version(self):
                if self.port != "udp:12.0.0.3":
                    raise debuglink.LinkError("no answer")
                return {}

            def close(self):
                pass
        with mock.patch.object(debuglink, "Link", Fake):
            self.assertEqual(debuglink.find_network_radios([ip("12.0.0.3"), ip("192.168.10.1")]), ["udp:12.0.0.3"])

    def test_probe_falls_back_to_the_network_radio_only_without_a_serial_port(self):
        seen = []

        class Fake:
            def __init__(self, port, timeout=1.0):
                seen.append(port)

            def version(self):
                return {"board": "XR-100", "git": "abc"}

            def chip_uid(self):
                return "00"

            def close(self):
                pass
        with mock.patch.object(debuglink, "Link", Fake), \
                mock.patch.object(debuglink, "find_network_radios", return_value=["udp:12.0.0.3"]) as net:
            with mock.patch.object(debuglink, "find_ports", return_value=[]):
                self.assertEqual(debuglink.probe()["port"], "udp:12.0.0.3")
            with mock.patch.object(debuglink, "find_ports", return_value=["/dev/ttyACM0"]):
                self.assertEqual(debuglink.probe()["port"], "/dev/ttyACM0")
            self.assertEqual(net.call_count, 1)


if __name__ == "__main__":
    unittest.main()
