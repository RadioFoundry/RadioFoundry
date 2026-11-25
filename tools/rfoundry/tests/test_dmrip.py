# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The debug link over the air, the host's side (dmrip.py): SipHash and the key as the firmware has them, the datagram,
addressing, and a request through a fake radio that loses datagrams, holds replies and answers only its own ID."""
import os
import socket
import struct
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from rfoundry import dmrip  # noqa: E402
from rfoundry.debuglink import frame, parse_frames  # noqa: E402

K = bytes(range(16))
RADIO_ID = 3101276


class Crypto(unittest.TestCase):
    def test_siphash_reference_vectors(self):
        m = bytes(range(64))
        self.assertEqual(dmrip.siphash24(K, b""), 0x726FDB47DD0E0E31)
        self.assertEqual(dmrip.siphash24(K, m[:1]), 0x74F839C593DC67FD)
        self.assertEqual(dmrip.siphash24(K, m[:8]), 0x93F5F5799A932462)
        self.assertEqual(dmrip.siphash24(K, m[:15]), 0xA129CA6149BE45E5)
        self.assertEqual(dmrip.siphash24(K, m[:63]), 0x958A324CEB064572)

    def test_key_from_the_uid_as_the_firmware_derives_it(self):
        # tests/host: dl_dmrip_kdf over UID bytes 01..0c
        self.assertEqual(dmrip.kdf(bytes(range(1, 13))).hex(), "28d5e608725f52162750448ed15e961b")

    def test_datagram_round_trip_and_tag(self):
        d = dmrip.seal(K, 0, RADIO_ID, 7, 9, 0, b"body")
        self.assertEqual(len(d), dmrip.HDR + 4 + dmrip.TAG)
        self.assertEqual(dmrip.open_dgram(K, d), (0, RADIO_ID, 7, 9, 0, b"body"))
        for i in range(len(d)):
            bad = bytearray(d)
            bad[i] ^= 1
            self.assertIsNone(dmrip.open_dgram(K, bytes(bad)), i)
        self.assertIsNone(dmrip.open_dgram(bytes(16), d))


class Addressing(unittest.TestCase):
    def test_the_radios_individual_address(self):
        self.assertEqual(dmrip.parse_spec("dmrip:3101276"), (3101276, ("12.47.82.92", 3335)))

    def test_a_relay(self):
        self.assertEqual(dmrip.parse_spec("dmrip:3101276@bench.local"), (3101276, ("bench.local", 3335)))
        self.assertEqual(dmrip.parse_spec("dmrip:0x2F525C@10.0.0.2:4000"), (3101276, ("10.0.0.2", 4000)))

    def test_one_radio_only(self):
        for bad in ("dmrip:16777215", "dmrip:0", "dmrip:tg9"):
            with self.assertRaises(dmrip.DmripError, msg=bad):
                dmrip.parse_spec(bad)

    def test_key_file(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "k.json")
            dmrip.save_key(RADIO_ID, K, path)
            self.assertEqual(dmrip.load_key(RADIO_ID, path=path), K)
            with self.assertRaises(dmrip.DmripError):
                dmrip.load_key(RADIO_ID + 1, path=path)
            if os.name == "posix":      # Windows keeps no group or other mode bits
                self.assertEqual(os.stat(path).st_mode & 0o077, 0)


class FakeRadio(threading.Thread):
    """The radio's side (debuglink_dmrip.c) in small: HELLO, the replay check, chunked replies kept for a repeat"""

    def __init__(self, radio_id=RADIO_ID, drop=(), reply_len=600):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.05)
        self.radio_id, self.drop, self.reply_len = radio_id, set(drop), reply_len
        self.nonce, self.last, self.kept, self.sent = 0, 0, None, 0
        self.requests, self.repeats, self.lose_kept = 0, 0, False
        self.boot = 0x5EED
        self.running = True

    def restart(self):
        """A new boot: a new nonce, nothing kept; requests of the old session are dropped unanswered"""
        self.boot += 1
        self.nonce, self.last, self.kept = self.boot, 0, None

    def send(self, to, flags, counter, chunk, body):
        self.sent += 1
        if self.sent in self.drop:
            return
        self.sock.sendto(dmrip.seal(K, dmrip.F_REPLY | flags, self.radio_id, self.nonce, counter, chunk, body), to)

    def reply(self, to, counter, mask):
        f = self.kept
        n = (len(f) + dmrip.CHUNK - 1) // dmrip.CHUNK
        for i in range(n):
            if mask & (1 << i):
                self.send(to, 0, counter, (i << 4) | n, f[i * dmrip.CHUNK:(i + 1) * dmrip.CHUNK])

    def run(self):
        while self.running:
            try:
                d, src = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            m = dmrip.open_dgram(K, d)
            if m is None or m[0] & dmrip.F_REPLY or m[1] != self.radio_id:
                continue
            flags, _, nonce, counter, chunk, body = m
            if flags & dmrip.F_HELLO:
                self.nonce = self.nonce or self.boot
                self.send(src, dmrip.F_HELLO, self.last, 0, body)
                continue
            if nonce != self.nonce:
                continue
            if counter == self.last and self.kept is not None:
                self.repeats += 1
                if self.lose_kept:
                    self.lose_kept = False
                    self.send(src, dmrip.F_GONE, counter, 0, b"")
                    continue
                self.reply(src, counter, chunk or 0xFF)
            elif (counter - self.last) & 0x80000000 == 0 and counter != self.last:
                self.last = counter
                self.requests += 1
                (cmd, seq, payload), = parse_frames(body)
                out = bytes([0]) + bytes((i & 0xFF for i in range(self.reply_len)))
                self.kept = frame(cmd | 0x80, seq, out)
                self.reply(src, counter, 0xFF)

    def stop(self):
        self.running = False
        self.join()
        self.sock.close()


class Transport(unittest.TestCase):
    def port(self, radio, radio_id=RADIO_ID):
        p = dmrip.DmripPort("dmrip:%d@127.0.0.1:%d" % (radio_id, radio.sock.getsockname()[1]), K, timeout=0.3,
                            tries=3)
        self.addCleanup(p.close)
        return p

    def request(self, p, seq=1):
        p.write(frame(0x01, seq, b"x"))
        (cmd, rseq, data), = parse_frames(p.read(4096))
        return cmd, rseq, data

    def radio(self, **kw):
        r = FakeRadio(**kw)
        r.start()
        self.addCleanup(r.stop)
        return r

    def test_a_reply_in_chunks(self):
        r = self.radio()
        cmd, seq, data = self.request(self.port(r))
        self.assertEqual((cmd, seq, len(data)), (0x81, 1, 601))
        self.assertEqual(r.requests, 1)

    def test_a_lost_chunk_is_asked_for_alone(self):
        r = self.radio(drop={3})     # HELLO is datagram 1: the reply's second chunk is lost
        p = self.port(r)
        self.assertEqual(len(self.request(p)[2]), 601)
        self.assertEqual((r.requests, r.repeats), (1, 1))
        self.assertEqual(p.stats["resent"], 1)

    def test_a_reply_the_radio_no_longer_holds_is_asked_as_new(self):
        r = self.radio(drop={2})
        r.lose_kept = True
        p = self.port(r)
        self.assertEqual(len(self.request(p)[2]), 601)
        self.assertEqual(r.requests, 2)

    def test_a_change_whose_reply_is_lost_runs_once(self):
        # A detour RUN the radio took, its reply lost twice over (the datagram, then the buffer): never sent as new
        r = self.radio(drop={2})
        r.lose_kept = True
        p = self.port(r)
        with self.assertRaises(dmrip.ReplyLost):
            p.write(frame(dmrip.CMD_DETOUR, 1, dmrip.detour_run(3, [(0, 100)])))
        self.assertEqual((r.requests, r.repeats, p.stats["gone"]), (1, 1, 1))

    def test_detour_status_is_a_read(self):
        r = self.radio(drop={2})
        r.lose_kept = True
        p = self.port(r)
        p.write(frame(dmrip.CMD_DETOUR, 1, bytes([dmrip.DETOUR_STATUS])))
        self.assertEqual(r.requests, 2)

    def test_which_requests_only_read(self):
        reads = [(0x01, b"x"), (0x09, bytes(5)), (dmrip.CMD_DETOUR, bytes([dmrip.DETOUR_STATUS])),
                 (dmrip.CMD_DMRIP, bytes([dmrip.DMRIP_STATUS]))]
        changes = [(0x04, b""), (0x08, b""), (0x20, bytes(3)), (dmrip.CMD_DETOUR, dmrip.detour_run(5, [])),
                   (dmrip.CMD_DETOUR, bytes([dmrip.DETOUR_RETURN])), (dmrip.CMD_DETOUR, b""),
                   (dmrip.CMD_DMRIP, bytes([dmrip.DMRIP_KEY]) + bytes(16))]
        for cmd, payload in reads:
            self.assertTrue(dmrip.only_reads(frame(cmd, 1, payload)), hex(cmd))
        for cmd, payload in changes:
            self.assertFalse(dmrip.only_reads(frame(cmd, 1, payload)), (hex(cmd), payload[:1]))

    def test_a_restarted_radio_gets_a_new_session(self):
        r = self.radio()
        p = self.port(r)
        self.request(p)
        r.restart()
        self.assertEqual(self.request(p, 2)[1], 2)      # a read: sent again in the new session
        self.assertEqual(p.nonce, r.nonce)
        r.restart()
        with self.assertRaises(dmrip.ReplyLost):        # a change: may have run before the restart, never resent
            p.write(frame(dmrip.CMD_DETOUR, 3, dmrip.detour_run(3, [(0, 100)])))
        self.assertEqual(r.requests, 2)

    def test_counters_go_up(self):
        r = self.radio()
        p = self.port(r)
        for seq in range(1, 4):
            self.assertEqual(self.request(p, seq)[1], seq)
        self.assertEqual(r.requests, 3)

    def test_another_radio_stays_silent(self):
        r = self.radio(radio_id=RADIO_ID + 1)
        with self.assertRaises(dmrip.DmripError):
            self.request(self.port(r))
        self.assertEqual(r.requests, 0)


class Detour(unittest.TestCase):
    def test_run_payload(self):
        p = dmrip.detour_run(30, [(0x0A, bytes([5, 1, 2, 3, 4])), (0, 500)])
        self.assertEqual(p, bytes([1, 30, 0, 0x0A, 5, 5, 1, 2, 3, 4, 0, 2]) + struct.pack("<H", 500))
        for bad in (0, 301):
            with self.assertRaises(dmrip.DmripError):
                dmrip.detour_run(bad, [])

    def test_status(self):
        r = struct.pack("<BIBBBI", 3, 1234, 2, 0, 0, 5)
        self.assertEqual(dmrip.decode_detour_status(r)["phase"], "running")
        last = struct.pack("<BBHBIIIIHHHh", 4, 2, 1, 1, 461462500, 31000, 2000, 0, 3, 0, 0, -80)
        s = dmrip.decode_detour_status(r + last)
        self.assertEqual((s["last"]["why"], s["last"]["restored"], s["last"]["rssi_max"]), ("backstop", True, -80))


if __name__ == "__main__":
    unittest.main()
