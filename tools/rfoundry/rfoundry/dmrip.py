# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The debug link over the air (docs/spec/debuglink.md 7): debug link frames to one radio as UDP datagrams over the DMR
IP data path, through an IP gateway (a radio in USB network mode, or a bench IP gateway), and the replies back.

DmripPort looks enough like a pyserial port for a debug link client: write() takes one request frame and sends it
in a datagram, read() gives the reply frame's bytes once every chunk came. Lost datagrams are asked for again (the
same counter: the radio sends its kept reply, or only the chunks still missing). A request the radio took is never
sent as new again unless it only reads: when the radio no longer holds its reply (GONE), a read goes again under a new
counter and anything else raises ReplyLost, so a detour never runs twice.

Port syntax: dmrip:RADIOID sends to the radio's individual address (12.x.y.z from its DMR ID) by the host's routes;
dmrip:RADIOID@HOST[:PORT] sends to a relay at HOST:PORT (default 3335) that forwards each datagram to the DMR ID in
its header. The key: --dmrip-key, $RFOUNDRY_DMRIP_KEY, or the key file (dmrip-keys.json in the rfoundry config
directory) that `rfoundry_debug.py dmrip learn` fills over the cable.
"""
import json
import os
import socket
import struct
import time

PORT = 3335
VERSION = 0xD1
HDR = 14
TAG = 8
KEY_LEN = 16
CHUNK = 224
F_REPLY, F_HELLO, F_GONE = 0x01, 0x02, 0x04
INDIVIDUAL_NET = 0x0C000000         # 12.0.0.0/8: a DMR ID's individual address
KDF_KEY = b"RFoundryDMRIPkey"
CMD_DETOUR, CMD_DMRIP = 0x27, 0x28
DETOUR_RUN, DETOUR_EXTEND, DETOUR_RETURN, DETOUR_STATUS = 1, 2, 3, 4
DETOUR_MAX_S = 300
DETOUR_PHASES = ["idle", "arming", "clear", "running", "returning"]
DMRIP_STATUS, DMRIP_KEY = 0, 1

M64 = (1 << 64) - 1


class DmripError(Exception):
    pass


class ReplyLost(DmripError):
    """The radio took the request but no longer holds its reply (GONE), and the request changes the radio: it is not
    sent again. Read the state the request changed (DETOUR STATUS, the view) to learn what happened"""


# Requests the air takes that only read, by command (and first payload byte where the command also changes things):
# safe to send again as new after GONE. GET_LOG is missing on purpose: it hands each line over once
CMD_PING, CMD_VERSION, CMD_GET_DIAG, CMD_SCREENSHOT, CMD_GET_VIEW, CMD_IDDB = 0x01, 0x02, 0x03, 0x05, 0x09, 0x1F
READS = {CMD_PING: None, CMD_VERSION: None, CMD_GET_DIAG: None, CMD_SCREENSHOT: None, CMD_GET_VIEW: None,
         CMD_IDDB: None, CMD_DETOUR: (DETOUR_STATUS,), CMD_DMRIP: (DMRIP_STATUS,)}


def _rotl(x, b):
    return ((x << b) | (x >> (64 - b))) & M64


def siphash24(key, data):
    """SipHash-2-4, 64-bit output (the firmware's dl_siphash)"""
    k0, k1 = struct.unpack("<QQ", key)
    v0, v1 = 0x736F6D6570736575 ^ k0, 0x646F72616E646F6D ^ k1
    v2, v3 = 0x6C7967656E657261 ^ k0, 0x7465646279746573 ^ k1

    def rounds(n):
        nonlocal v0, v1, v2, v3
        for _ in range(n):
            v0 = (v0 + v1) & M64
            v1 = _rotl(v1, 13) ^ v0
            v0 = _rotl(v0, 32)
            v2 = (v2 + v3) & M64
            v3 = _rotl(v3, 16) ^ v2
            v0 = (v0 + v3) & M64
            v3 = _rotl(v3, 21) ^ v0
            v2 = (v2 + v1) & M64
            v1 = _rotl(v1, 17) ^ v2
            v2 = _rotl(v2, 32)

    n = len(data)
    whole = n & ~7
    for i in range(0, whole, 8):
        (m,) = struct.unpack_from("<Q", data, i)
        v3 ^= m
        rounds(2)
        v0 ^= m
    b = (n & 0xFF) << 56
    for i, c in enumerate(data[whole:]):
        b |= c << (8 * i)
    v3 ^= b
    rounds(2)
    v0 ^= b
    v2 ^= 0xFF
    rounds(4)
    return v0 ^ v1 ^ v2 ^ v3


def kdf(uid):
    """The radio's default key from its chip UID (INFO_CHIP's 12 bytes): the firmware's dl_dmrip_kdf"""
    uid = bytes(uid)[:32]
    return struct.pack("<QQ", siphash24(KDF_KEY, b"\x00" + uid), siphash24(KDF_KEY, b"\x01" + uid))


def seal(key, flags, radio_id, nonce, counter, chunk, body):
    d = struct.pack("<BB", VERSION, flags) + struct.pack("<I", radio_id)[:3] + struct.pack("<IIB", nonce, counter,
                                                                                            chunk) + bytes(body)
    return d + struct.pack("<Q", siphash24(key, d))


def open_dgram(key, d):
    """(flags, radio_id, nonce, counter, chunk, body), or None when the version, length or tag is wrong"""
    if len(d) < HDR + TAG or len(d) > HDR + CHUNK + TAG or d[0] != VERSION:
        return None
    if struct.unpack_from("<Q", d, len(d) - TAG)[0] != siphash24(key, d[:-TAG]):
        return None
    flags = d[1]
    radio_id = d[2] | (d[3] << 8) | (d[4] << 16)
    nonce, counter, chunk = struct.unpack_from("<IIB", d, 5)
    return flags, radio_id, nonce, counter, chunk, bytes(d[HDR:-TAG])


def parse_spec(spec):
    """dmrip:RADIOID[@HOST[:PORT]] -> (radio_id, (host, port)); the address is the radio's own without a relay"""
    s = spec[6:] if spec.startswith("dmrip:") else spec
    rid, _, relay = s.partition("@")
    try:
        radio_id = int(rid, 0)
    except ValueError:
        raise DmripError(f"{spec}: the DMR ID is not a number (dmrip:RADIOID[@HOST[:PORT]])")
    if not 1 <= radio_id <= 0xFFFFFE:
        raise DmripError(f"{spec}: DMR ID {radio_id} is not one radio's (1..16777214)")
    if relay:
        host, _, port = relay.partition(":")
        return radio_id, (host, int(port or PORT))
    a = INDIVIDUAL_NET | radio_id
    return radio_id, ("%d.%d.%d.%d" % (a >> 24, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF), PORT)


def key_file():
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "rfoundry", "dmrip-keys.json")


def load_key(radio_id, key=None, path=None):
    """The key for a radio: given (hex), $RFOUNDRY_DMRIP_KEY, else the key file's entry for its DMR ID"""
    key = key or os.environ.get("RFOUNDRY_DMRIP_KEY")
    if not key:
        try:
            with open(path or key_file()) as f:
                key = json.load(f).get(str(radio_id))
        except (OSError, ValueError):
            key = None
    if not key:
        raise DmripError(f"no key for DMR ID {radio_id}: give --dmrip-key, or run `rfoundry_debug.py dmrip learn` "
                         "over the cable first")
    try:
        k = bytes.fromhex(key)
    except ValueError:
        k = b""
    if len(k) != KEY_LEN:
        raise DmripError("a key is 32 hex digits")
    return k


def save_key(radio_id, key, path=None):
    path = path or key_file()
    try:
        with open(path) as f:
            keys = json.load(f)
    except (OSError, ValueError):
        keys = {}
    keys[str(radio_id)] = bytes(key).hex()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(keys, f, indent=1, sort_keys=True)
    return path


def only_reads(frame_bytes):
    """A debug link request frame that only reads (READS): the radio may run it twice"""
    if len(frame_bytes) < 6 or frame_bytes[2] not in READS:
        return False
    ops = READS[frame_bytes[2]]
    return ops is None or (len(frame_bytes) > 6 + 2 and frame_bytes[6] in ops)


class DmripPort:
    """A debug link port over the air. timeout: how long one datagram's answer is waited for before it is sent again;
    tries: how often"""

    def __init__(self, spec, key, timeout=4.0, tries=4, sock=None):
        self.radio_id, self.addr = parse_spec(spec)
        self.key = bytes(key)
        self.timeout, self.tries = timeout, tries
        self.sock = sock or socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        if sock is None:
            self.sock.bind(("", 0))
        self.nonce = 0
        self.counter = 0
        self.pending = bytearray()
        self.stats = {"sent": 0, "resent": 0, "chunks": 0, "dropped": 0, "gone": 0}

    # -- the datagrams --
    def _send(self, flags, counter, chunk, body):
        d = seal(self.key, flags, self.radio_id, self.nonce, counter, chunk, body)
        self.sock.sendto(d, self.addr)
        self.stats["sent"] += 1

    def _recv(self, deadline):
        """The next datagram from the radio that opens under the key and names it, or None at the deadline"""
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            self.sock.settimeout(left)
            try:
                d, _ = self.sock.recvfrom(2048)
            except (socket.timeout, BlockingIOError):
                return None
            except ConnectionResetError:
                continue
            m = open_dgram(self.key, d)
            if m is None or not m[0] & F_REPLY or m[1] != self.radio_id:
                self.stats["dropped"] += 1
                continue
            return m

    def hello(self):
        """Start a session: the radio's nonce and the last counter it took"""
        challenge = os.urandom(8)
        for _ in range(self.tries):
            self.nonce = 0
            self._send(F_HELLO, 0, 0, challenge)
            end = time.monotonic() + self.timeout
            while True:
                m = self._recv(end)
                if m is None:
                    break
                flags, _, nonce, counter, _, body = m
                if flags & F_HELLO and body == challenge and nonce:
                    self.nonce, self.counter = nonce, counter
                    return nonce
            time.sleep(1.0)     # HELLO is answered once a second at most
        raise DmripError(f"no answer from DMR ID {self.radio_id} at {self.addr[0]}:{self.addr[1]} (key, route, "
                         "the radio's channel?)")

    def transact(self, frame_bytes):
        """One request frame out, its reply frame back (bytes). A radio that restarted (a new nonce per boot) drops
        every request of the old session unanswered: after a request gets no reply, a HELLO tells; a read is then sent
        again in the new session, anything else is reported (the radio may have run it before the restart)"""
        if not self.nonce:
            self.hello()
        try:
            return self._transact(frame_bytes)
        except ReplyLost:
            raise
        except DmripError:
            old = self.nonce
            try:
                self.hello()
            except DmripError:
                self.nonce = old
                raise
            if self.nonce == old:
                raise
            if not only_reads(frame_bytes):
                raise ReplyLost(f"DMR ID {self.radio_id} restarted (new session): request (command "
                                f"0x{frame_bytes[2]:02X}) may or may not have run before; not sent again")
            return self._transact(frame_bytes)

    def _transact(self, frame_bytes):
        self.counter = (self.counter + 1) & 0xFFFFFFFF
        counter = self.counter
        parts, count, mask = {}, 0, 0
        for attempt in range(self.tries):
            if attempt:
                self.stats["resent"] += 1
            self._send(0, counter, mask if count else 0, frame_bytes)
            end = time.monotonic() + self.timeout
            while True:
                m = self._recv(end)
                if m is None:
                    break
                flags, _, nonce, rcounter, chunk, body = m
                if nonce != self.nonce or rcounter != counter:
                    continue
                if flags & F_GONE:
                    # The radio took the request, then the cable used its reply buffer. Only a read goes again, as
                    # new: anything else (a detour RUN) already ran once and must not run twice
                    self.stats["gone"] += 1
                    if not only_reads(frame_bytes):
                        raise ReplyLost(f"DMR ID {self.radio_id} took request {counter} (command "
                                        f"0x{frame_bytes[2]:02X}) but lost its reply: not sent again")
                    self.counter = (self.counter + 1) & 0xFFFFFFFF
                    counter = self.counter
                    parts, count, mask = {}, 0, 0
                    break
                i, n = chunk >> 4, chunk & 0x0F
                if not n or i >= n:
                    continue
                count = n
                parts[i] = body
                self.stats["chunks"] += 1
                mask = sum(1 << j for j in range(count) if j not in parts)
                if not mask:
                    return b"".join(parts[j] for j in range(count))
                end = time.monotonic() + self.timeout     # more on the way
        raise DmripError(f"no reply from DMR ID {self.radio_id} after {self.tries} tries")

    # -- what a debug link client calls --
    def write(self, data):
        self.pending += self.transact(bytes(data))
        return len(data)

    def read(self, n=1):
        out = bytes(self.pending[:n])
        del self.pending[:n]
        return out

    @property
    def in_waiting(self):
        return len(self.pending)

    def reset_input_buffer(self):
        self.pending.clear()

    def flush(self):
        pass

    def close(self):
        self.sock.close()


def detour_items(steps):
    """A detour's items from (cmd, payload) pairs; cmd 0 is a wait whose payload is the milliseconds (int)"""
    out = bytearray()
    for cmd, p in steps:
        if cmd == 0:
            p = struct.pack("<H", int(p))
        out += bytes([cmd, len(p)]) + bytes(p)
    return bytes(out)


def detour_run(seconds, steps):
    """DETOUR RUN's payload: the deadline (1..300 s), then the items"""
    if not 1 <= seconds <= DETOUR_MAX_S:
        raise DmripError(f"a detour lasts 1..{DETOUR_MAX_S} s")
    return struct.pack("<BH", DETOUR_RUN, seconds) + detour_items(steps)


def decode_detour_status(r):
    """DETOUR STATUS's reply (after the status byte), with the radio core's last summary when present"""
    phase, left, index, failed, result, runs = struct.unpack_from("<BIBBBI", r)
    out = {"phase": DETOUR_PHASES[phase] if phase < len(DETOUR_PHASES) else phase, "left_ms": left, "items_done": index,
           "failed_item": failed, "result": result, "runs": runs}
    if len(r) >= 12 + 29:
        f, why, count, mode, rx_hz, ms, rx_ms, tx_ms, rxc, txc, txd, rssi = struct.unpack_from("<BBHBIIIIHHHh", r, 12)
        out["last"] = {"armed": bool(f & 1), "returning": bool(f & 2), "restored": bool(f & 4),
                       "why": {0: None, 1: "asked", 2: "backstop"}.get(why, why), "count": count, "mode": mode,
                       "rx_hz": rx_hz, "ms": ms, "rx_ms": rx_ms, "tx_ms": tx_ms, "rx_count": rxc, "tx_count": txc,
                       "tx_denied": txd, "rssi_max": rssi}
    return out
