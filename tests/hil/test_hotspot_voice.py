# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The MMDVM hotspot on air, both directions (opt-in: --allow-rf --allow-service-stop; the radio transmits).

The radio is driven over USB as MMDVMHost drives a simplex modem (tools/bench/mmdvm_hotspot.py's MmdvmLink on the
DMR test channel, RF level 12: the radio's lowest power), and the SDR bench's modem is the other radio. The cases are
tools/bench/hotspot_cases.py's (docs/testing/dmr-bench.md); this module runs them under the bench lease:

- rf_voice8: the bench modem sends a group call (8 headers, 3 superframes, 3 terminators); what the radio sends up
  the USB link, fed through a model of MMDVMHost's DMRSlot::writeModem(), is one call with the bench's LC, no
  DMR_LOST, 17 or 18 of 18 voice bursts in sequence, ended by the terminator.
- rf_data_unconf: unconfirmed data from the bench reaches the host (header and blocks, in order).
- net_voice: a network call sent the way MMDVMHost sends one (paced by GET_STATUS space) is heard on air by the bench
  modem: one call, the network call's LC, >= 17 of 18 voice bursts, a terminator, no voice after it, no NAKs.
- net_data: network data on air: the data header and blocks heard by the bench modem.

Needs the odr_sdrbench package on this machine (burst building; installed, or a checkout in $ODR_SDRBENCH) and
numpy; skipped without them. The hotspot ends when the MMDVM port closes (DTR
drop); restore_radio puts the channel back."""
import os
import sys
import time

import pytest

from odr_hil import ROOT, skip

CASES = ("rf_voice8", "rf_data_unconf", "net_voice", "net_data")


@pytest.fixture(scope="module")
def hotspot_cases():
    sys.path.insert(0, os.path.join(ROOT, "tools", "bench"))
    try:
        import hotspot_cases as H
    except ImportError as e:
        skip("bench", f"needs odr_sdrbench and numpy for burst building: {e}")
    return H


@pytest.mark.rf
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.parametrize("case", CASES)
def test_hotspot_on_air(radio, sdr_bench, pytestconfig, hotspot_cases, case):
    H = hotspot_cases
    if "HOTSPOT" not in radio.features():
        skip("not_built", "built without the hotspot")
    freq, cc = pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc")
    H.BENCH_HOST = sdr_bench.host
    args = type("Args", (), {"freq": freq, "cc": cc, "power": 12})()
    r = H.Result(case)
    with sdr_bench.lease(), radio.released() as port:
        # the bench modem as SdrBench runs it: this bench's host, directory, python and transmit level (on a
        # shared bench, the channel and the modem held for the case)
        H.MODEM_CMD = sdr_bench.modem_prefix(freq, expect_s=60)
        t = H.M.Transport(port)
        hs = H.M.MmdvmLink(t, int(round(freq * 1e6)), cc, args.power, simplex=True)
        try:
            H.CASES[case](hs, args, r)
        finally:
            hs.stop = True
            time.sleep(0.3)
            t.close()
    for what, ok in r.checks:
        print(f"{'ok ' if ok else 'BAD'} {what}")
    print(r.info)
    radio.wait_for(lambda v: v.get("hotspot") == "0", 25, what="the radio back from hotspot mode")
    assert r.ok, f"{case}: " + "; ".join(w for w, ok in r.checks if not ok)
