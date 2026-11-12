# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""DMR data as the bench modem hears it, for the HIL tests: data headers and their rate 1/2 blocks (ETSI TS 102 361-1
9.2.6, B.3.8), the IPv4 / UDP inside, and LRRP documents (docs/spec/dmr-data.md 7; the radio's side is
core/dmr/dmrLrrp.c). Pure functions: tests/hil/test_dmr_packets.py checks them without a radio."""

LRRP_PORT = 4001
LRRP_IMMEDIATE_REQUEST, LRRP_IMMEDIATE_RESPONSE, LRRP_TRIGGERED_REPORT = 0x05, 0x07, 0x0D
LRRP_RESULT_NO_FIX = 0x10


def crc16_ccitt(data, mask):
    crc = 0
    for b in data[:10]:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return (~crc ^ ((mask << 8) | mask)) & 0xFFFF


def crc32_dmr(data):
    """ETSI TS 102 361-1 B.3.8: the octets of each 16 bit word swapped, polynomial 0x04C11DB7, initial 0."""
    crc = 0
    n = len(data) + (len(data) & 1)
    for i in range(n):
        j = i ^ 1
        crc ^= (data[j] if j < len(data) else 0) << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def data_packets(lines):
    """Data headers heard (dt 6) with the rate 1/2 blocks (dt 7) that follow: dicts with the header fields."""
    out, cur = [], None
    for r in lines:
        dt, info = r.get("dt"), r.get("info")
        if not info:
            continue
        b = bytes.fromhex(info)
        if dt == 6:
            dpf = b[0] & 0x0F
            n = (b[8] & 0x7F) if dpf == 2 else (b[8] & 0x03) + 1 if dpf == 0 else 0
            cur = {"dpf": dpf, "sap": b[1] >> 4, "dst": int.from_bytes(b[2:5], "big"),
                   "src": int.from_bytes(b[5:8], "big"), "pad": (b[0] & 0x10) | (b[1] & 0x0F), "fmt": b[1] & 0x0F,
                   "crc_ok": crc16_ccitt(b, 0xCC) == int.from_bytes(b[10:12], "big"), "need": n, "blocks": []}
            out.append(cur)
        elif dt == 7 and cur is not None and len(cur["blocks"]) < cur["need"]:
            cur["blocks"].append(b)
    return out


def packet_user(p):
    """The user data of an unconfirmed packet (data_packets()) with every block heard: the packet CRC-32 checked, the
    pad octets dropped."""
    assert p["dpf"] == 2, f"not unconfirmed data (DPF {p['dpf']}): {p}"
    assert len(p["blocks"]) == p["need"], f"{len(p['blocks'])} of {p['need']} blocks heard: {p}"
    user = b"".join(p["blocks"])
    body, crc = user[:-4], int.from_bytes(user[-4:], "little")
    assert crc32_dmr(body) == crc, "packet CRC-32"
    return body[:len(body) - p["pad"]] if p["pad"] else body


def udp_payload(user, port):
    """The payload of IPv4 / UDP to `port` in a packet's user data; asserts the layers."""
    assert user[0] >> 4 == 4 and user[9] == 17, f"not IPv4 / UDP: {user.hex()}"
    udp = user[(user[0] & 0x0F) * 4:]
    assert int.from_bytes(udp[2:4], "big") == port, f"UDP port {int.from_bytes(udp[2:4], 'big')}, not {port}"
    return udp[8:]


def lrrp_immediate_request(request_id):
    """An LRRP immediate location request (document 0x05) with this request ID (bytes), asking for the position with
    its accuracy (token 0x51)."""
    tokens = bytes([0x22, len(request_id)]) + bytes(request_id) + b"\x51"
    return bytes([LRRP_IMMEDIATE_REQUEST, len(tokens)]) + tokens


def _uintvar(d, i):
    v = 0
    while True:
        v = (v << 7) | (d[i] & 0x7F)
        i += 1
        if not d[i - 1] & 0x80:
            return v, i


def lrrp_answer(doc):
    """An LRRP answer document: {"doc", "request_id", "result", "lat", "lon"}. result None when the document carries
    no result token (success); lat / lon None without a position token."""
    out = {"doc": doc[0], "request_id": None, "result": None, "lat": None, "lon": None}
    length, i = _uintvar(doc, 1)
    end = min(len(doc), i + length)
    while i < end:
        t = doc[i]
        if t == 0x22:
            n, j = _uintvar(doc, i + 1)
            out["request_id"], i = bytes(doc[j:j + n]), j + n
        elif t == 0x23:
            out["request_id"], i = bytes(doc[i + 1:i + 2]), i + 2
        elif t == 0x24:
            v, i = _uintvar(doc, i + 1)
            out["request_id"] = v
        elif t == 0x37:
            out["result"], i = _uintvar(doc, i + 1)
        elif t == 0x38:
            out["result"], i = 0, i + 1
        elif t == 0x34:
            i += 6
        elif t == 0x51:
            la, lo = int.from_bytes(doc[i + 1:i + 5], "big"), int.from_bytes(doc[i + 5:i + 9], "big", signed=True)
            lat = (la & 0x7FFFFFFF) * 90.0 / 2 ** 31
            out["lat"], out["lon"] = (-lat if la & 0x80000000 else lat), lo * 360.0 / 2 ** 32
            break       # the radius and what follows aren't needed
        else:
            break
    return out


def lrrp_position(user):
    """(lat, lon) of an LRRP triggered location report in IPv4 / UDP to port 4001; asserts the layers."""
    doc = udp_payload(user, LRRP_PORT)
    assert doc[0] == LRRP_TRIGGERED_REPORT, f"LRRP document {doc[0]:#x} (0x0D: triggered location report): {doc.hex()}"
    a = lrrp_answer(doc)
    assert a["lat"] is not None, f"no position token in the LRRP report: {doc.hex()}"
    return a["lat"], a["lon"]


def udt_position(block):
    bits = int.from_bytes(block[:10], "big")

    def f(pos, width):
        return (bits >> (80 - pos - width)) & ((1 << width) - 1)
    north, east, valid = f(1, 1), f(2, 1), f(3, 1)
    lat = f(11, 7) + (f(18, 6) + f(24, 14) / 10000.0) / 60.0
    lon = f(38, 8) + (f(46, 6) + f(52, 14) / 10000.0) / 60.0
    return valid, (lat if north else -lat), (lon if east else -lon)
