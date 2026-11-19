# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The DMR data decoders the on-air tests use (dmr_packets.py). No radio: these run anywhere, and tools/ci.sh runs
them."""
import pytest

import dmr_packets as D

LAT, LON = 33.7490, -84.3880
ID = b"\x00\x00\x00\x2a"


def coords():
    lat = round(abs(LAT) / 90 * 2 ** 31) | (0x80000000 if LAT < 0 else 0)
    return lat.to_bytes(4, "big") + round(LON / 360 * 2 ** 32).to_bytes(4, "big", signed=True)


def doc(doc_id, tokens):
    return bytes([doc_id, len(tokens)]) + tokens


def ipv4_udp(port, payload):
    udp = (4001).to_bytes(2, "big") + port.to_bytes(2, "big") + (8 + len(payload)).to_bytes(2, "big") + b"\0\0"
    ip = bytes([0x45, 0]) + (20 + len(udp) + len(payload)).to_bytes(2, "big") + bytes(5) + bytes([17]) + bytes(10)
    return ip + udp + payload


def test_immediate_request_layout():
    # The layout core/dmr/dmrLrrp.c decodes (tests/host/lrrpTest.c): document 0x05, request ID 22 <n> <id>, then 0x51
    assert D.lrrp_immediate_request(ID) == bytes([0x05, 0x07, 0x22, 0x04]) + ID + b"\x51"


def test_immediate_answer_with_a_position():
    a = D.lrrp_answer(doc(D.LRRP_IMMEDIATE_RESPONSE, b"\x22\x04" + ID + b"\x51" + coords() + b"\x81\x48"))
    assert a["doc"] == D.LRRP_IMMEDIATE_RESPONSE and a["request_id"] == ID and a["result"] is None
    assert a["lat"] == pytest.approx(LAT, abs=1e-6) and a["lon"] == pytest.approx(LON, abs=1e-6)


def test_immediate_answer_without_a_fix():
    a = D.lrrp_answer(doc(D.LRRP_IMMEDIATE_RESPONSE, b"\x22\x04" + ID + b"\x37\x10"))
    assert a["request_id"] == ID and a["result"] == D.LRRP_RESULT_NO_FIX and a["lat"] is None


def test_info_time_is_skipped():
    a = D.lrrp_answer(doc(D.LRRP_IMMEDIATE_RESPONSE, b"\x22\x04" + ID + b"\x34" + bytes(5) + b"\x51" + coords()))
    assert a["lat"] == pytest.approx(LAT, abs=1e-6)


def test_triggered_report_in_udp():
    user = ipv4_udp(D.LRRP_PORT, doc(D.LRRP_TRIGGERED_REPORT, b"\x22\x01\x09\x51" + coords()))
    lat, lon = D.lrrp_position(user)
    assert lat == pytest.approx(LAT, abs=1e-6) and lon == pytest.approx(LON, abs=1e-6)
    with pytest.raises(AssertionError):
        D.udp_payload(user, 4007)


def test_packet_user_checks_the_crc():
    body = ipv4_udp(D.LRRP_PORT, b"x" * 9)
    pad = -(len(body) + 4) % 12
    user = body + bytes(pad)
    blocks = user + D.crc32_dmr(user).to_bytes(4, "little")
    p = {"dpf": 2, "pad": pad, "need": len(blocks) // 12, "blocks": [blocks[i:i + 12] for i in range(0, len(blocks), 12)]}
    assert D.packet_user(p) == body
    p["blocks"][0] = b"\xff" + p["blocks"][0][1:]
    with pytest.raises(AssertionError):
        D.packet_user(p)


# --- The bursts the privacy tests send (tools/bench/dmr_burst.py) ---

# An unconfirmed packet (2 preambles, SAP 4, 30 octets i*7+1, from 9990301 to 3101276, CC 1) with the privacy header,
# its user data XORed with 0x1F00: the bursts core/dmr/dmrData.c's dmrDataBuildPacket and
# core/hotspot/dmrDataFrame.c's dmrDataBurstToFrame make (BPTC, slot type, MS data sync)
PRIVACY_PACKET = [
    "48590455b9f418fc6cf80b9304cd5d7f77fd757ac9725948d6f85c4137ded89620",
    "483d04d5b8ec18c46899033104cd5d7f77fd757ac9ba79e8d6385c813f5ec21624",
    "794318be54c64a9b01b20d26c58d5d7f77fd7573381c9c60b176c54405409134ca",
    "4012004b0036018d03080020058d5d7f77fd757338b1417c825101e407980f9019",
    "54f104e601b2348c25d8e5fa85ed5d7f77fd7570967c3499b0832284240e160060",
    "28b9e93e747e70b86abc686fc5ed5d7f77fd75709695c0f5a5f656e411829a3ce8",
    "3c2ab8f2818ac10ccba00b3205ed5d7f77fd7570956fc42244eb842c155e6328dc",
]


def _burst():
    import os
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "bench"))
    import dmr_burst
    return dmr_burst


def test_bursts_match_the_firmware():
    B = _burst()
    user = bytes((i * 7 + 1) & 0xFF for i in range(30))
    got = B.data_bursts(3101276, 9990301, 4, user, cc=1, preambles=2, privacy={"alg": 0, "word": 0x1F00})
    assert [b.hex() for b in got] == PRIVACY_PACKET


def _heard(bursts):
    """The listener's records for bursts: data type and the 12 info octets (BPTC decoded, as the bench gives them)"""
    B = _burst()
    out = []
    for b in bursts:
        bits = [(b[i // 8] >> (7 - i % 8)) & 1 for i in range(264)]
        st = int("".join(map(str, bits[98:108] + bits[156:166])), 2)
        info = bits[:98] + bits[166:]
        m = [0] * 196
        for k in range(196):
            m[k] = info[(k * 181) % 196]
        pay = [m[1 + 3 + n] if n < 8 else m[1 + 15 * (1 + (n - 8) // 11) + (n - 8) % 11] for n in range(96)]
        out.append({"dt": (st >> 12) & 0x0F, "info": bytes(int("".join(map(str, pay[i:i + 8])), 2)
                                                             for i in range(0, 96, 8)).hex()})
        assert B.golay2087(st >> 12) == st
    return out


def test_privacy_header_is_one_of_the_blocks():
    B = _burst()
    text = "hi"
    user = B.udp_ipv4(9990301, 3101276, B.TMS_PORT, B.TMS_PORT, B.tms_text(text), ident=7)
    bursts = B.data_bursts(3101276, 9990301, 4, user, cc=1, privacy={"alg": 0, "word": 0x1F00}, first_sap=9)
    (p,) = D.data_packets(_heard(bursts))
    assert p["sap"] == 9 and p["crc_ok"] and p["privacy"] == {"sap": 4, "alg": 0, "encrypted": True, "key_id": 0,
                                                                "mi": 0, "crc_ok": True}
    clear = B.bp_xor(0x1F00, D.packet_user(p))
    assert clear == user and B._checksum(clear[:20]) == 0
    assert D.udp_payload(clear, B.TMS_PORT).endswith(text.encode("utf-16-le"))


def test_network_header_is_not_a_privacy_header():
    B = _burst()
    bursts = B.data_bursts(1, 2, 4, bytes(20), privacy={"alg": 0})
    heard = _heard(bursts)
    h = bytearray(bytes.fromhex(heard[3]["info"]))
    h[0] = (1 << 4) | 15                       # SAP 1: a network header
    h[10:12] = B.crc16(h, 0xCC).to_bytes(2, "big")
    heard[3]["info"] = bytes(h).hex()
    assert "privacy" not in D.data_packets(heard)[0]
