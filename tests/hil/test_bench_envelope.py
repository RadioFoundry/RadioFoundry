# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""A bench board's burst and envelope records as the DMR TX checks read them (rfoundry_hil.py, dmr_bench.check_tx), no
radio: tools/ci.sh runs it. The board's sample count is the time base, so a UTC step inside a transmission moves nothing."""
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


def test_slow_ramp_fails():
    sdr = analysis()
    sdr["edges"][4]["ramp_up_ms"] = 2.0
    assert [i["check"] for i in checks(sdr)] == ["ramps < 1.5 ms"]


def test_cut_segment_is_not_judged():
    edges = H.SdrBench.edges_from_segments([segment(0, open_end=True), segment(1, ramp_up_ms=None), segment(2)], 5000, FS)
    assert ["on_ms" in e for e in edges] == [False, False, True]
