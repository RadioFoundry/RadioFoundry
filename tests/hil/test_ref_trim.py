# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The reference trim (radio-features.md 6.1) over the debug link's SETTING op 35, RAM only.

A trim in range (ppb) is taken, rounded to 100 ppb, and shows as `ref_trim` in the storage section; past +-10000 ppb
(10.0 ppm) it is refused (bad argument) and the trim in effect stays. The stored setting (`settings.ref_trim`) isn't changed, and the
trim in effect before is put back. Skipped on a radio without the trim (`ref_trim=-`).

test_ref_trim_moves_carrier (radio_tx, --allow-rf; the SDR bench): the FM VFO on --fm-mhz, lowest power, no tone,
keyed at trims 0, +5.0 and -5.0 ppm. Each trim moves the carrier from the one at 0 by trim x f within
CARRIER_TOL_HZ (5.0 ppm is 2307 Hz at 461.4625 MHz): the board's scale, its sign and that the trim reaches the
transmitter. The trim in effect before is put back.
"""
import time

import pytest

from rfoundry_hil import skip
from rfoundry_debug import DebugLinkError
from test_fm_tx import PTT_S, fm_vfo, parse_fm, set_fm  # noqa: F401  (fm_vfo: the fixture)

TRIMS_PPB = (5000, -5000)
# The retune the trim asks for, and time for the RF supply to settle before the key-up: a radio keyed right after a
# retune can dip its supply and restart
TRIM_SETTLE_S = 3.0
CARRIER_TOL_HZ = 120		# the SDR's carrier estimate and, on a board with a reference control, its scale


def trim_view(radio):
    v = radio.link.view(["storage"])
    return v.get("ref_trim"), v.get("settings.ref_trim")


@pytest.mark.feature("ref_trim")
def test_ref_trim_setting(radio):
    now, stored = trim_view(radio)
    if now is None:
        skip("not_built", "the firmware has no reference trim keys (older firmware)")
    if now == "-":
        skip("not_built", "the reference trim is not written for this radio (no hal_radio_t.ref_trim)")
    before = int(now)
    try:
        radio.link.setting("ref_trim", -1240)
        time.sleep(0.06)        # the radio request rate limit
        assert trim_view(radio) == ("-1200", stored), "a trim in range is taken, rounded to 100 ppb, not stored"
        radio.link.setting("ref_trim", 10000)
        time.sleep(0.06)
        assert trim_view(radio)[0] == "10000"
        with pytest.raises(DebugLinkError):
            radio.link.setting("ref_trim", 10100)
        time.sleep(0.06)
        with pytest.raises(DebugLinkError):
            radio.link.setting("ref_trim", -10100)
        time.sleep(0.06)
        assert trim_view(radio) == ("10000", stored), "a refused trim leaves the one in effect"
    finally:
        radio.link.setting("ref_trim", before)
        time.sleep(0.06)
    assert trim_view(radio) == (str(before), stored)


def carrier_hz(radio, sdr_bench, pytestconfig, mhz, tag):
    """One keyed segment's carrier, Hz from mhz (the SDR bench's own error corrected)."""
    rdir = sdr_bench.run_dir(tag)
    with sdr_bench.lease():
        cap = sdr_bench.capture_start(rdir, mhz, seconds=PTT_S + 5)
        radio.ptt_for(PTT_S)
        radio.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
        sdr_bench.capture_wait()
    _, text = sdr_bench.analyze_fm(cap, bw_khz=25, ppm=pytestconfig.getoption("--sdr-ppm"), freq=mhz)
    r = parse_fm(text)
    assert r["segments"] >= 1, f"no keyed segment at {tag}: {text[-600:]}"
    hz = r.get("carrier_corrected_hz", r.get("carrier_hz"))
    assert hz is not None, f"no carrier at {tag}: {text[-600:]}"
    return hz


@pytest.mark.feature("ref_trim")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_ref_trim_moves_carrier(fm_vfo, sdr_bench, pytestconfig):
    radio = fm_vfo
    now, _ = trim_view(radio)
    if now is None:
        skip("not_built", "the firmware has no reference trim keys (older firmware)")
    if now == "-":
        skip("not_built", "the reference trim is not written for this radio (no hal_radio_t.ref_trim)")
    mhz = pytestconfig.getoption("--fm-mhz")
    set_fm(radio, False, "none", 0)
    before = int(now)
    try:
        radio.link.setting("ref_trim", 0)
        time.sleep(TRIM_SETTLE_S)
        base = carrier_hz(radio, sdr_bench, pytestconfig, mhz, "trim0")
        print(f"trim 0: carrier {base:+d} Hz")
        for ppb in TRIMS_PPB:
            radio.link.setting("ref_trim", ppb)
            time.sleep(TRIM_SETTLE_S)
            hz = carrier_hz(radio, sdr_bench, pytestconfig, mhz, f"trim{ppb:+d}")
            want = round(ppb * mhz / 1000)
            print(f"trim {ppb / 1000:+.1f} ppm: carrier {hz:+d} Hz, moved {hz - base:+d} Hz (want {want:+d})")
            assert abs(hz - base - want) <= CARRIER_TOL_HZ, \
                f"trim {ppb} ppb moved the carrier {hz - base:+d} Hz, want {want:+d} +-{CARRIER_TOL_HZ}"
    finally:
        radio.link.setting("ref_trim", before)
        time.sleep(0.06)
