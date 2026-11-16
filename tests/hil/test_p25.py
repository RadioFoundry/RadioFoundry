# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""P25 conventional on the radio, with nothing sent (docs/spec/p25.md 7). Radio only: the radio never transmits.

test_p25_channel_switch: the channel on view becomes P25 by a temporary mode change (the debug link's mode request,
debuglink.md 3.8 op 31); the p25 view section must show the engine running (`p25.on=1`, no new `begin_fails`) and,
where the provider counts it, the receive load at idle (`p25.rx_work_us / p25.rx_span_us`, at most MAX_IDLE_LOAD).
Undoing the temporary change brings the channel's own mode back with the engine stopped, and a DMR channel's call
stack running again.

test_p25_settings: the P25 settings (op 39, RAM only) round trip through the p25 view and are put back.

test_p25_tx_off_air (opt-in: --allow-rf --allow-service-stop --p25-mhz MHz; the radio transmits, the bench only
receives): the VFO on --p25-mhz in P25 at the lowest power keys for PTT_S through the debug link while the bench's
receive board records its FM discriminator (the rx op's disc stream). tools/bench/p25tool.py (the firmware's own
framer and call layer, built for the host) decodes it. Pass criteria:
- one call: a header first with the radio's NAC (p25.tx_nac) and talk group (p25.tx_dst), a terminator last;
- LDU1 and LDU2 in turn, every LDU1's link control with the radio's ID (p25.unit_id, else dmr.own_id) and the talk group, every LDU2's
  encryption sync clear;
- the LDUs back to back: 180 ms apart (+-0.5 ms), as many as the over's length gives (within two);
- every unit's NID decoded with at most MAX_NID_ERRS bit errors; no voice frame bad, the corrected bits at most
  MAX_CORRECTED of the voice bits; no link control or encryption sync failure;
- the outer deviation the soft modem measures at its syncs (median) within DEV_PCT of 1800 Hz;
- on the radio: p25.tx_calls one more, no p25.tx_underruns.
The carrier offset the soft modem measures is printed, not judged: it includes the bench board's own error.

Skipped without CONFIG_P25, on a channel in a listen-only band, and in hotspot mode."""
import os
import statistics
import tempfile
import time

import pytest

from rfoundry_hil import as_int, skip
from hil_config import rig

# Receive work over the signal it covered, P25 running with nothing on the channel. The soft demodulator runs on
# every sample whether a call is there or not, so idle is close to the busy figure
MAX_IDLE_LOAD = float(os.environ.get("RFOUNDRY_HIL_P25_MAX_LOAD", "0.5"))


def p25_ready(radio):
    if "P25" not in radio.features():
        skip("not_built", "built without CONFIG_P25")
    radio.go_home()
    if radio.core().get("hotspot") == "1":
        skip("state", "the radio is in hotspot mode")


def p25_view(radio):
    v = radio.view(["p25"])
    if v.get("p25.built") != "1":
        skip("not_built", "no p25 view section")
    return v


def load(radio):
    """(work_us, span_us) of the provider's receive since P25 started, or None when it doesn't count them."""
    v = radio.view(["p25"])
    w, s = as_int(v.get("p25.rx_work_us")), as_int(v.get("p25.rx_span_us"))
    return None if w is None or s is None else (w, s)


@pytest.mark.feature("p25")
def test_p25_channel_switch(radio):
    p25_ready(radio)
    snap = radio.snapshot()
    mode0 = radio.core().get("mode")
    if mode0 == "p25":
        skip("state", "the channel is P25 already")
    fails0 = as_int(p25_view(radio).get("p25.begin_fails")) or 0
    try:
        radio.link.dmr_fields(mode="p25")
        try:
            v = radio.wait_for(lambda x: x.get("mode") == "p25" and x.get("p25.on") == "1", 3.0,
                               sections=("core", "p25"), what="P25 running on the channel")
        except AssertionError:
            v = radio.view(["core", "p25"])
            if v.get("mode") != "p25":
                skip("state", f"the radio kept mode {v.get('mode')} (a listen-only band, or the VFO without "
                              "both modes)")
            raise
        assert (as_int(v.get("p25.begin_fails")) or 0) == fails0, "the engine did not start (begin_fails)"
        print(f"\np25: nac {v.get('p25.rx_nac')}/{v.get('p25.tx_nac')} tg {v.get('p25.rx_tgid')} "
              f"state {v.get('state')}")
        a = load(radio)
        if a is not None:
            time.sleep(3.0)
            b = load(radio)
            dw, ds = b[0] - a[0], b[1] - a[1]
            assert ds > 0, "the provider counted no receive in 3 s"
            share = dw / ds
            print(f"p25 receive load at idle: {dw} us of work over {ds} us = {share:.1%}")
            assert share <= MAX_IDLE_LOAD, f"receive takes {share:.1%} of the CPU at idle"
        assert radio.core().get("state") in ("idle", "rx")
    finally:
        radio.restore(snap)
    v = radio.wait_for(lambda x: x.get("mode") == mode0 and x.get("p25.on") == "0", 3.0,
                       sections=("core", "p25"), what=f"mode {mode0} back, P25 stopped")
    if mode0 == "dmr" and "DMR" in radio.features():
        radio.wait_for(lambda x: x.get("dmr.active") == "1", 3.0, sections=("dmr",), what="the DMR stack back")


@pytest.mark.feature("p25")
def test_p25_settings(radio):
    p25_ready(radio)
    v = p25_view(radio)
    old = {k: int(v[f"p25.{k}"]) for k in ("rx_nac", "tx_nac", "rx_tgid", "talkaround", "tx_dst", "unit_id")}
    old["unit_call"] = v.get("p25.unit_call") == "1"
    try:
        radio.link.p25_settings_update(rx_nac=0xF7E, rx_tgid=4321, talkaround="on")
        v = radio.wait_for(lambda x: x.get("p25.rx_tgid") == "4321", 2.0, sections=("p25",), what="settings taken")
        assert v.get("p25.rx_nac") == str(0xF7E) and v.get("p25.talkaround") == "1"
        assert v.get("p25.tx_nac") == str(old["tx_nac"]), "a field not given changed"
    finally:
        radio.link.p25_settings(**old)
    radio.wait_for(lambda x: x.get("p25.rx_tgid") == str(old["rx_tgid"]), 2.0, sections=("p25",),
                   what="settings put back")


PTT_S = 3.0
MAX_NID_ERRS = 3        # of the NID's 64 bits (the BCH code corrects 11)
MAX_CORRECTED = 0.005   # of the voice bits heard
DEV_PCT = (80, 115)     # the outer deviation, % of 1800 Hz: the bench board's discriminator scale is not calibrated
LDU_MS = 180.0


def p25_decode(job):
    """p25tool's JSON lines for an rx job's disc stream"""
    import p25tool
    pcm = job.pcm.get("disc")
    if not pcm:
        skip("bench", "the bench board sent no discriminator stream (the rx op's disc)")
    rate, full = None, None
    for st in (job.header or {}).get("streams", []):
        if st.get("name") == "disc":
            rate, full = st.get("rate", (job.header or {}).get("rate")), st.get("full")
    if not rate or not full:
        skip("bench", f"the bench board gave no rate or full scale for its disc stream: {job.header}")
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, "disc.raw")
        with open(raw, "wb") as f:
            f.write(pcm)
        try:
            return p25tool.run(["disc", raw, "--rate", int(round(float(rate))), "--full", full])
        except SystemExit as e:
            pytest.fail(f"p25tool: {e}")


@pytest.mark.feature("p25")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_p25_tx_off_air(radio, sdr_bench, pytestconfig):
    mhz = rig(pytestconfig, "p25-mhz")
    hz = round(mhz * 1e6)
    p25_ready(radio)
    if not sdr_bench.can_fm_rx():
        skip("bench", f"the bench board {sdr_bench.api_rx} has no FM receive")
    with radio.vfo_session():
        L = radio.link
        L.vfo_set(hz)
        radio.wait_for(lambda v: v.get("rx_hz") == str(hz), what="VFO tuned")
        if radio.core().get("mode") != "p25":
            L.dmr_fields(mode="p25")
        v = radio.wait_for(lambda x: x.get("mode") == "p25" and x.get("p25.on") == "1", 3.0,
                           sections=("core", "p25"), what="P25 running on the VFO")
        assert v.get("tx_hz") == str(hz), f"the VFO transmits on {v.get('tx_hz')}, not {hz}"
        radio.lowest_power()
        nac, tg, src = (int(v[f"p25.{k}"]) for k in ("tx_nac", "tx_dst", "unit_id"))
        if src == 0:        # no P25 unit ID set: the radio's DMR ID
            src = as_int(radio.view(["dmr"]).get("dmr.own_id")) or 0
        group = v.get("p25.unit_call") != "1"
        calls0, under0 = (as_int(v.get(f"p25.{k}")) or 0 for k in ("tx_calls", "tx_underruns"))
        with sdr_bench.lease():
            job = sdr_bench.fm_rx_start(mhz, PTT_S + 4, bw=16000, streams="disc")
            time.sleep(0.5)
            radio.ptt_for(PTT_S)
            radio.wait_for(lambda x: x.get("state") != "tx", timeout=3, what="TX ended")
            done = job.wait(PTT_S + 20)
        if done.get("gap_samples"):
            skip("bench", f"the bench board lost samples (gap_samples {done['gap_samples']})")
        v = radio.view(["p25"])
    lines = p25_decode(job)

    units = [d for d in lines if d["ev"] == "unit"]
    stats = next(d for d in lines if d["ev"] == "stats")
    syncs = [d for d in lines if d["ev"] == "sync" and "outer_hz" in d]
    names = [u["name"] for u in units]
    print(f"\np25 off air: {len(units)} units {' '.join(names)}")
    print(f"framer {stats['framer']}, call {stats['call']}")
    assert units, "nothing decoded"
    c = stats["call"]
    assert c["calls"] == 1, f"{c['calls']} calls"
    assert names[0] == "HDU", f"the call starts with {names[0]}, not a header"
    assert names[-1] in ("TDU", "TDULC"), f"the call ends with {names[-1]}, not a terminator"
    hdr = units[0].get("hdr")
    assert hdr is not None and hdr["algid"] == 0x80, f"header: {hdr}"
    if group:
        assert hdr["tgid"] == tg, f"header talk group {hdr['tgid']}, the radio's {tg}"
    for u in units:
        assert u["nac"] == nac, f"{u['name']} NAC {u['nac']:03X}, the radio's {nac:03X}"
        assert u["nid_errs"] <= MAX_NID_ERRS, f"{u['name']} NID with {u['nid_errs']} bit errors"
    ldus = [u for u in units if u["name"] in ("LDU1", "LDU2")]
    assert [u["name"] for u in ldus] == ["LDU1", "LDU2"] * (len(ldus) // 2) + ["LDU1"] * (len(ldus) % 2), \
        f"LDUs not in turn: {names}"
    for u in ldus:
        if u["name"] == "LDU1":
            lc = u.get("lc") or {}
            assert lc.get("src") == src, f"LDU1 source {lc.get('src')}, the radio's {src}"
            if group:
                assert lc.get("lcf") == 0 and lc.get("tgid") == tg, f"LDU1 link control {lc}"
            else:
                assert lc.get("lcf") == 3 and lc.get("dst") == tg, f"LDU1 link control {lc}"
        else:
            assert (u.get("es") or {}).get("algid") == 0x80, f"LDU2 encryption sync {u.get('es')}"
    gaps = [(b["t_us"] - a["t_us"]) / 1000 for a, b in zip(ldus, ldus[1:])]
    assert all(abs(g - LDU_MS) <= 0.5 for g in gaps), f"LDU spacing {min(gaps):.2f}-{max(gaps):.2f} ms"
    want = PTT_S * 1000 / LDU_MS
    assert abs(len(ldus) - want) <= 2, f"{len(ldus)} LDUs in a {PTT_S} s over (about {want:.0f})"
    assert c["voice_bad"] == 0, f"{c['voice_bad']} voice frames bad"
    share = c["voice_bits"] / max(1, c["voice"] * 144)
    print(f"voice frames {c['voice']}, {c['voice_bits']} bits corrected ({share:.3%})")
    assert share <= MAX_CORRECTED, f"{c['voice_bits']} bits corrected in {c['voice']} voice frames"
    assert c["lc_fail"] == 0 and c["es_fail"] == 0, f"LC fails {c['lc_fail']}, ES fails {c['es_fail']}"
    assert syncs, "the soft modem measured no level"
    outer = statistics.median(d["outer_hz"] for d in syncs)
    offset = statistics.median(d["offset_hz"] for d in syncs)
    print(f"outer deviation {outer:.0f} Hz ({outer / 18:.1f} % of 1800), carrier offset {offset:+.0f} Hz (not judged)")
    assert DEV_PCT[0] <= outer / 18 <= DEV_PCT[1], f"outer deviation {outer:.0f} Hz"
    assert (as_int(v.get("p25.tx_calls")) or 0) == calls0 + 1, "the radio counted no P25 call"
    assert (as_int(v.get("p25.tx_underruns")) or 0) == under0, "the air ran dry during the call"
