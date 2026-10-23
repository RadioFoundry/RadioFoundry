# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""DMR receive from the SDR bench's transmitter (opt-in RF: --allow-rf --allow-service-stop). The bench sends two group
calls of 10
superframes each (8 voice LC headers, 60 voice bursts and 3 terminators per call) on the DMR test channel; the radio's
counters must show exactly that.

Tolerances (documented in README.md): voice bursts 120 - 0 / + 0, headers 14..16 (the first header of a call can
fall in the receiver's acquisition), terminators 5..6, CRC errors 0, two calls seen by the call stack."""
import time

import pytest

from odr_hil import counter_deltas, skip

CALLS, SUPERFRAMES, HEADERS, TERMINATORS = 2, 10, 8, 3


@pytest.mark.feature("dmr_voice")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_dmr_rx_counters(radio, sdr_bench, pytestconfig):
    if "DMR" not in radio.features():
        skip("not_built", "built without DMR")
    freq, cc, tg = (pytestconfig.getoption(o) for o in ("--dmr-freq", "--dmr-cc", "--dmr-tg"))
    radio.go_home()
    radio.find_channel(round(freq * 1e6), "dmr")
    time.sleep(1.0)                      # the call stack settles on the channel
    radio.reset_counters()
    _, before = radio.counters()
    with sdr_bench.lease():
        recs = sdr_bench.voice_call(freq, cc, tg, superframes=SUPERFRAMES, calls=CALLS, headers=HEADERS,
                               terminators=TERMINATORS, gap_ms=500)
    time.sleep(1.5)                      # the last call times out / ends in the radio
    _, after = radio.counters()
    d = counter_deltas(before, after, ["modem.voice", "modem.headers", "modem.terminators", "modem.crc",
                                       "call.rx_calls", "call.rx_dup_dropped"])
    print("bench:", recs[-1:] if recs else "no JSON", "radio deltas:", d)
    assert d["modem.voice"] == CALLS * SUPERFRAMES * 6, d
    assert CALLS * HEADERS - 2 <= d["modem.headers"] <= CALLS * HEADERS, d
    assert CALLS * TERMINATORS - 1 <= d["modem.terminators"] <= CALLS * TERMINATORS, d
    assert d["modem.crc"] == 0, d
    assert d["call.rx_calls"] == CALLS, d
