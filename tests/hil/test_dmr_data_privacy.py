# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Packet data under a privacy key (docs/spec/dmr-data.md 13): what the radio sends, holds back and takes in.

test_data_privacy_tx: a Motorola basic privacy key (1) loaded over the debug link, compressed headers off, a text
message to the bench: the packet carries the privacy header (the user data's SAP 4, algorithm 0, encrypted) after its
first header, and its user data is the IPv4 / UDP 4007 text message XORed with the key's 16-bit word. The test
recovers the word from the known start of the IPv4 header (0x45 0x00), checks it is key 1's, and reads the message
back with it. data.privacy_tx counts the packet. The bench modem, overloaded by the radio next to it, misses bursts:
the message is sent again, up to 3 times, until a whole packet is heard.

test_data_privacy_blocked: an Anytone key (no data form): the text message fails at once, data.privacy_blocked counts
it, and no data header from us goes on air.

test_data_privacy_rx: the bench sends encrypted packets the test builds (tools/bench/dmr_burst.py) as raw DMR bursts
(a bench_tx raw job). With the key the text message lands in the inbox (data.privacy_rx); with SAP 9 in the first
header too (the privacy header names the user data's SAP). Without a key, or with the AES algorithm the key's scheme
doesn't do, the packet is heard (data.rx_packets) and dropped (data.privacy_rx_dropped); with another key it is
decrypted to nonsense that never reaches the inbox."""
import time

import pytest

import dmr_burst as B
import dmr_packets as D
from rfoundry_hil import HIL_SMS_PREFIX, skip
from test_dmr_data import BENCH_SRC, _deltas, _need, _radio_sends, _setup, heard_headers
from test_dmr_ip import ip_settings  # noqa: F401 (a fixture)

# Key 1's 16-bit word (the key table in core/dmr/dmrPrivacy.c); test_data_privacy_tx recovers it off air too
BP_KEY1_WORD = 0x1F00
ALG_BP, ALG_AES128 = 0, 4


def _privacy_off_at_start(radio):
    if radio.view(["dmr"]).get("dmr.privacy") == "1":
        skip("state", "a privacy key is set already: not overwriting it")


def _key(radio, kind, key):
    radio.link.dmr_privacy(kind, key)
    want = "0" if kind == "off" else "1"
    radio.wait_for(lambda x: x.get("dmr.privacy") == want, sections=("dmr",), what=f"privacy {kind} {key}")


@pytest.mark.feature("dmr_data_privacy")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_data_privacy_tx(radio, sdr_bench, pytestconfig, ip_settings):  # noqa: F811
    freq, cc, own, feats = _setup(radio, pytestconfig, tx=True)
    _need(feats, "DMR_SMS")
    _need(feats, "DMR_PRIVACY")
    _privacy_off_at_start(radio)
    # SAP 4, the full IPv4 header (its first octets are known), and one unconfirmed packet
    radio.link.ip_settings("off", "off", 0, ())
    text = f"{HIL_SMS_PREFIX}P{int(time.time()) % 10000}"
    got = None
    with radio.debuglink_privacy():
        _key(radio, "moto", 1)
        for attempt in range(1, 4):
            before = radio.data_view()
            heard, after = _radio_sends(radio, sdr_bench, freq, cc, lambda: radio.link.sms(BENCH_SRC, text, ack=False),
                                        10, lambda v: v.get("data.sms_state") in ("ok", "failed"))
            after = radio.data_view()
            d = _deltas(before, after, ["data.sms_sent", "data.privacy_tx", "data.privacy_blocked", "data.tx_ok"])
            print("radio deltas:", d)
            assert after["data.sms_state"] == "ok" and d["data.sms_sent"] == 1, (after, d)
            # every list the radio sent here was encrypted (an ARS retry is a packet too)
            assert d["data.privacy_tx"] == d["data.tx_ok"] >= 1 and d["data.privacy_blocked"] == 0, d
            ours = [p for p in D.data_packets(heard) if p["src"] == own and p["dst"] == BENCH_SRC and p["crc_ok"]]
            for p in ours:
                assert "privacy" in p, f"a data packet from us without the privacy header: {p}"
                ph = p["privacy"]
                assert ph["sap"] == 4, ph
                assert ph["alg"] == ALG_BP and ph["encrypted"] and ph["key_id"] == 0 and ph["mi"] == 0, ph
                assert p["sap"] == 4, f"the first header's SAP: {p}"
            whole = [p for p in ours if p["privacy"]["crc_ok"] and len(p["blocks"]) == p["need"]]
            if whole:
                got = whole[0]
                break
            print(f"send {attempt}: the bench heard {len(ours)} of our headers, no whole packet")
        _key(radio, "off", 0)
    assert got is not None, "the bench heard no whole packet from us in 3 sends"
    user = D.packet_user(got)
    word = ((user[0] ^ 0x45) << 8) | (user[1] ^ 0x00)
    assert word == BP_KEY1_WORD, f"keystream word {word:04x} (the IPv4 header's first octets), not key 1's"
    clear = B.bp_xor(word, user)
    assert B._checksum(clear[:20]) == 0, f"IPv4 header checksum after decryption: {clear[:20].hex()}"
    payload = D.udp_payload(clear, B.TMS_PORT)
    assert payload.endswith(text.encode("utf-16-le")), f"TMS payload {payload.hex()}"
    print(f"privacy header {got['privacy']}, key word {word:04x}, text {text!r} read back")


@pytest.mark.feature("dmr_data_privacy")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_data_privacy_blocked(radio, sdr_bench, pytestconfig):
    freq, cc, own, feats = _setup(radio, pytestconfig, tx=True)
    _need(feats, "DMR_SMS")
    _need(feats, "DMR_PRIVACY")
    _privacy_off_at_start(radio)
    with radio.debuglink_privacy():
        _key(radio, "anytone", 0x1234)
        before = radio.data_view()
        heard, _ = _radio_sends(radio, sdr_bench, freq, cc,
                                lambda: radio.link.sms(BENCH_SRC, "RadioFoundry HIL blocked", ack=False), 6,
                                lambda v: v.get("data.sms_state") in ("ok", "failed"))
        after = radio.data_view()
        _key(radio, "off", 0)
    d = _deltas(before, after, ["data.sms_sent", "data.privacy_blocked", "data.privacy_tx"])
    print("radio deltas:", d)
    assert after["data.sms_state"] == "failed", after
    assert d["data.privacy_blocked"] >= 1 and d["data.sms_sent"] == 0 and d["data.privacy_tx"] == 0, d
    assert not heard_headers(heard, own), "a data header from us went on air under a key with no data form"


# (radio key, packet privacy header or None, the first header's SAP, the inbox gets it)
RX_CASES = {
    "key": (("moto", 1), {"alg": ALG_BP, "word": BP_KEY1_WORD}, 4, True),
    "sap9": (("moto", 1), {"alg": ALG_BP, "word": BP_KEY1_WORD}, 9, True),
    "no_key": (("off", 0), {"alg": ALG_BP, "word": BP_KEY1_WORD}, 4, False),
    "other_key": (("moto", 2), {"alg": ALG_BP, "word": BP_KEY1_WORD}, 4, False),
    "aes": (("moto", 1), {"alg": ALG_AES128, "word": BP_KEY1_WORD}, 4, False),
}


@pytest.mark.feature("dmr_data_privacy")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.parametrize("case", list(RX_CASES))
def test_data_privacy_rx(radio, sdr_bench, pytestconfig, case):
    freq, cc, own, feats = _setup(radio, pytestconfig)
    _need(feats, "DMR_SMS")
    _need(feats, "DMR_PRIVACY")
    _privacy_off_at_start(radio)
    key, privacy, first_sap, delivered = RX_CASES[case]
    stamp = int(time.time()) % 10000
    keys = ["data.rx_packets", "data.sms_received", "data.privacy_rx", "data.privacy_rx_dropped"]
    with radio.debuglink_privacy():
        _key(radio, *key)
        for attempt in range(1, 4):
            text = f"{HIL_SMS_PREFIX}R{stamp}{attempt}"
            user = B.udp_ipv4(BENCH_SRC, own, B.TMS_PORT, B.TMS_PORT, B.tms_text(text, seq=attempt), ident=stamp)
            bursts = B.data_bursts(own, BENCH_SRC, 4, user, cc=cc, preambles=2, privacy=privacy, first_sap=first_sap)
            before = radio.data_view()
            with sdr_bench.lease():
                sdr_bench.dmr_bursts(freq, cc, bursts)
            time.sleep(0.8)
            after = radio.data_view()
            d = _deltas(before, after, keys)
            print(f"{case} send {attempt}: radio deltas {d}")
            if d["data.rx_packets"] >= 1:
                break
        _key(radio, "off", 0)
    assert d["data.rx_packets"] == 1, f"the radio heard none of 3 sends: {d}"
    if delivered:
        assert d["data.privacy_rx"] == 1 and d["data.sms_received"] == 1 and d["data.privacy_rx_dropped"] == 0, d
        m = radio.link.inbox(0)
        assert m is not None and m["text"] == text and m["src"] == BENCH_SRC, m
        radio.link.mark_read(0)
    elif case == "other_key":
        # decrypted with the wrong word: nonsense, which no layer above takes
        assert d["data.privacy_rx"] == 1 and d["data.sms_received"] == 0, d
    else:
        assert d["data.privacy_rx_dropped"] == 1 and d["data.privacy_rx"] == 0 and d["data.sms_received"] == 0, d
