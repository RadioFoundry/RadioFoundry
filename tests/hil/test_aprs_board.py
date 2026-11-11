# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""SdrBench.aprs_from_records: a bench board's APRS decode (an FM rx job with "aprs": true) as aprs_afsk_check.py's
JSON, the shape test_aprs.py's on-air tests read. No radio and no bench: these run anywhere, and tools/ci.sh runs
them."""
from rfoundry_hil import SdrBench

FRAME = {"tnc2": "N0CALL-15>APZRDF:>status", "src": "N0CALL-15", "dst": "APZRDF", "digis": [], "control": 3,
         "pid": 240, "info": ">status", "fcs_ok": True, "t0": 1.234, "t1": 1.534, "txdelay_flags": 22,
         "txdelay_ms": 146.7, "decision_gain": 1.0, "peak_dev_hz": 3100.0, "max_dev_hz": 3400.0,
         "rms_dev_hz": 2100.0, "mark_dev_hz": 2900.0, "space_dev_hz": 2700.0, "twist_db": 0.6, "mark_hz": 1200.4,
         "space_hz": 2199.1}


def _blocks(on, n=20, step=0.1):
    return [{"block": {"t": round(i * step, 3), "carrier": on[0] <= i < on[1], "power_dbfs": -20.0 if
                       on[0] <= i < on[1] else -70.0, "cfo_hz": 150.0 if on[0] <= i < on[1] else None}}
            for i in range(n)]


def _done(frames=1, fcs_bad=0):
    return {"mode": "fm", "freq": 446.05, "seconds": 2.0,
            "summary": {"noise_dbfs": -71.5, "aprs": {"frames": frames, "fcs_bad": fcs_bad}}}


def test_frame_fields_and_units():
    res = SdrBench.aprs_from_records(_blocks((10, 17)) + [{"aprs": FRAME}], _done())
    assert res["source"] == "board" and res["fcs_bad"] == 0 and res["decoded"] == 1
    (f,) = res["frames"]
    assert f["src"] == "N0CALL-15" and f["dst"] == "APZRDF" and f["digis"] == [] and f["fcs_ok"]
    assert f["control"] == 0x03 and f["pid"] == 0xF0
    assert f["t_start_s"] == 1.234 and f["t_end_s"] == 1.534 and "t0" not in f
    for k in ("peak_dev_hz", "max_dev_hz", "mark_dev_hz", "space_dev_hz", "twist_db", "mark_hz", "space_hz",
              "txdelay_flags", "txdelay_ms"):
        assert f[k] == FRAME[k], k
    # what the board doesn't measure is absent, not zero
    assert "keyup_to_data_ms" not in f and "phase_rms_deg" not in f


def test_segments_from_the_carrier_blocks():
    res = SdrBench.aprs_from_records(_blocks((10, 17)), _done(0))
    assert res["frames"] == [] and res["floor_dbfs"] == -71.5
    (seg,) = res["segments"]
    assert seg == {"t_start_s": 1.0, "t_end_s": 1.7, "carrier_hz": 150, "level_dbfs": -20.0}
    assert isinstance(seg["carrier_hz"], int)       # test_aprs.py prints it with {:+d}


def test_carrier_to_the_end_of_the_job_closes_the_segment():
    res = SdrBench.aprs_from_records(_blocks((15, 20)), _done(0))
    assert [(s["t_start_s"], s["t_end_s"]) for s in res["segments"]] == [(1.5, 2.0)]


def test_hex_octets_text_digis_and_derived_txdelay():
    a = dict(FRAME, control="0x03", pid="F0", digis="WIDE1-1,WIDE2-1")
    del a["txdelay_ms"]
    (f,) = SdrBench.aprs_from_records([{"aprs": a}], _done())["frames"]
    assert f["control"] == 3 and f["pid"] == 240 and f["digis"] == ["WIDE1-1", "WIDE2-1"]
    assert f["txdelay_ms"] == 146.7       # 22 flags of 8 bits at 1200 baud


def test_bad_fcs_count_and_no_frame():
    res = SdrBench.aprs_from_records([], _done(0, fcs_bad=2))
    assert res["frames"] == [] and res["fcs_bad"] == 2 and res["segments"] == []


def test_board_without_the_decode():
    # a board that doesn't know "aprs" ignores it: its summary has no aprs, and the caller skips
    assert SdrBench.aprs_from_records([{"aprs": FRAME}], {"summary": {"noise_dbfs": -70}}) is None
    assert SdrBench.aprs_from_records([], None) is None
