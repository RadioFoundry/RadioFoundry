# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The hotspot's CW ID (MMDVM SEND_CWID, docs/spec/mmdvm-protocol.md 2.11) on a radio whose board keys the FM tone
(the DM-1701, hal_radio_t tx_tone_key): the radio leaves DMR, keys an FM carrier on the hotspot's TX frequency with
the tone keyed in Morse at the settings' speed (20 wpm by default), then the hotspot's DMR set-up is back.

Opt-in: radio_tx + bench_service (--allow-rf --allow-service-stop). The bench only records (sdrmon.py capture) and
tools/sdr_fm_check.py finds the keyed segment. The MMDVM client keeps DTR clear (see test_hotspot.py).

Pass criteria: SEND_CWID ACKed; the GET_STATUS TX flag set while keying; hs.cw_sent +1, hs.cw_wpm 20, hs.cw_last_ms
within 10 % of 150 + 60 x units + 150 ms; one keyed segment of about that length off air, with the Morse in it (at
least 8 runs of one audio tone, sdr_fm_check.py --tones: the 12 dashes of N0CALL); the hotspot answers
GET_STATUS again with the TX flag clear and DMR slot space."""
import struct
import time

import pytest

from odr_hil import skip
from test_fm_tx import parse_fm

GET_VERSION, GET_STATUS, SET_CONFIG, SET_MODE, SET_FREQ, SEND_CWID = 0x00, 0x01, 0x02, 0x03, 0x04, 0x0A
CALL = "N0CALL"
UNITS = 73          # N -. 5, 0 ----- 19, C -.-. 11, A .- 5, L .-.. 9, L 9, and 5 character gaps of 3


def morse_units(text):
    table = {"N": "-.", "0": "-----", "C": "-.-.", "A": ".-", "L": ".-.."}
    chars = [sum(3 if e == "-" else 1 for e in table[c]) + len(table[c]) - 1 for c in text]
    return sum(chars) + 3 * (len(chars) - 1)


def config(cc=1):
    return bytes([0x80, 0x02, 10, 2, 128, 128, cc, 0, 128, 0, 128, 0, 0, 128, 128] + [0] * 8)


def freq(rx, tx):
    return bytes([0]) + struct.pack("<II", rx, tx) + bytes([255]) + struct.pack("<I", tx)


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_hotspot_cwid(radio, sdr_bench, pytestconfig):
    feats = radio.features()
    if "HOTSPOT" not in feats:
        skip("not_built", "built without the hotspot")
    if radio.view(["dmr_ext"]).get("hs.cw_requests") is None:
        skip("not_built", "firmware without the CW ID keyer")
    import mmdvm_probe
    assert morse_units(CALL) == UNITS
    mhz = pytestconfig.getoption("--dmr-freq")
    hz = round(mhz * 1e6)
    radio.go_home()
    radio.find_channel(hz, "dmr")
    radio.lowest_power()
    x0 = radio.view(["dmr_ext"])
    expect_ms = 150 + 60 * UNITS + 150
    rdir = sdr_bench.run_dir("cwid")
    with sdr_bench.lease():
        cap = sdr_bench.capture_start(rdir, mhz, seconds=expect_ms / 1000 + 4)
        with radio.released() as port:
            link = mmdvm_probe.Link(port, dtr=False)
            try:
                assert link.request(GET_VERSION, timeout=2.0)[0] == "reply"
                assert link.request(SET_CONFIG, config())[0] == "ack"
                assert link.request(SET_FREQ, freq(hz, hz))[0] == "ack"
                assert link.request(SET_MODE, bytes([0]))[0] == "ack"
                kind, f = link.request(SEND_CWID, CALL.encode())
                assert kind == "ack", f
                time.sleep(0.5)
                kind, st = link.request(GET_STATUS)
                assert kind == "reply" and st[5] & 0x01, f"TX flag not set while keying: {st!r}"
                time.sleep(expect_ms / 1000 + 0.5)
                kind, st = link.request(GET_STATUS)
                assert kind == "reply" and not st[5] & 0x01, f"TX flag still set: {st!r}"
            finally:
                link.s.close()
        sdr_bench.capture_wait()
    x1 = radio.view(["dmr_ext"])
    radio.link.hotspot_exit()
    radio.wait_for(lambda v: v.get("hostlink") == "idle", 5, sections=("hotspot",), what="hotspot left")
    print({k: x1[k] for k in x1 if k.startswith("hs.cw")})
    assert int(x1["hs.cw_sent"]) == int(x0["hs.cw_sent"]) + 1 and x1["hs.cw_wpm"] == "20", x1
    assert abs(int(x1["hs.cw_last_ms"]) - expect_ms) <= expect_ms // 10, (x1["hs.cw_last_ms"], expect_ms)
    rc, text = sdr_bench.analyze_fm(cap, bw_khz=12.5, ppm=pytestconfig.getoption("--sdr-ppm"), freq=mhz, tones=True)
    print(text)
    r = parse_fm(text)
    assert r["segments"] == 1, f"keyed segments: {r['segments']} (~/{sdr_bench.dir}/{rdir}/)"
    # The Morse itself: the dashes (180 ms at 20 wpm; "N0CALL" has 12) as tone runs at one audio frequency
    tones = r["tones"]
    best = max((sum(1 for u in tones if abs(u[0] - t[0]) <= 25) for t in tones), default=0)
    assert best >= 8, f"the CW tone isn't on air: {best} tone runs at one frequency (tones {tones})"
