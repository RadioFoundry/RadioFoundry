# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Analog FM transmit checked off air by the SDR bench (opt-in: --allow-rf --allow-service-stop; the radio and the
bench's RTL-SDR only, the bench never transmits here).

The radio's FM VFO is tuned to --fm-mhz (default the DMR test channel, 461.6875 MHz) on its lowest power, with a
sub-audio tone and a bandwidth set through debug-link radio requests; it keys for 3 s with no audio while the bench
records raw I/Q (sdrmon.py capture), and tools/sdr_fm_check.py analyzes the over on the bench.

Pass criteria:
- test_fm_tx_ctcss: one keyed segment; CTCSS 100.0 Hz decoded within 0.5 Hz (sdr_fm_check --expect-ctcss PASS);
  tone deviation 200-900 Hz at 12.5 kHz; |carrier offset| < --max-foff Hz (default 500; corrected by --sdr-ppm).
- test_fm_tx_dcs: DCS D023N decoded with the right polarity (--expect-dcs 023N PASS; an inverted code fails).
- test_fm_tx_bandwidth: the same CTCSS 100.0 Hz tone at 12.5 and at 25 kHz: the tone deviation at 25 kHz is 1.5 to
  2.6 times the one at 12.5 kHz (the radio scales every deviation with the channel bandwidth; nominally 2).
The VFO's bandwidth and TX tone are put back afterwards (restore_radio does the rest: channel, power, temporary
changes).
"""
import re
import time

import pytest

PTT_S = 3.0


def parse_fm(text):
    """The first keyed segment of sdr_fm_check.py's report: carrier Hz, CTCSS Hz / deviation, DCS names, verdicts."""
    out = {"segments": len(re.findall(r"^segment \d+:", text, re.M))}
    m = re.search(r"carrier ([+-]\d+) Hz", text)
    if m:
        out["carrier_hz"] = int(m.group(1))
    m = re.search(r"corrected ([+-]\d+) Hz", text)
    if m:
        out["carrier_corrected_hz"] = int(m.group(1))
    m = re.search(r"sub-audio: CTCSS ([\d.]+) Hz .*?deviation (\d+) Hz peak", text)
    if m:
        out["ctcss_hz"], out["ctcss_dev_hz"] = float(m.group(1)), int(m.group(2))
    m = re.search(r"sub-audio: DCS (\S+), deviation \+-(\d+) Hz", text)
    if m:
        out["dcs"], out["dcs_dev_hz"] = m.group(1), int(m.group(2))
    out["expect"] = re.findall(r"expect (?:CTCSS|DCS) [^:]+: (PASS|FAIL[^\n]*)", text)
    return out


@pytest.fixture
def fm_vfo(radio, pytestconfig):
    """The radio's FM VFO on --fm-mhz, lowest power; the VFO's bandwidth and TX tone restored afterwards."""
    if "VFO_FM" not in radio.features():
        pytest.skip("built without the FM VFO")
    if "SUBTONE" not in radio.features():
        pytest.skip("built without CTCSS / DCS")
    hz = round(pytestconfig.getoption("--fm-mhz") * 1e6)
    radio.go_home()
    if radio.screen() == "hotspot":
        pytest.skip("the radio is in hotspot mode")
    with radio.vfo_session():
        L = radio.link
        ch = radio.view(["channel"])
        bw0, tone0 = ch["ch.bw"], ch["ch.tx_tone"]
        L.vfo_set(hz)
        radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="FM VFO tuned")
        radio.lowest_power()
        try:
            yield radio
        finally:
            L.ptt(False)
            kind, value = tone0.split(":")
            L.tone(1, kind, int(value))
            L.bandwidth(bw0 == "25k")
            radio.wait_for(lambda v: v.get("ch.bw") == bw0 and v.get("ch.tx_tone") == tone0, sections=("channel",),
                           what="VFO bandwidth and TX tone restored")


def set_fm(radio, wide, kind, value):
    radio.link.bandwidth(wide)
    time.sleep(0.05)
    radio.link.tone(1, kind, value)
    want_bw, want_tone = ("25k" if wide else "12k5"), f"{kind}:{value}"
    radio.wait_for(lambda v: v.get("ch.bw") == want_bw and v.get("ch.tx_tone") == want_tone, sections=("channel",),
                   what=f"bandwidth {want_bw}, TX tone {want_tone}")


def key_and_check(radio, sdr_bench, pytestconfig, tag, bw_khz, ctcss=None, dcs=None):
    mhz = pytestconfig.getoption("--fm-mhz")
    rdir = sdr_bench.run_dir(tag)
    with sdr_bench.lease():
        cap = sdr_bench.capture_start(rdir, mhz, seconds=PTT_S + 5)
        radio.ptt_for(PTT_S)
        radio.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
        sdr_bench.capture_wait()
    rc, text = sdr_bench.analyze_fm(cap, bw_khz=bw_khz, ctcss=ctcss, dcs=dcs, ppm=pytestconfig.getoption("--sdr-ppm"),
                                    freq=mhz)
    print(text)
    r = parse_fm(text)
    assert r["segments"] >= 1, f"no keyed segment in the capture (~/{sdr_bench.dir}/{rdir}/): {text[-800:]}"
    foff = r.get("carrier_corrected_hz", r.get("carrier_hz"))
    max_foff = pytestconfig.getoption("--max-foff")
    assert foff is not None and abs(foff) < max_foff, f"carrier offset {foff} Hz (limit {max_foff})"
    return rc, r


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_fm_tx_ctcss(fm_vfo, sdr_bench, pytestconfig):
    set_fm(fm_vfo, False, "ctcss", 1000)
    rc, r = key_and_check(fm_vfo, sdr_bench, pytestconfig, "hil_fm_ctcss", 12.5, ctcss=100.0)
    assert r["expect"] and r["expect"][0] == "PASS", f"CTCSS 100.0 Hz not decoded: {r}"
    assert 200 <= r["ctcss_dev_hz"] <= 900, f"CTCSS deviation {r['ctcss_dev_hz']} Hz at 12.5 kHz: {r}"


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_fm_tx_dcs(fm_vfo, sdr_bench, pytestconfig):
    set_fm(fm_vfo, False, "dcs", 0o23)
    rc, r = key_and_check(fm_vfo, sdr_bench, pytestconfig, "hil_fm_dcs", 12.5, dcs="023N")
    assert r["expect"] and r["expect"][0] == "PASS", f"DCS D023N not decoded (or inverted): {r}"


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_fm_tx_bandwidth(fm_vfo, sdr_bench, pytestconfig):
    devs = {}
    for wide, bw in ((False, 12.5), (True, 25.0)):
        set_fm(fm_vfo, wide, "ctcss", 1000)
        _, r = key_and_check(fm_vfo, sdr_bench, pytestconfig, f"hil_fm_bw{bw:g}", bw, ctcss=100.0)
        assert "ctcss_dev_hz" in r, f"no CTCSS decoded at {bw} kHz: {r}"
        devs[bw] = r["ctcss_dev_hz"]
    ratio = devs[25.0] / max(1, devs[12.5])
    print(f"CTCSS deviation: 12.5 kHz {devs[12.5]} Hz, 25 kHz {devs[25.0]} Hz, ratio {ratio:.2f}")
    assert 1.5 <= ratio <= 2.6, f"25 / 12.5 kHz deviation ratio {ratio:.2f} (tones {devs})"
