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
