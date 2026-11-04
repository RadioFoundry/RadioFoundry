# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Air band AM receive from a bench board's AM transmitter (opt-in RF: --allow-rf, a bench board that sends AM,
--am-mhz). The bench sends a full-carrier AM signal (the line protocol's tx op, mode am: a 1 kHz tone at depth 0.3 on a
bare carrier lead and tail) on --am-mhz; the radio's VFO is tuned there and received in AM (docs/spec/airband-and-
broadcast.md). The view shows AM receive by the state (rx when the squelch is open, idle when closed) and by
rssi_dbm, which the squelch works on (there is no noise reading in AM).

test_am_rx_squelch: the carrier opens the squelch (state rx, or rssi_dbm 10 dB over the floor read just before) and
the squelch closes after the carrier ends. The radio never transmits.

The bench transmits on --am-mhz: a cabled setup unless the frequency is approved for the air. The speaker check with
--microphone is test_am_rx_speaker in test_speaker.py."""
import time

import pytest

from rfoundry_hil import _LineTx, skip
from hil_config import rig

RISE_DB = 10.0              # rssi_dbm over the floor that counts as the carrier when the state shows no squelch
LEAD_S = 0.5                # bare carrier before and after the tone: the squelch opens and closes on it
DEPTH = 0.3
PROGRAM = [{"kind": "tone", "hz": 1000, "s": 3.0, "level": 1.0}]


def am_tx_start(bench, freq, program, max_s=10.0, **fields):
    """AM from the bench board on freq (MHz) in the background, like SdrBench.fm_tx_start (the tx op, mode am): a
    _LineTx once the board keyed; wait() gives its done record (depth, carrier_db, cut ...). A board that only sends
    frequency refuses AM: a skip (bench). Needs the lease and --allow-rf."""
    if not bench.allow_rf:
        raise RuntimeError("the bench board's AM transmitter keys up: needs --allow-rf")
    if not bench.api:
        skip("bench", "AM needs a bench board (--bench-api): the SDR bench host's modem is DMR only")
    bench._tx_check(freq)
    bench._need_lease()
    modes = (bench._api_info().get("signal_modes") or {}).get("tx", [])
    if "am" not in modes:
        skip("bench", f"the bench board {bench.api} doesn't transmit AM (its signal modes: {modes or 'none'})")
    L = bench._line()
    bench._api_claim(freq)
    req = {"op": "tx", "mode": "am", "freq": freq, "session": bench._api_session, "max_s": max_s, "program": program,
           "wait_max": bench.dmr_bench.WAIT_MAX, "label": getattr(bench.bench, "label", None) or "am"}
    if bench.api_level is not None:
        req["power"] = bench.api_level
    req.update({k: v for k, v in fields.items() if v is not None})
    t = _LineTx(L, L.address(bench.api), req)
    bench.bg.append(t)
    end = time.monotonic() + bench.dmr_bench.WAIT_MAX + 30
    while not t.started.wait(0.2) and not t.req.done.is_set() and time.monotonic() < end:
        pass
    e = t.req.error
    if e is not None:
        if "only sends frequency" in str(e):
            skip("bench", f"the bench board can't send AM: {e}")
        bench._api_fail(e, f"AM on {freq} MHz")
    if not t.started.is_set():
        t.terminate()
        raise RuntimeError(f"the bench board never keyed AM on {freq} MHz: {t.req.replies[-3:]}")
    return t


def am_vfo_ready(radio, pytestconfig):
    """(MHz, Hz) of --am-mhz after the feature and band checks; skips otherwise. The VFO is not tuned yet."""
    if "AM_RX" not in radio.features():
        skip("not_built", "built without CONFIG_AM_RX (no AM receiver on this radio, or switched off)")
    if "VFO_FM" not in radio.features():
        skip("not_built", "built without the FM VFO")
    mhz = rig(pytestconfig, "am-mhz", "the AM test frequency the bench board sends on:")
    if not 108.0 <= mhz < 137.0:
        skip("opt_in", f"{mhz} MHz is outside the AM receive band (108-137 MHz)")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    return mhz, round(mhz * 1e6)


def tune_am(radio, hz):
    radio.link.vfo_set(hz)
    v = radio.wait_for(lambda x: x.get("rx_hz") == str(hz), 3.0, what=f"VFO at {hz}")
    if v.get("am") != "1":
        skip("state", f"the VFO at {hz} Hz is not received in AM (am={v.get('am')})")
    return v


def _floor(radio):
    """The RSSI with nothing sent: the highest of a few readings."""
    vals = []
    for _ in range(5):
        vals.append(int(radio.core()["rssi_dbm"]))
        time.sleep(0.1)
    return max(vals)


@pytest.mark.feature("am_rx")
@pytest.mark.rf
@pytest.mark.bench_service
def test_am_rx_squelch(radio, sdr_bench, pytestconfig):
    mhz, hz = am_vfo_ready(radio, pytestconfig)
    with radio.vfo_session():
        tune_am(radio, hz)
        time.sleep(1.0)
        floor = _floor(radio)
        if radio.core().get("state") == "rx":
            skip("state", f"the squelch is open on {mhz} MHz with nothing sent: something else is on the channel")
        seen = None
        with sdr_bench.lease():
            tx = am_tx_start(sdr_bench, mhz, PROGRAM, depth=DEPTH, lead=LEAD_S, tail=LEAD_S)
            end = time.monotonic() + 4.0
            while time.monotonic() < end and not tx.over.is_set():
                v = radio.core()
                if v.get("state") == "rx" or int(v["rssi_dbm"]) >= floor + RISE_DB:
                    seen = {"state": v.get("state"), "rssi_dbm": v["rssi_dbm"]}
                    break
                time.sleep(0.1)
            done = tx.wait(20)
        print(f"bench {done}; floor {floor} dBm; radio during the carrier {seen}")
        assert not done.get("cut"), f"the bench cut its transmission: {done}"
        assert seen, f"the radio never showed the carrier (state rx, or rssi_dbm {RISE_DB:g} dB over {floor} dBm)"
        radio.wait_for(lambda x: x.get("state") == "idle", timeout=3.0, what="the squelch closing after the carrier")
