# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""A minimal PPP peer in user space, for HIL tests of a radio's PPP host link (docs/spec/ppp.md) without pppd or root.
Written from RFC 1661 (LCP), RFC 1662 (HDLC-like framing) and RFC 1332 (IPCP).

It runs on an open port-like object (read, write; the debug link's serial port), negotiates LCP and IPCP as pppd
would with `noipdefault` (asks for 0.0.0.0 and takes the radio's Nak), then carries IPv4: ICMP echo and UDP.
`UdpPort` gives a UDP port on the radio as a port-like object, so tools/rfoundry_debug.py's DebugLink runs over it."""
import random
import struct
import time

FLAG, ESC = 0x7E, 0x7D
PROTO_LCP, PROTO_IPCP, PROTO_IP = 0xC021, 0x8021, 0x0021
CONF_REQ, CONF_ACK, CONF_NAK, CONF_REJ, TERM_REQ, TERM_ACK = 1, 2, 3, 4, 5, 6
ECHO_REQ, ECHO_REP = 9, 10
OPT_IP_ADDRESS = 3


class PppError(Exception):
    pass


def fcs16(data, fcs=0xFFFF):
    """RFC 1662 C.2, bit by bit (polynomial 0x8408, reflected)."""
    for b in data:
        fcs ^= b
        for _ in range(8):
            fcs = (fcs >> 1) ^ 0x8408 if fcs & 1 else fcs >> 1
    return fcs


def encode(proto, info):
    """One frame with address, control and FCS, every control character escaped (the ACCM we never narrow)."""
    body = bytes([0xFF, 0x03]) + struct.pack(">H", proto) + bytes(info)
    fcs = fcs16(body) ^ 0xFFFF
    body += struct.pack("<H", fcs)
    out = bytearray([FLAG])
    for b in body:
        if b < 0x20 or b in (FLAG, ESC):
            out += bytes([ESC, b ^ 0x20])
        else:
            out.append(b)
    out.append(FLAG)
    return bytes(out)


def ip_checksum(data):
    if len(data) & 1:
        data += b"\0"
    s = sum(struct.unpack(f">{len(data) // 2}H", data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def ip_bytes(addr):
    return bytes(int(x) for x in addr.split("."))


def ip_str(b):
    return ".".join(str(x) for x in b)


class PppClient:
    """PPP on an open port (pyserial-like: read(n), write(data), in_waiting). open() negotiates; then ping(), udp_send()
    and the UDP receive queue; close() sends LCP Terminate-Request."""

    def __init__(self, port):
        self.port = port
        self.buf = bytearray()
        self.esc = False
        self.ident = 1
        self.lcp_open = self.ipcp_open = False
        self.lcp_acked_ours = self.lcp_acked_theirs = False
        self.ipcp_acked_ours = self.ipcp_acked_theirs = False
        self.local_ip = "0.0.0.0"
        self.peer_ip = None
        self.udp_in = {}            # our port -> list of (src ip, src port, payload)
        self.icmp_in = []
        self.bad_frames = 0
        self.ip_id = random.randrange(0x10000)
        self.magic = random.randrange(1, 1 << 32)
        self.terminated = False

    # ── framing ──
    def send(self, proto, info):
        self.port.write(encode(proto, info))

    def _frames(self):
        """The good frames among the bytes waiting now: (protocol, information)."""
        n = getattr(self.port, "in_waiting", 0) or 1
        data = self.port.read(n)
        out = []
        for b in data:
            if b == FLAG:
                if len(self.buf) >= 4:
                    f = bytes(self.buf)
                    if fcs16(f) == 0xF0B8 and f[0] == 0xFF and f[1] == 0x03:
                        out.append((struct.unpack(">H", f[2:4])[0], f[4:-2]))
                    else:
                        self.bad_frames += 1
                self.buf.clear()
                self.esc = False
            elif b == ESC:
                self.esc = True
            else:
                self.buf.append(b ^ 0x20 if self.esc else b)
                self.esc = False
        return out

    def pump(self, seconds=0.05):
        """Read and handle frames for about this long."""
        end = time.monotonic() + seconds
        while True:
            for proto, info in self._frames():
                self._handle(proto, info)
            if time.monotonic() >= end:
                return

    def _next_id(self):
        self.ident = (self.ident + 1) & 0xFF
        return self.ident

    # ── LCP / IPCP ──
    def _handle(self, proto, info):
        if len(info) < 4 and proto in (PROTO_LCP, PROTO_IPCP):
            return
        if proto == PROTO_LCP:
            code, ident = info[0], info[1]
            length = struct.unpack(">H", info[2:4])[0]
            data = info[4:length]
            if code == CONF_REQ:
                self.send(PROTO_LCP, bytes([CONF_ACK, ident]) + struct.pack(">H", length) + data)   # take them all
                self.lcp_acked_theirs = True
            elif code == CONF_ACK:
                self.lcp_acked_ours = True
            elif code in (CONF_NAK, CONF_REJ):
                self._lcp_request()      # ours is empty: nothing to give up, ask again
            elif code == ECHO_REQ and self.lcp_open:
                self.send(PROTO_LCP, bytes([ECHO_REP, ident]) + struct.pack(">HI", 8, self.magic))
            elif code == TERM_REQ:
                self.send(PROTO_LCP, bytes([TERM_ACK, ident]) + struct.pack(">H", 4))
                self.lcp_open = self.ipcp_open = False
                self.terminated = True
            elif code == TERM_ACK:
                self.terminated = True
            self.lcp_open = self.lcp_acked_ours and self.lcp_acked_theirs
        elif proto == PROTO_IPCP and self.lcp_open:
            code, ident = info[0], info[1]
            length = struct.unpack(">H", info[2:4])[0]
            data = info[4:length]
            opts = self._options(data)
            if code == CONF_REQ:
                if OPT_IP_ADDRESS in opts:
                    self.peer_ip = ip_str(opts[OPT_IP_ADDRESS])
                self.send(PROTO_IPCP, bytes([CONF_ACK, ident]) + struct.pack(">H", length) + data)
                self.ipcp_acked_theirs = True
            elif code == CONF_ACK:
                self.ipcp_acked_ours = True
            elif code == CONF_NAK and OPT_IP_ADDRESS in opts:
                self.local_ip = ip_str(opts[OPT_IP_ADDRESS])
                self._ipcp_request()
            elif code == CONF_REJ:
                raise PppError(f"the radio rejected our IPCP request ({data.hex()}): no address (DMR ID 0?)")
            self.ipcp_open = self.ipcp_acked_ours and self.ipcp_acked_theirs
        elif proto == PROTO_IP and self.ipcp_open:
            self._ip_in(info)

    @staticmethod
    def _options(data):
        opts, i = {}, 0
        while i + 2 <= len(data):
            t, n = data[i], data[i + 1]
            if n < 2:
                break
            opts[t] = data[i + 2:i + n]
            i += n
        return opts

    def _lcp_request(self):
        self.send(PROTO_LCP, bytes([CONF_REQ, self._next_id()]) + struct.pack(">H", 4))

    def _ipcp_request(self):
        opt = bytes([OPT_IP_ADDRESS, 6]) + ip_bytes(self.local_ip)
        self.send(PROTO_IPCP, bytes([CONF_REQ, self._next_id()]) + struct.pack(">H", 4 + len(opt)) + opt)

    def open(self, timeout=15.0):
        """LCP, then IPCP. Returns (our address, the radio's)."""
        end = time.monotonic() + timeout
        last = 0
        while not self.lcp_open:
            if time.monotonic() > end:
                raise PppError("LCP didn't open")
            if time.monotonic() - last > 1.0:
                self._lcp_request()
                last = time.monotonic()
            self.pump(0.05)
        last = 0
        while not self.ipcp_open:
            if time.monotonic() > end:
                raise PppError(f"IPCP didn't open (ours acked {self.ipcp_acked_ours}, theirs {self.ipcp_acked_theirs})")
            if time.monotonic() - last > 1.0:
                self._ipcp_request()
                last = time.monotonic()
            self.pump(0.05)
        return self.local_ip, self.peer_ip

    def close(self, timeout=5.0):
        """LCP Terminate-Request until the Terminate-Ack. True when it came."""
        end = time.monotonic() + timeout
        last = 0
        while not self.terminated and time.monotonic() < end:
            if time.monotonic() - last > 1.0:
                self.send(PROTO_LCP, bytes([TERM_REQ, self._next_id()]) + struct.pack(">H", 4))
                last = time.monotonic()
            self.pump(0.05)
        self.lcp_open = self.ipcp_open = False
        return self.terminated

    # ── IPv4 ──
    def _ip_send(self, proto, payload, dst=None):
        dst = dst or self.peer_ip
        self.ip_id = (self.ip_id + 1) & 0xFFFF
        hdr = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), self.ip_id, 0, 64, proto, 0,
                          ip_bytes(self.local_ip), ip_bytes(dst))
        hdr = hdr[:10] + struct.pack(">H", ip_checksum(hdr)) + hdr[12:]
        self.send(PROTO_IP, hdr + payload)

    def _ip_in(self, pkt):
        if len(pkt) < 20 or pkt[0] >> 4 != 4:
            return
        ihl = (pkt[0] & 0x0F) * 4
        total = struct.unpack(">H", pkt[2:4])[0]
        if ip_checksum(bytes(pkt[:ihl])) != 0:
            self.bad_frames += 1
            return
        proto, src, body = pkt[9], ip_str(pkt[12:16]), pkt[ihl:total]
        if proto == 1 and len(body) >= 8 and body[0] == 0:          # ICMP echo reply
            self.icmp_in.append((src, struct.unpack(">HH", body[4:8]), bytes(body[8:])))
        elif proto == 17 and len(body) >= 8:
            sport, dport, ulen = struct.unpack(">HHH", body[:6])
            self.udp_in.setdefault(dport, []).append((src, sport, bytes(body[8:ulen])))

    def ping(self, size=56, timeout=3.0, ident=0x5250, seq=1):
        """One ICMP echo to the radio. The round trip in seconds, or None."""
        data = bytes((i * 7 + seq) & 0xFF for i in range(size))
        msg = struct.pack(">BBHHH", 8, 0, 0, ident, seq) + data
        msg = msg[:2] + struct.pack(">H", ip_checksum(msg)) + msg[4:]
        t0 = time.monotonic()
        self._ip_send(1, msg)
        while time.monotonic() - t0 < timeout:
            self.pump(0.02)
            for i, (src, (rid, rseq), rdata) in enumerate(self.icmp_in):
                if src == self.peer_ip and rid == ident and rseq == seq and rdata == data:
                    del self.icmp_in[i]
                    return time.monotonic() - t0
        return None

    def udp_send(self, sport, dport, payload, dst=None):
        dst = dst or self.peer_ip
        udp = struct.pack(">HHHH", sport, dport, 8 + len(payload), 0) + bytes(payload)
        pseudo = ip_bytes(self.local_ip) + ip_bytes(dst) + struct.pack(">BBH", 0, 17, len(udp))
        csum = ip_checksum(pseudo + udp) or 0xFFFF
        self._ip_send(17, udp[:6] + struct.pack(">H", csum) + udp[8:], dst)


class UdpPort:
    """A UDP port on the radio through PPP, as a port-like object (read, write, in_waiting, close) for DebugLink(s=...).
    Each write is one datagram, as the debug link's UDP transport sends a frame."""

    def __init__(self, ppp, dport=3334, sport=43334):
        self.ppp, self.dport, self.sport = ppp, dport, sport
        self.rx = bytearray()

    def _take(self):
        for src, sport, data in self.ppp.udp_in.pop(self.sport, []):
            if src == self.ppp.peer_ip and sport == self.dport:
                self.rx += data

    @property
    def in_waiting(self):
        self.ppp.pump(0)
        self._take()
        return len(self.rx)

    def read(self, n=1):
        if not self.rx:
            self.ppp.pump(0.02)
            self._take()
        out = bytes(self.rx[:n])
        del self.rx[:n]
        return out

    def write(self, data):
        self.ppp.udp_send(self.sport, self.dport, data)
        return len(data)

    def close(self):
        pass
