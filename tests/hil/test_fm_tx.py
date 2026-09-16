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
- test_fm_tx_tone_burst: TONE_BURST on for 2 s (radio-features.md 3.3): one keyed segment holding a 1750 Hz tone
  (+-10 Hz) for at least 1.5 s, deviation 500-3000 Hz peak at 12.5 kHz (sdr_fm_check.py --tones).
- test_fm_tx_roger_beep: roger_beep 1, a 1.5 s over: the segment ends with 1200 Hz then 900 Hz (+-15 Hz), each 60-160
  ms (nominally 100), the 900 Hz tone within 80 ms of the end of the carrier; the same over with roger_beep 0 ends with
  no tone.
- test_fm_tx_power_levels: 1.5 s overs at the three lowest power levels (DM-1701: 50, 250, 500 mW) captured at the
  SDR's lowest gain: each level's carrier is at least 1 dB stronger than the one below, and the lowest to the third
  differs by 5-15 dB (nominally 10). Relative only: the SDR isn't calibrated, and the radio is in its near field.
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
    out["spans"] = [(float(a), float(b)) for a, b in re.findall(r"^segment \d+: ([\d.]+)-([\d.]+) s", text, re.M)]
    out["levels"] = [float(x) for x in re.findall(r"^segment \d+: .*level ([+-]?[\d.]+) dBFS", text, re.M)]
    out["tones"] = [(float(f), float(a), float(b), int(d)) for f, a, b, d in
                    re.findall(r"tone ([\d.]+) Hz ([\d.]+)-([\d.]+) s \([\d.]+ s\), deviation (\d+) Hz", text)]
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


def key_and_check(radio, sdr_bench, pytestconfig, tag, bw_khz, ctcss=None, dcs=None, key=None, seconds=PTT_S + 5,
                  tones=False):
    """Capture while key() keys the radio (default: PTT for PTT_S), analyze, check one segment and the carrier."""
    mhz = pytestconfig.getoption("--fm-mhz")
    rdir = sdr_bench.run_dir(tag)
    with sdr_bench.lease():
        cap = sdr_bench.capture_start(rdir, mhz, seconds=seconds)
        if key is None:
            radio.ptt_for(PTT_S)
        else:
            key()
        radio.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
        sdr_bench.capture_wait()
    rc, text = sdr_bench.analyze_fm(cap, bw_khz=bw_khz, ctcss=ctcss, dcs=dcs, ppm=pytestconfig.getoption("--sdr-ppm"),
                                    freq=mhz, tones=tones)
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


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_fm_tx_tone_burst(fm_vfo, sdr_bench, pytestconfig):
    if "FM_TX_TONES" not in fm_vfo.features():
        pytest.skip("built without CONFIG_FM_TX_TONES")
    set_fm(fm_vfo, False, "none", 0)

    def burst():
        fm_vfo.link.tone_burst(True)
        try:
            fm_vfo.wait_for(lambda v: v.get("tone_burst") == "1", 1.0, sections=("features",), what="burst on")
            time.sleep(2.0)
        finally:
            fm_vfo.link.tone_burst(False)

    _, r = key_and_check(fm_vfo, sdr_bench, pytestconfig, "hil_fm_burst", 12.5, key=burst, seconds=6, tones=True)
    burst_runs = [t for t in r["tones"] if abs(t[0] - 1750.0) <= 10.0]
    print("tones:", r["tones"])
    assert burst_runs, f"no 1750 Hz tone on air: {r['tones']}"
    hz, t0, t1, dev = max(burst_runs, key=lambda t: t[2] - t[1])
    assert t1 - t0 >= 1.5, f"1750 Hz for only {t1 - t0:.2f} s of the 2 s burst"
    assert 500 <= dev <= 3000, f"1750 Hz burst deviation {dev} Hz peak at 12.5 kHz"


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_fm_tx_roger_beep(fm_vfo, sdr_bench, pytestconfig):
    if "FM_TX_TONES" not in fm_vfo.features():
        pytest.skip("built without CONFIG_FM_TX_TONES")
    from test_radio_features import settings
    set_fm(fm_vfo, False, "none", 0)

    def over():
        fm_vfo.link.ptt(True)
        try:
            fm_vfo.wait_for(lambda v: v.get("state") == "tx", 1.0, what="TX")
            time.sleep(1.5)
        finally:
            fm_vfo.link.ptt(False)

    results = {}
    for on in (1, 0):
        with settings(fm_vfo, roger_beep=on):
            _, r = key_and_check(fm_vfo, sdr_bench, pytestconfig, f"hil_fm_roger{on}", 12.5, key=over, seconds=5,
                                 tones=True)
        assert len(r["spans"]) == 1, f"roger_beep {on}: {len(r['spans'])} keyed segments: {r}"
        results[on] = r
        print(f"roger_beep {on}: segment {r['spans'][0]}, tones {r['tones']}")
    end = results[1]["spans"][0][1]
    tail = [t for t in results[1]["tones"] if t[2] >= end - 0.5]
    hi = [t for t in tail if abs(t[0] - 1200.0) <= 15.0]
    lo = [t for t in tail if abs(t[0] - 900.0) <= 15.0]
    assert hi and lo, f"no 1200 Hz + 900 Hz roger beep at the end of the over: {results[1]['tones']}"
    assert hi[-1][1] < lo[-1][1], f"900 Hz before 1200 Hz: {tail}"
    for name, t in (("1200 Hz", hi[-1]), ("900 Hz", lo[-1])):
        assert 0.06 <= t[2] - t[1] <= 0.16, f"{name} roger tone {t[2] - t[1]:.3f} s (nominal 0.100)"
    assert end - lo[-1][2] <= 0.08, f"the carrier stays {end - lo[-1][2]:.3f} s after the roger beep"
    end0 = results[0]["spans"][0][1]
    assert not [t for t in results[0]["tones"] if t[2] >= end0 - 0.5], \
        f"roger_beep 0 still ends with a tone: {results[0]['tones']}"


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_fm_tx_power_levels(fm_vfo, sdr_bench, pytestconfig):
    set_fm(fm_vfo, False, "none", 0)
    mhz = pytestconfig.getoption("--fm-mhz")
    levels = []
    for step in range(3):
        if step:
            fm_vfo.link.power_step(1)
            fm_vfo.wait_for(lambda v: v.get("power") == str(step), what=f"power {step}")
        name = fm_vfo.core().get("power_name")
        rdir = sdr_bench.run_dir(f"hil_fm_power{step}")
        with sdr_bench.lease():
            cap = sdr_bench.capture_start(rdir, mhz, seconds=4, gain=0)
            fm_vfo.ptt_for(1.5)
            fm_vfo.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
            sdr_bench.capture_wait()
        meta = sdr_bench.capture_meta(cap)
        _, text = sdr_bench.analyze_fm(cap, bw_khz=12.5)
        r = parse_fm(text)
        assert len(r["levels"]) == 1, f"power {step} ({name}): {len(r['levels'])} keyed segments: {text[-600:]}"
        levels.append((step, name, r["levels"][0], meta.get("clip_pct")))
    print("power levels (step, name, dBFS, ADC clip %):", levels)
    clipped = [x for x in levels if (x[3] or 0) > 0.5]
    assert not clipped, f"the SDR's ADC clipped at gain 0: levels can't be compared {clipped}"
    d = [levels[i + 1][2] - levels[i][2] for i in range(2)]
    assert all(x >= 1.0 for x in d), f"a higher power level is not stronger on air: steps {d} dB ({levels})"
    assert 5.0 <= levels[2][2] - levels[0][2] <= 15.0, \
        f"{levels[0][1]} to {levels[2][1]}: {levels[2][2] - levels[0][2]:.1f} dB (nominally 10) ({levels})"
