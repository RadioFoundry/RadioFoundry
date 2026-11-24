# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Fox mode (docs/spec/radio-features.md 3.8) on the radio, checked off air by the SDR bench (opt-in: --allow-rf
--allow-service-stop; the radio and the bench's receive board only, the bench never transmits here).

The radio's FM VFO is tuned to --fm-mhz on its lowest power, with no sub-audio tone. The test writes a fox record of its
own through the debug link (CPKV: every 30 s, no message, a jingle of 1000 Hz then 1500 Hz, 600 ms each), starts the
fox (RADIO op 44) and follows the fox section through one cycle while the receive board demodulates it; then it stops
the fox and puts the radio's own record back (or none). A radio whose name holds no callsign skips (the fox won't
start without one).

Pass criteria:
- test_fox_cycle: the fox starts (fox.on=1) and keys once: one keyed segment; in it 1000 Hz then 1500 Hz (+-15 Hz),
  each on air 0.45-0.75 s (nominally 0.6 s), deviation 300 Hz to 2.5 kHz peak at 12.5 kHz; then the ID's Morse: at
  least three runs of one tone (the CW tone, +-15 Hz) after the jingle. After the cycle the radio is back in receive, fox.phase=wait,
  fox.cycles=1, fox.next_s at most 30. The stop request ends it (fox.on=0, fox.why=stopped) and nothing keys after.
"""
import time

import pytest

from hil_config import rig
from rfoundry_hil import skip
from rfoundry_debug import DebugLink, FOX_KEY
import fm_measure
from test_fm_tx import fm_vfo, set_fm  # noqa: F401  (fm_vfo: the fixture)

JINGLE = [(1000, 600), (1500, 600)]
CYCLE_MAX_S = 20.0       # the ID of a 10-character callsign at 20 wpm is about 8 s


def runs_near(r, hz, tol=15.0):
    return [t for t in r["tones"] if abs(t[0] - hz) <= tol]


@pytest.fixture
def fox_record(fm_vfo):
    """The test's fox record in the codeplug; the radio's own (or none) put back afterwards."""
    L = fm_vfo.link
    saved = L.cpkv_get(FOX_KEY)
    L.cpkv_set(FOX_KEY, DebugLink.fox_record(30, "", msg=False, jingle=True, steps=JINGLE))
    try:
        yield fm_vfo
    finally:
        L.fox("stop")
        if saved is None:
            L.cpkv_remove(FOX_KEY)
        else:
            L.cpkv_set(FOX_KEY, saved)


@pytest.mark.feature("fox")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
@pytest.mark.needs_switch("VFO_FM")
def test_fox_cycle(fox_record, sdr_bench, pytestconfig):
    radio = fox_record
    if "FOX" not in radio.features():
        skip("not_built", "built without CONFIG_FOX")
    set_fm(radio, False, "none", 0)
    mhz = rig(pytestconfig, "fm-mhz")
    with sdr_bench.lease():
        job = sdr_bench.fm_over_start(mhz, CYCLE_MAX_S + 3, bw_khz=12.5, audio=True)
        radio.link.fox("start")
        v = radio.wait_for(lambda v: v.get("fox.on") == "1" or v.get("fox.why") not in (None, "none"), 2.0,
                           sections=("fox",), what="fox started or refused")
        if v.get("fox.why") == "no_call":
            sdr_bench.fm_over_result(job)
            skip("state", "the radio name holds no callsign: the fox won't start")
        assert v.get("fox.on") == "1", f"the fox didn't start: {v}"
        radio.wait_for(lambda v: v.get("fox.cycles") == "1" and v.get("fox.phase") == "wait", CYCLE_MAX_S,
                       interval=0.25, sections=("fox",), what="one fox cycle sent")
        radio.wait_for(lambda v: v.get("state") != "tx", 2.0, what="back in receive")
        r = sdr_bench.fm_over_result(job)
    print(fm_measure.describe(r))
    print("tones:", r["tones"])
    end = radio.view(["fox"])
    assert end.get("fox.on") == "1" and int(end.get("fox.next_s", "99")) <= 30, end
    assert r["segments"] == 1, f"{r['segments']} keyed segments for one cycle"
    # The jingle: 1000 Hz then 1500 Hz, each about 600 ms
    lo, hi = runs_near(r, 1000.0), runs_near(r, 1500.0)
    assert lo and hi, f"the jingle's tones aren't on air: {r['tones']}"
    a, b = max(lo, key=lambda t: t[2] - t[1]), max(hi, key=lambda t: t[2] - t[1])
    assert a[1] < b[1], f"1500 Hz came before 1000 Hz: {a} {b}"
    for hz, t0, t1, dev in (a, b):
        assert 0.45 <= t1 - t0 <= 0.75, f"{hz} Hz on air {t1 - t0:.2f} s (nominally 0.6 s)"
        assert 300 <= dev <= 2500, f"{hz} Hz deviation {dev} Hz peak at 12.5 kHz"
    # The ID after it: Morse elements, runs of one tone (the CW tone)
    after = [t for t in r["tones"] if t[1] > b[2]]
    assert after, f"nothing on air after the jingle: {r['tones']}"
    cw = max((t[0] for t in after), key=lambda hz: len(runs_near({"tones": after}, hz)))
    assert len(runs_near({"tones": after}, cw)) >= 3, f"no Morse ID after the jingle: {after}"
    # Stopped: off with the reason, nothing keys
    radio.link.fox("stop")
    radio.wait_for(lambda v: v.get("fox.on") == "0" and v.get("fox.why") == "stopped", 2.0, sections=("fox",),
                   what="fox stopped")
    time.sleep(1.0)
    assert radio.view(["core"]).get("state") != "tx"
