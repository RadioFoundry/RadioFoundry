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

from rfoundry_hil import skip
from hil_config import rig

RISE_DB = 10.0              # rssi_dbm over the floor that counts as the carrier when the state shows no squelch
LEAD_S = 0.5                # bare carrier before and after the tone: the squelch opens and closes on it
DEPTH = 0.3
PROGRAM = [{"kind": "tone", "hz": 1000, "s": 3.0, "level": 1.0}]


def am_tx_start(bench, freq, program, max_s=10.0, **fields):
    """AM from the bench board on freq (MHz) in the background (SdrBench.tx_start: the tx op, mode am): a
    _LineTx once the board keyed; wait() gives its done record (depth, carrier_db, cut ...). A board that only sends
    frequency refuses AM: a skip (bench). Needs the lease and --allow-rf."""
    return bench.tx_start("am", freq, program, max_s, **fields)


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
            tx = am_tx_start(sdr_bench, mhz, PROGRAM, depth=DEPTH, lead_s=LEAD_S, tail_s=LEAD_S)
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
