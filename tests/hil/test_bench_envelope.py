# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""A bench board's burst and envelope records as the DMR TX checks read them (rfoundry_hil.py, dmr_bench.check_tx), no
radio: tools/ci.sh runs it. The board's sample count is the time base, so a UTC step inside a transmission moves nothing."""
import pytest

import dmr_bench
import rfoundry_hil as H

FS = 384000
LC = {"type": "grp_voice", "flco": 0, "src": 1234, "dst": 9, "group": True}


def rec(i, kind, t_step=0.0):
    lc = dict(LC, emb_flco=0) if kind == "VOICE_E" else LC if kind in ("VLC_HDR", "TERM_LC") else None
    return {"t": 1000.0 + 0.06 * i + t_step, "sc": 5000 + i * 23040, "t_rel_ms": 0.0, "slot_ab": "A", "slot": 0,
            "sync": "MS_DATA" if not kind.startswith("VOICE") else "EMB", "voice": kind.startswith("VOICE"), "kind": kind,
            "cc": 2, "lc": lc, "cfo_hz": 10.0, "level_dbfs": -12.0, "dev_hz": 1900.0, "fec": {"fec_ok": True}}


def segment(i, **kw):
    s = {"up_sc": 5000 + i * 23040 - 5900, "on_ms": 30.5, "ramp_up_ms": 0.45, "ramp_down_ms": 1.1, "level_dbfs": -12.0,
         "steps": [], "open_start": False, "open_end": False, "gap": False}
    s.update(kw)
    return s


def analysis(step_at=None):
    kinds = ["VLC_HDR"] * 3 + [f"VOICE_{c}" for c in "ABCDEF"] * 2 + ["TERM_LC"]
    rows = []
    for i, k in enumerate(kinds):
        r = rec(i, k, 0.3 if step_at is not None and i >= step_at else 0.0)
        r["t_rel_ms"] = (r["sc"] - 5000) / FS * 1e3
        rows.append(r)
    edges = H.SdrBench.edges_from_segments([segment(i) for i in range(len(kinds))], 5000, FS)
    env = {"clip_pct": 0.0, "overlaps": 0, "power_steps": 0, "unexplained_carrier": []}
    return H.SdrBench.analysis_from_bursts(rows, {"envelope": env, "edges": edges})


def checks(sdr):
    c = dmr_bench.Checks()
    dmr_bench.check_tx(sdr, {"flco": 0, "dst": 9}, 1234, 2, c)
    return [i for i in c.items if i["ok"] is False]


def test_whole_transmission_passes():
    assert checks(analysis()) == []


def test_utc_step_changes_nothing():
    assert checks(analysis(step_at=7)) == []


def test_utc_step_keeps_the_slot():
    """A board labels slot_ab and t_rel_ms from `t`: both come from the sample count here."""
    class Watch:
        api, seconds, error = True, 1, None
        replies = []

        @staticmethod
        def wait(_timeout):
            return True

    Watch.replies = [{"burst": dict(rec(i, "VOICE_A", 0.345 if i >= 2 else 0.0), slot_ab="B" if i >= 2 else "A")}
                     for i in range(4)]
    bench = H.SdrBench.__new__(H.SdrBench)
    bench._api_info = lambda rx=False: {"window": {"fs": FS}}
    rows, _ = bench.bursts_result(Watch())
    assert [r["slot_ab"] for r in rows] == ["A"] * 4
    assert [round(r["t_rel_ms"]) for r in rows] == [0, 60, 120, 180]


def test_slow_ramp_fails():
    sdr = analysis()
    sdr["edges"][4]["ramp_up_ms"] = 2.0
    assert [i["check"] for i in checks(sdr)] == ["ramps < 1.5 ms"]


def test_cut_segment_is_not_judged():
    edges = H.SdrBench.edges_from_segments([segment(0, open_end=True), segment(1, ramp_up_ms=None), segment(2)], 5000, FS)
    assert ["on_ms" in e for e in edges] == [False, False, True]


def test_cach_decoded_in_burst_order():
    import dmr_cach
    words = [dmr_cach.cach_build(0, i % 2, 0, 0x155) for i in range(2)]
    rows = [dict(rec(i, "VLC_HDR"), sync="BS_DATA", cach=f"{w:06x}", sync_errors=1) for i, w in enumerate(words)]
    bursts = H.SdrBench.analysis_from_bursts(rows, {})["bursts"]
    assert [(b["cach"]["ok"], b["cach"]["tc"]) for b in bursts] == [(True, 0), (True, 1)]
    assert bursts[0]["t"] == rows[0]["t"] and bursts[0]["sync_errors"] == 1


def test_no_cach_on_other_bursts():
    bursts = H.SdrBench.analysis_from_bursts([dict(rec(0, "VLC_HDR"), cach=None)], {})["bursts"]
    assert "cach" not in bursts[0]


def test_counts_of_each_kind():
    sdr = H.SdrBench.analysis_from_bursts([rec(i, "VOICE_A") for i in range(3)], {})
    assert sdr["summary"]["counts"] == {"VOICE_A": 3}


def skipped(fn, *args):
    """The skip fn raises (pytest's isn't an Exception)."""
    try:
        fn(*args)
    except BaseException as e:
        return e
    raise AssertionError("no skip")


@pytest.mark.parametrize("need", ["need_dmr_waveform", "need_bursts_cach"])
def test_a_bench_without_a_board_skips(need):
    bench = H.SdrBench.__new__(H.SdrBench)
    bench.api = None
    e = skipped(getattr(bench, need), 446.0)
    assert e.kind == "bench" and "bench board" in str(e)
