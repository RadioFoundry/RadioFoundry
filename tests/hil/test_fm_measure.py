# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""fm_measure: a bench board's FM receive job read as the FM tests' report, on made-up records. No radio and no bench:
tools/ci.sh runs it when numpy is installed."""
from types import SimpleNamespace

from rfoundry_hil import SdrBench, skip

try:
    import numpy as np
except ImportError:
    skip("bench", "numpy is not installed", module=True)

import pytest

import fm_measure
from sdr_fm_check import received_twist

FS = 16000
FULL = 5000.0
HEADER = {"streams": [{"name": "audio", "unit": "Hz", "rate": FS, "full": FULL}], "freq_tol_hz": 40, "ppm_source": "gps"}


def blocks(*keyed, n=300, step=0.01, cfo=120.0):
    """n block records, carrier on in the index ranges of keyed"""
    out = []
    for i in range(n):
        on = any(a <= i < b for a, b in keyed)
        out.append({"block": {"t": round(i * step, 3), "carrier": on, "power_dbfs": -20.0 if on else -70.0,
                              "cfo_hz": cfo if on else None}})
    return out


def pcm(x):
    return (np.asarray(x) * 32767.0 / FULL).astype("<i2").tobytes()


def test_a_short_dropout_is_bridged():
    assert fm_measure.measure({}, blocks((50, 100), (102, 150)))["segments"] == 1


def test_a_long_dropout_splits():
    r = fm_measure.measure({}, blocks((50, 100), (120, 170)))
    assert r["spans"] == [(0.5, 1.0), (1.2, 1.7)]
    assert r["levels"] == [-20.0, -20.0]


def test_a_blip_is_no_segment():
    assert fm_measure.measure({}, blocks((50, 70)))["segments"] == 0


@pytest.mark.parametrize("cfo", [-87.4, {"mean": -87.4, "min": -90.0, "max": -80.0}])
def test_carrier_from_the_summary(cfo):
    assert fm_measure.measure({"summary": {"cfo_hz": cfo}}, blocks((50, 150)))["carrier_hz"] == -87


def test_carrier_from_the_blocks_without_a_summary():
    assert fm_measure.measure({}, blocks((50, 150), cfo=33.0))["carrier_hz"] == 33


def test_track_is_the_carrier_every_50_ms():
    r = fm_measure.measure({}, blocks((50, 100), cfo=-40.0))
    assert [t for t, _ in r["track"]] == [0.5, 0.55, 0.6, 0.65, 0.7, 0.75, 0.8, 0.85, 0.9, 0.95]
    assert {hz for _, hz in r["track"]} == {-40}


def test_ctcss():
    r = fm_measure.measure({"summary": {"sub": {"kind": "ctcss", "hz": 100.1, "dev_hz": 362.4}}}, [])
    assert (r["ctcss_hz"], r["ctcss_dev_hz"]) == (100.1, 362)
    assert "dcs" not in r


def test_dcs_codes_from_a_string_of_aliases():
    sub = {"kind": "dcs", "code": "D023N", "aliases": "D047I D244I", "dev_hz": 511.0, "turnoff": True}
    r = fm_measure.measure({"summary": {"sub": sub}}, [])
    assert r["dcs_codes"] == ["D023N", "D047I", "D244I"]
    assert r["dcs_turnoff"] is True


def test_no_turnoff_reported():
    r = fm_measure.measure({"summary": {"sub": {"kind": "dcs", "code": "D023N", "aliases": ""}}}, [])
    assert r["dcs_codes"] == ["D023N"] and r["dcs_turnoff"] is False


def test_dtmf_twist_after_de_emphasis():
    rec = {"dtmf": {"digit": "5", "t0": 1.0, "t1": 1.2, "s": 0.2, "dev_hz": 2400.2, "twist_db": 1.5}}
    r = fm_measure.measure({}, [rec])
    assert r["dtmf"] == [("5", 1.0, 1.2, 2400, 1.5, round(received_twist(1.5, "5"), 1))]


def test_voice_figures_from_the_audio_stream():
    t = np.arange(3 * FS) / FS
    x = 1500.0 * np.sin(2 * np.pi * 1000.0 * t)
    r = fm_measure.measure({}, blocks((50, 250)), HEADER, pcm(x))
    v = r["voice"][0]
    assert abs(v["peak"] - 1500) < 30 and abs(v["rms"] - 1061) < 30
    assert len(v["levels"]) == 40


def test_no_audio_no_voice_figures():
    r = fm_measure.measure({}, blocks((50, 250)))
    assert r["voice"] == [] and r["tones"] == []


def test_voice_figures_allow_for_the_stream_delay():
    # A burst just past the segment's trimmed end, inside it only once the audio's delay is counted
    x = np.full(3 * FS, 100.0)
    end = 2.5 - fm_measure.EDGE_S
    x[int((end + 0.001) * FS):int((end + 0.008) * FS)] = 3000.0
    assert fm_measure.measure({}, blocks((50, 250)), HEADER, pcm(x))["voice"][0]["peak999"] > 2000


class Job:
    records, header, pcm, seconds, want_audio = [], {}, {}, 1.0, True
    req = SimpleNamespace(replies=[])

    def __init__(self, done):
        self.done = done

    def wait(self, _timeout):
        return self.done


def skipped(fn, *args):
    """The skip fn raises (pytest's isn't an Exception)."""
    try:
        fn(*args)
    except BaseException as e:
        return e
    raise AssertionError("no skip")


def test_lost_samples_skip_as_a_bench_fault():
    e = skipped(SdrBench.fm_over_result, SdrBench.__new__(SdrBench), Job({"gap_samples": 96}))
    assert e.kind == "bench" and "gap_samples" in str(e)


def test_a_missing_audio_stream_is_a_bench_fault():
    bench = SdrBench.__new__(SdrBench)
    bench.api_rx = "board"
    e = skipped(bench.fm_over_result, Job({"gap_samples": 0}))
    assert e.kind == "bench" and "no FM audio" in str(e)


def test_tone_runs_from_the_board():
    run = {"tone_run": {"hz": 1750.04, "t0": 0.6, "t1": 1.1, "s": 0.5, "dev_hz": 1251.4}}
    late = {"tone_run": {"hz": 900.0, "t0": 3.5, "t1": 3.6, "s": 0.1, "dev_hz": 1100.0}}
    t = np.arange(3 * FS) / FS
    r = fm_measure.measure({}, blocks((50, 250)) + [run, late], HEADER, pcm(0.0 * t))
    assert r["tones"] == [(1750.0, 0.6, 1.1, 1251), (900.0, 3.5, 3.6, 1100)]
    assert r["voice"][0]["tones"] == [(1750.0, 0.6, 1.1, 1251)]


def test_pieces_of_one_digit_are_joined():
    got = fm_measure.merge_dtmf([("#", 1.203, 1.238, 763, 5.6, 1.8), ("#", 1.248, 1.333, 735, 5.4, 1.6)])
    assert len(got) == 1 and got[0][:3] == ("#", 1.203, 1.333) and 735 <= got[0][3] <= 763


def test_another_digit_or_a_new_press_stays_apart():
    two = [("1", 0.9, 1.0, 700, 5.0, 1.0), ("2", 1.01, 1.1, 700, 5.0, 1.0)]
    assert fm_measure.merge_dtmf(two) == two
    again = [("7", 0.9, 1.0, 700, 5.0, 1.0), ("7", 1.04, 1.14, 700, 5.0, 1.0)]
    assert fm_measure.merge_dtmf(again) == again
