#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
DMR packet data from what the bench modem heard: reassembles the packets in `bench_tx.py listen --json` lines (data
headers and rate 1/2 blocks, their "info" octets) and decodes the IP in them, the compressed UDP/IPv4 header (SAP 3)
included. Written from ETSI TS 102 361-1 (8.2, 9.2.1, B.3.8-B.3.10) and TS 102 361-3 (5.6, 7.2), apart from the
firmware's code, so the HIL tests (tests/hil/test_dmr_ip.py) check the radio against an independent reading.

    dmr_packet.py listen.jsonl          # one line per packet heard
    python3 -m ... / import dmr_packet: packets(lines) -> list of dicts

Only rate 1/2 blocks: the listener reports the info octets of those (bench_tx.py); a packet at rate 3/4 or 1 shows as
incomplete. Pure Python, no rfoundry_sdrbench package: runs on the test host.
"""
import json
import sys

DT_DATA_HEADER, DT_RATE_12 = 6, 7
DPF_UDT, DPF_RESPONSE, DPF_UNCONFIRMED, DPF_CONFIRMED = 0, 1, 2, 3
SAP_UDPIP_COMPRESSION, SAP_IP, SAP_SHORT_DATA = 3, 4, 10
CRC9_MASK_RATE_12 = 0x0F0
UDPC_PORTS = {1: 5016, 2: 5017, 98: 4007}     # port IDs (TS 102 361-3 Tables 7.17 / 7.18; 98: MOTOTRBO TMS)


def _polymod(m, g, deg):
    while m.bit_length() > deg:
        m ^= g << (m.bit_length() - 1 - deg)
    return m


def crc_ccitt_ok(info12, mask=0xCCCC):
    """B.3.8: the header's CRC-CCITT (inverted, masked) in octets 10-11."""
    r = _polymod(int.from_bytes(bytes(info12[:10]), "big") << 16, 0x11021, 16) ^ 0xFFFF ^ mask
    return r == int.from_bytes(bytes(info12[10:12]), "big")


def crc32(data):
    """B.3.9: octets in pairs, least significant of each 16-bit word first, zero initial remainder."""
    d = bytearray(data)
    if len(d) % 2:
        d.append(0)
    swapped = bytes(b for i in range(0, len(d), 2) for b in (d[i + 1], d[i]))
    return _polymod(int.from_bytes(swapped, "big") << 32 if swapped else 0, 0x104C11DB7, 32)


def crc9(data, serial):
    """B.3.10: the block's data octets then the 7 serial number bits, inverted (not masked)."""
    bits = int.from_bytes(bytes(data), "big") << 7 | (serial & 0x7F)
    return _polymod(bits << 9, 0x259, 9) ^ 0x1FF


def parse_header(h):
    """The fields of a data header (12 octets) this decoder uses."""
    dpf = h[0] & 0x0F
    out = {"dpf": dpf, "group": bool(h[0] & 0x80), "response_requested": bool(h[0] & 0x40), "sap": h[1] >> 4,
           "dst": int.from_bytes(h[2:5], "big"), "src": int.from_bytes(h[5:8], "big")}
    if dpf in (DPF_UNCONFIRMED, DPF_CONFIRMED):
        out.update(blocks=h[8] & 0x7F, pad=((h[0] & 0x10) | (h[1] & 0x0F)), first_try=bool(h[8] & 0x80),
                   ns=(h[9] >> 4) & 7, fsn=h[9] & 0x0F)
    elif dpf == DPF_RESPONSE:
        out.update(blocks=h[8] & 0x7F, pad=0, response=h[9], cls=h[9] >> 6, type=(h[9] >> 3) & 7, status=h[9] & 7)
    else:
        out.update(blocks=0, pad=0)
    return out


def decode_udp(sap, user):
    """(source port, destination port, payload, header octets) of the UDP in a packet's user data, or None.
    SAP 3: the compressed header (Table 7.14: identification, SAID / DAID, opcode with the port IDs, extended headers);
    SAP 4: IPv4 without options or with, UDP only."""
    if sap == SAP_UDPIP_COMPRESSION:
        if len(user) < 5 or (user[3] | user[4]) & 0x80:
            return None
        pos = 5
        ports = []
        for pid in (user[3] & 0x7F, user[4] & 0x7F):
            if pid == 0:
                ports.append(int.from_bytes(user[pos:pos + 2], "big"))
                pos += 2
            else:
                ports.append(UDPC_PORTS.get(pid, 0x8000 | pid))
        return ports[0], ports[1], bytes(user[pos:]), pos
    if sap == SAP_IP and len(user) >= 28 and user[0] >> 4 == 4 and user[9] == 17:
        ihl = (user[0] & 0x0F) * 4
        udp_len = int.from_bytes(user[ihl + 4:ihl + 6], "big")
        return (int.from_bytes(user[ihl:ihl + 2], "big"), int.from_bytes(user[ihl + 2:ihl + 4], "big"),
                bytes(user[ihl + 8:ihl + udp_len]), ihl + 8)
    return None


def _finish(p, blocks):
    """Reassemble one packet from its rate 1/2 block infos (in the order heard)."""
    p["heard_blocks"] = len(blocks)
    if p["dpf"] == DPF_CONFIRMED:
        placed, bad = {}, []
        for b in blocks:
            serial, rx = b[0] >> 1, ((b[0] & 1) << 8) | b[1]
            if crc9(b[2:12], serial) ^ CRC9_MASK_RATE_12 == rx:
                placed[serial] = b[2:12]
            else:
                bad.append(serial)
        p["serials"] = sorted(placed)
        p["bad_blocks"] = bad
        if not p["first_try"] or len(placed) != p["blocks"] or sorted(placed) != list(range(p["blocks"])):
            p["complete"] = False
            return p
        data = b"".join(placed[k] for k in range(p["blocks"]))
    else:
        if len(blocks) != p["blocks"]:
            p["complete"] = False
            return p
        data = b"".join(bytes(b) for b in blocks)
    p["complete"] = True
    body, crc = data[:-4], int.from_bytes(data[-4:], "little")
    p["crc_ok"] = crc32(body) == crc
    user = body[:len(body) - p["pad"]] if p["pad"] else body
    p["user"] = user.hex()
    if p["dpf"] == DPF_RESPONSE:
        p["flags"] = user[:8].hex()
    u = decode_udp(p["sap"], user) if p["crc_ok"] else None
    if u is not None:
        p.update(src_port=u[0], dst_port=u[1], payload=u[2].hex(), header_octets=u[3])
    return p


def packets(lines):
    """Every packet in the listen JSON records (dicts, in order): a data header with a good CRC starts one, the rate
    1/2 blocks after it fill it. Returns dicts: the header fields, "complete", "crc_ok", "user" (hex), and for UDP
    "src_port dst_port payload header_octets"; confirmed ones also "serials" (blocks with a good CRC-9) and
    "bad_blocks"."""
    out, cur, blocks = [], None, []
    for r in lines:
        dt, info = r.get("dt"), r.get("info")
        if dt == DT_DATA_HEADER and info:
            h = bytes.fromhex(info)
            if not crc_ccitt_ok(h):
                continue
            if cur is not None:
                out.append(_finish(cur, blocks))
            cur, blocks = parse_header(h), []
            cur["t"] = r.get("t")
            if cur["blocks"] == 0:
                cur.update(complete=True, crc_ok=True, heard_blocks=0)
                out.append(cur)
                cur = None
        elif dt == DT_RATE_12 and info and cur is not None:
            blocks.append(bytes.fromhex(info))
            if len(blocks) == cur["blocks"]:
                out.append(_finish(cur, blocks))
                cur, blocks = None, []
    if cur is not None:
        out.append(_finish(cur, blocks))
    return out


def _self_test():
    # The firmware test's vectors (tests/host/dmr_ip_vectors.py): a confirmed SAP 3 packet and its decode
    hdr = bytes.fromhex("433800006400271582581c55")
    blocks = [bytes.fromhex("01ef00010001014f44520000"), bytes.fromhex("03fb000000000000a4625e72")]
    lines = [{"dt": 6, "info": hdr.hex()}] + [{"dt": 7, "info": b.hex()} for b in blocks]
    p, = packets(lines)
    assert p["complete"] and p["crc_ok"] and p["dpf"] == DPF_CONFIRMED and p["ns"] == 5, p
    assert p["sap"] == SAP_UDPIP_COMPRESSION and p["src_port"] == 5016 and p["dst_port"] == 5016, p
    assert bytes.fromhex(p["payload"]) == bytes.fromhex("4f4452") and p["header_octets"] == 5, p
    assert decode_udp(3, bytes.fromhex("1234000000c35000a1") + b"x") == (50000, 161, b"x", 9)
    print("dmr_packet: self test passed")


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--self-test":
        _self_test()
        return 0
    src = open(sys.argv[1]) if len(sys.argv) > 1 else sys.stdin
    lines = []
    for line in src:
        line = line.strip()
        if line.startswith("{"):
            try:
                lines.append(json.loads(line))
            except ValueError:
                pass
    for p in packets(lines):
        print(json.dumps(p))
    return 0


if __name__ == "__main__":
    sys.exit(main())
