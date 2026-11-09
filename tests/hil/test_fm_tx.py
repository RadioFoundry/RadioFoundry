# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Analog FM transmit checked off air by the SDR bench (opt-in: --allow-rf --allow-service-stop; the radio and the
bench's receive board only, the bench never transmits here).

The radio's FM VFO is tuned to --fm-mhz (the rig's FM test frequency) on its lowest power, with a
sub-audio tone and a bandwidth set through debug-link radio requests; it keys for 3 s with no audio while the receive
board demodulates the over itself (the line protocol's rx op: SdrBench.fm_over_start, fm_measure.py): its summary gives
the carrier offset, deviation and CTCSS or DCS decode, its 10 ms blocks the keyed segments, its digit records the DTMF.
Timed audio tones (the 1750 Hz burst, the roger beep) are found on the test host in the board's demodulated audio
stream. A bench without the rx op skips; samples the board lost (gap_samples) skip as a bench fault.

Pass criteria:
- test_fm_tx_ctcss: at least one keyed segment; CTCSS 100.0 Hz decoded within 0.5 Hz; tone deviation 200-900 Hz at
  12.5 kHz; |carrier offset| < --max-foff Hz (default 500; the board takes its crystal out itself and the limit adds
  its correction's tolerance at the frequency, freq_tol_hz).
- test_fm_tx_dcs: DCS D023N decoded with the right polarity (an inverted code fails); deviation 150-900 Hz at 12.5 kHz;
  the board heard the 134.4 Hz turn-off tail after PTT release (its analysis needs about 125 ms of it, so this no
  longer times the tail).
- test_fm_tx_bandwidth: the same CTCSS 100.0 Hz tone at 12.5 and at 25 kHz: the tone deviation at 25 kHz is 1.5 to
  2.6 times the one at 12.5 kHz (the radio scales every deviation with the channel bandwidth; nominally 2).
- test_fm_tx_tone_burst: TONE_BURST on for 2 s (radio-features.md 3.3): one keyed segment holding a 1750 Hz tone
  (+-10 Hz) for at least 1.5 s, deviation 500-2500 Hz peak at 12.5 kHz (the narrow band limit).
- test_fm_tx_roger_beep: roger_beep 1, a 1.5 s over: the segment ends with 1200 Hz then 900 Hz (+-15 Hz), each 60-160
  ms (nominally 100), the 900 Hz tone within 80 ms of the end of the carrier; the same over with roger_beep 0 ends with
  no tone.
- test_fm_tx_dtmf: PTT held, DTMF 1 # 5 D sent by the debug link (150 ms each) and 0 as a 20 ms tap
  (radio-features.md 3.7), at 12.5 and 25 kHz: one keyed segment; the digits decode in order;
  each at least 80 ms on air (the 20 ms tap too), at least 40 ms apart, deviation 300 Hz to 2.5 kHz (12.5 kHz) or
  5 kHz (25 kHz) peak, twist (high tone over low, after a receiver's 750 us de-emphasis, worked out from the twist the
  board reports on the carrier) -8 to +4 dB; the view's dtmf_digits and dtmf_sent agree.
- test_fm_tx_dtmf_stop: a digit held down when PTT is released, then when a 3 s time-out ends the transmission: one
  keyed segment, the digit decoded for at least 0.8 s (breaks up to 150 ms allowed: the board can lose a weak digit)
  and ending within 0.15 s of the carrier, the view's dtmf back to '-' at once, one digit counted (the key's late
  release sends nothing).
- test_fm_tx_power_levels: 1.5 s overs at the three lowest power levels: each level's carrier, as the bench board
  reads it, is at least 1 dB stronger than the one below. Order only: the board's level isn't linear near its
  quantization floor, so how far apart the levels are is the wattmeter's check below.
  With --manual -s it reads a wattmeter instead (docs/testing/power-calibration.md 4, the short check): a person
  with an averaging wattmeter between the radio and a dummy load rated for the top level. Every power level keys 2 s
  on each --wattmeter-mhz frequency (default 445 and 146 MHz, the middle of where hams transmit on 70 cm and 2 m)
  that the radio tunes and tx_mhz allows, 4 s apart, in one batch; the person notes each reading and types them all
  afterwards. Each level named in watts must read within 1 dB of its name (2 dB under 1 W), and each level must
  read at least 1 dB over the one below.
The VFO's bandwidth and TX tone are put back afterwards (restore_radio does the rest: channel, power, temporary
changes).
"""
import math
import time

import pytest

from rfoundry_debug import DebugLinkError
import fm_measure
from rfoundry_hil import power_name_watts, skip
from hil_config import rig

PTT_S = 3.0


@pytest.fixture
def fm_vfo(radio, pytestconfig):
    """The radio's FM VFO on --fm-mhz, lowest power; the VFO's bandwidth and TX tone restored afterwards."""
    if "VFO_FM" not in radio.features():
        skip("not_built", "built without the FM VFO")
    if "SUBTONE" not in radio.features():
        skip("not_built", "built without CTCSS / DCS")
    hz = round(rig(pytestconfig, "fm-mhz") * 1e6)
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
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


def key_and_check(radio, sdr_bench, pytestconfig, bw_khz, ctcss=None, dcs=None, key=None, seconds=PTT_S + 5,
                  tones=False, dtmf=False):
    """The board measures while key() keys the radio (default: PTT for PTT_S); one segment and the carrier checked.
    Returns fm_measure's report of the over."""
    mhz = rig(pytestconfig, "fm-mhz")
    with sdr_bench.lease():
        job = sdr_bench.fm_over_start(mhz, seconds, bw_khz=bw_khz, audio=tones, dtmf=dtmf)
        if key is None:
            radio.ptt_for(PTT_S)
        else:
            key()
        radio.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
        r = sdr_bench.fm_over_result(job)
    print(fm_measure.describe(r))
    assert r["segments"] >= 1, f"no keyed segment: the board heard {r['carrier_s']} s of carrier: {r}"
    foff = r["carrier_hz"]
    max_foff = pytestconfig.getoption("--max-foff")
    # The board says how far its own crystal correction may be off at the frequency: the limit allows for it
    tol = r.get("freq_tol_hz") or 0.0
    print(f"bench board's correction: {r.get('ppm_source')}, +-{r.get('freq_tol_hz')} Hz at {mhz} MHz")
    assert foff is not None and abs(foff) < max_foff + tol, \
        f"carrier offset {foff} Hz (limit {max_foff} + the bench's {tol} Hz)"
    if ctcss is not None:
        assert abs((r.get("ctcss_hz") or 0.0) - ctcss) <= 0.5, f"CTCSS {ctcss} Hz not decoded: {r['sub']}"
    if dcs is not None:
        assert dcs.upper() in r.get("dcs_codes", []), f"DCS {dcs} not decoded (or inverted): {r['sub']}"
    return r


@pytest.mark.feature("subtone")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_fm_tx_ctcss(fm_vfo, sdr_bench, pytestconfig):
    set_fm(fm_vfo, False, "ctcss", 1000)
    r = key_and_check(fm_vfo, sdr_bench, pytestconfig, 12.5, ctcss=100.0)
    assert 200 <= r["ctcss_dev_hz"] <= 900, f"CTCSS deviation {r['ctcss_dev_hz']} Hz at 12.5 kHz: {r}"


@pytest.mark.feature("subtone")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_fm_tx_dcs(fm_vfo, sdr_bench, pytestconfig):
    set_fm(fm_vfo, False, "dcs", 0o23)
    r = key_and_check(fm_vfo, sdr_bench, pytestconfig, 12.5, dcs="023N")
    # The radios send 210-350 Hz; a weak DCS (the HR_C7000's sub-audio field once sent 100 Hz) still decodes on the SDR
    assert 150 <= r["dcs_dev_hz"] <= 900, f"DCS deviation +-{r['dcs_dev_hz']} Hz at 12.5 kHz: {r}"
    # The turn-off tail after PTT release: 134.4 Hz for about 200-300 ms (the core holds the carrier 300 ms). The board
    # says whether it heard the tone (for 125 ms or more), not how long
    assert r["dcs_turnoff"], f"no DCS turn-off tail (134.4 Hz) at the end: {r['sub']}"


@pytest.mark.feature("fm")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_fm_tx_bandwidth(fm_vfo, sdr_bench, pytestconfig):
    devs = {}
    for wide, bw in ((False, 12.5), (True, 25.0)):
        set_fm(fm_vfo, wide, "ctcss", 1000)
        r = key_and_check(fm_vfo, sdr_bench, pytestconfig, bw, ctcss=100.0)
        assert "ctcss_dev_hz" in r, f"no CTCSS decoded at {bw} kHz: {r}"
        devs[bw] = r["ctcss_dev_hz"]
    ratio = devs[25.0] / max(1, devs[12.5])
    print(f"CTCSS deviation: 12.5 kHz {devs[12.5]} Hz, 25 kHz {devs[25.0]} Hz, ratio {ratio:.2f}")
    assert 1.5 <= ratio <= 2.6, f"25 / 12.5 kHz deviation ratio {ratio:.2f} (tones {devs})"


@pytest.mark.feature("fm_tx_tones")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_fm_tx_tone_burst(fm_vfo, sdr_bench, pytestconfig):
    if "FM_TX_TONES" not in fm_vfo.features():
        skip("not_built", "built without CONFIG_FM_TX_TONES")
    set_fm(fm_vfo, False, "none", 0)

    def burst():
        fm_vfo.link.tone_burst(True, 4000)
        try:
            fm_vfo.wait_for(lambda v: v.get("tone_burst") == "1", 1.0, sections=("features",), what="burst on")
            time.sleep(2.0)
        finally:
            fm_vfo.link.tone_burst(False)

    r = key_and_check(fm_vfo, sdr_bench, pytestconfig, 12.5, key=burst, seconds=6, tones=True)
    burst_runs = [t for t in r["tones"] if abs(t[0] - 1750.0) <= 10.0]
    print("tones:", r["tones"])
    assert burst_runs, f"no 1750 Hz tone on air: {r['tones']}"
    hz, t0, t1, dev = max(burst_runs, key=lambda t: t[2] - t[1])
    assert t1 - t0 >= 1.5, f"1750 Hz for only {t1 - t0:.2f} s of the 2 s burst"
    assert 500 <= dev <= 2500, f"1750 Hz burst deviation {dev} Hz peak at 12.5 kHz (at most 2500)"


@pytest.mark.feature("fm_tx_tones")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_fm_tx_roger_beep(fm_vfo, sdr_bench, pytestconfig):
    if "FM_TX_TONES" not in fm_vfo.features():
        skip("not_built", "built without CONFIG_FM_TX_TONES")
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
            r = key_and_check(fm_vfo, sdr_bench, pytestconfig, 12.5, key=over, seconds=5, tones=True)
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
        # rounded to the 10 ms steps of the tone analysis: 4.71 - 4.55 is 0.16000000000000014 and must still pass
        d = round(t[2] - t[1], 3)
        assert 0.06 <= d <= 0.16, f"{name} roger tone {d:.3f} s (nominal 0.100)"
    assert end - lo[-1][2] <= 0.08, f"the carrier stays {end - lo[-1][2]:.3f} s after the roger beep"
    end0 = results[0]["spans"][0][1]
    assert not [t for t in results[0]["tones"] if t[2] >= end0 - 0.5], \
        f"roger_beep 0 still ends with a tone: {results[0]['tones']}"


HELD_BREAK_S = 0.035    # a held digit's longest break: a decoder takes 40 ms of silence as the end of a digit
HELD_SNR_DB = 20.0      # the carrier over the noise from which the bench board hears every break as it is
DTMF_SENT = "1#5D0"     # the last a 20 ms tap: the radio holds it to its shortest tone


@pytest.mark.feature("dtmf")
@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_fm_tx_dtmf(fm_vfo, sdr_bench, pytestconfig):
    if "DTMF" not in fm_vfo.features():
        skip("not_built", "built without CONFIG_DTMF")
    L = fm_vfo.link

    def over():
        L.ptt(True)
        try:
            fm_vfo.wait_for(lambda v: v.get("state") == "tx" and v.get("dtmf_ready") == "1", 1.0,
                            sections=("core", "features"), what="TX, DTMF ready")
            time.sleep(0.5)
            L.dtmf(DTMF_SENT[:-1], ms=150, gap=150)
            L.dtmf(DTMF_SENT[-1], ms=20, gap=300)
            v = fm_vfo.view(["features"])
            assert v.get("dtmf_digits") == DTMF_SENT, f"the radio core sent {v.get('dtmf_digits')}"
        finally:
            L.ptt(False)

    for wide in (False, True):
        bw = 25.0 if wide else 12.5
        set_fm(fm_vfo, wide, "none", 0)
        sent0 = int(fm_vfo.view(["features"]).get("dtmf_sent", "0"))
        r = key_and_check(fm_vfo, sdr_bench, pytestconfig, bw, key=over, seconds=6, dtmf=True)
        print(f"{bw} kHz dtmf:", r["dtmf"])
        assert int(fm_vfo.view(["features"]).get("dtmf_sent", "0")) == sent0 + len(DTMF_SENT)
        assert len(r["spans"]) == 1, f"{len(r['spans'])} keyed segments: DTMF must not drop the carrier"
        got = "".join(d[0] for d in r["dtmf"])
        assert got == DTMF_SENT, f"{bw} kHz: decoded {got!r}, sent {DTMF_SENT!r}: {r['dtmf']}"
        for d, t0, t1, dev, twist, rx_twist in r["dtmf"]:
            assert t1 - t0 >= 0.08, f"{d}: {t1 - t0:.3f} s on air (shortest tone 0.100)"
            assert 300 <= dev <= (2500 if bw == 12.5 else 5000), f"{d}: deviation {dev} Hz peak at {bw} kHz"
            # The DTMF receiver limits (high tone -8 to +4 dB from the low one) apply to the audio a receiver hears,
            # after its de-emphasis; on the carrier a tone sent through the pre-emphasis reads 3-7 dB higher
            assert -8.0 <= rx_twist <= 4.0, f"{d}: twist {rx_twist:+.1f} dB received ({twist:+.1f} dB on the carrier)"
        for a, b in zip(r["dtmf"], r["dtmf"][1:]):
            assert b[1] - a[2] >= 0.04, f"{a[0]} to {b[0]}: {b[1] - a[2]:.3f} s apart"


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_fm_tx_dtmf_stop(fm_vfo, sdr_bench, pytestconfig):
    """A digit held down when the transmission ends (PTT released, then the time-out): the tone ends with the
    carrier, the view's dtmf goes back to '-', and the key's late release sends nothing."""
    if "DTMF" not in fm_vfo.features():
        skip("not_built", "built without CONFIG_DTMF")
    from test_radio_features import settings
    L = fm_vfo.link
    set_fm(fm_vfo, False, "none", 0)

    def held(digit, end):
        def over():
            L.ptt(True)
            try:
                fm_vfo.wait_for(lambda v: v.get("state") == "tx" and v.get("dtmf_ready") == "1", 1.0,
                                sections=("core", "features"), what="TX, DTMF ready")
                time.sleep(0.5)
                L.dtmf_key(digit, True)
                time.sleep(0.3)
                assert fm_vfo.view(["features"]).get("dtmf") == digit
                end()
                v = fm_vfo.view(["core", "features"])
                assert v.get("state") != "tx" and v.get("dtmf") is None, f"after the end of TX: {v}"
                L.dtmf_key(digit, False)
            finally:
                L.ptt(False)
        return over

    def release():
        time.sleep(0.7)
        L.ptt(False)
        fm_vfo.wait_for(lambda v: v.get("state") != "tx", timeout=2, what="TX ended")

    def timeout():
        fm_vfo.wait_for(lambda v: v.get("state") != "tx", timeout=5, what="the time-out")
        assert fm_vfo.core().get("tx_deny") == "timeout"

    for name, digit, end, extra in (("ptt", "7", release, {}), ("tot", "9", timeout, {"tot_s": 3, "tot_warn_s": 0})):
        sent0 = int(fm_vfo.view(["features"]).get("dtmf_sent", "0"))
        with settings(fm_vfo, **extra):
            r = key_and_check(fm_vfo, sdr_bench, pytestconfig, 12.5, key=held(digit, end), seconds=7, dtmf=True)
        print(f"{name}: segment {r['spans']}, dtmf {r['dtmf']}")
        assert len(r["spans"]) == 1, f"{name}: {len(r['spans'])} keyed segments"
        # One key held down. A break of 40 ms or more would read as a repeated digit. A weak signal loses the digit
        # for longer than the radio stopped it, so only a strong one judges the breaks
        assert r["dtmf"] and {d[0] for d in r["dtmf"]} == {digit}, f"{name}: decoded {r['dtmf']}"
        snr = None if r["power_dbfs"] is None or r["noise_dbfs"] is None else r["power_dbfs"] - r["noise_dbfs"]
        if snr is not None and snr >= HELD_SNR_DB:
            for a, b in zip(r["dtmf"], r["dtmf"][1:]):
                assert b[1] - a[2] <= HELD_BREAK_S, f"{name}: the held digit stopped for {b[1] - a[2]:.3f} s: {r['dtmf']}"
        else:
            print(f"{name}: carrier {snr} dB over the noise, under {HELD_SNR_DB:.0f}: breaks not judged")
        seg_end, tone_end = r["spans"][0][1], r["dtmf"][-1][2]
        assert tone_end - r["dtmf"][0][1] >= 0.8, f"{name}: the held digit lasted {r['dtmf']}"
        assert abs(seg_end - tone_end) <= 0.15, f"{name}: tone ends {tone_end:.3f} s, carrier {seg_end:.3f} s"
        assert int(fm_vfo.view(["features"]).get("dtmf_sent", "0")) == sent0 + 1


def _ask_watts(prompt):
    while True:
        try:
            w = float(input(f"\n>>> {prompt}: watts on the meter? ").strip())
            if w >= 0:
                return w
        except ValueError:
            pass
        print("    a number in watts")


def _power_levels_wattmeter(radio, pytestconfig):
    """The short check on a wattmeter (power-calibration.md 4): every level keyed 2 s on each frequency, all of them
    in one batch, the readings typed afterwards"""
    import hil_config
    plan = []
    for mhz in hil_config.wattmeter_mhz(pytestconfig):
        if not hil_config.tx_allowed(None, mhz):
            print(f"    {mhz:g} MHz: not in tx_mhz, left out (add it to the rig's tx_mhz to check it)")
            continue
        hz = round(mhz * 1e6)
        try:
            radio.link.vfo_set(hz)
            radio.wait_for(lambda v: v.get("rx_hz") == str(hz), timeout=3, what=f"VFO on {mhz} MHz")
        except (AssertionError, DebugLinkError):
            print(f"    {mhz:g} MHz: the radio doesn't tune it, left out")
            continue
        n = int(radio.core().get("power_count") or "0")
        assert n >= 2, f"power_count {n}"
        names = (radio.core().get("power_names") or "").split("/")
        plan += [(mhz, lv, names[lv] if lv < len(names) else "?") for lv in range(n)]
    if not plan:
        skip("opt_in", f"none of the wattmeter frequencies {hil_config.wattmeter_mhz(pytestconfig)} MHz is both "
                       "tuned by the radio and in tx_mhz (--wattmeter-mhz)")
    print("\n>>> A wattmeter (averaging) between the radio and a dummy load rated for its top power. In one batch, "
          "4 s apart, each key-up 2 s:", flush=True)
    for i, (mhz, lv, name) in enumerate(plan):
        print(f">>>   {i + 1:2}. {mhz:g} MHz, level {lv} ({name})", flush=True)
    # Nothing keys until the person says the meter is on this radio; the readings are asked for after the batch
    input(f"\n>>> Press Enter when the wattmeter is on {radio.version().get('board', 'the radio')} and ready: ")
    for i, (mhz, lv, name) in enumerate(plan):
        hz = round(mhz * 1e6)
        if radio.core().get("rx_hz") != str(hz):
            radio.link.vfo_set(hz)
            radio.wait_for(lambda v: v.get("rx_hz") == str(hz), what=f"VFO on {mhz} MHz")
        if lv == 0:
            radio.lowest_power(absolute=True)
        else:
            radio.link.power_step(1)
            radio.wait_for(lambda v: v.get("power") == str(lv), what=f"power {lv}")
        if i:
            time.sleep(4.0)     # between key-ups: the PA and the supply recover, the person notes the reading
        print(f">>>   {i + 1:2}. keying {mhz:g} MHz, level {lv} ({name})", flush=True)
        radio.ptt_for(2.0)
        radio.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
    print("\n>>> Done keying. The readings, in order:", flush=True)
    rows, bad, prev = [], [], {}
    for i, (mhz, lv, name) in enumerate(plan):
        w = _ask_watts(f"{i + 1:2}. {mhz:g} MHz, level {lv} ({name})")
        want = power_name_watts(name)
        d = 10 * math.log10(w / want) if want and w > 0 else None
        tol = 2.0 if want is not None and want < 1.0 else 1.0
        ok = want is None or (d is not None and abs(d) <= tol)
        p = prev.get(mhz)
        if p is not None and not (w > 0 and p > 0 and 10 * math.log10(w / p) >= 1.0):
            ok = False
        rows.append((mhz, lv, name, w, None if d is None else round(d, 1)))
        if not ok:
            bad.append(rows[-1])
        prev[mhz] = w
    print("power levels (MHz, level, name, W, dB from the name):", rows)
    assert not bad, f"out of tolerance (within 1 dB of the name, 2 dB under 1 W; 1 dB over the level below): {bad}"


@pytest.mark.feature("fm")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_fm_tx_power_levels(fm_vfo, sdr_bench, pytestconfig):
    set_fm(fm_vfo, False, "none", 0)
    if pytestconfig.getoption("--manual"):
        _power_levels_wattmeter(fm_vfo, pytestconfig)
        return
    mhz = rig(pytestconfig, "fm-mhz")
    levels = []
    # The first three levels, or every level of a radio with fewer (board_t.power_count)
    count = min(3, int(fm_vfo.core().get("power_count") or 3))
    if count < 2:
        skip("no_hw", "one power level: nothing to compare")
    # Each level set as such: the run may start the radio on any level (--radio-tx-watts), and a step from there
    # wraps past the highest
    start = int(fm_vfo.core()["power"])
    try:
        for step in range(count):
            power, name = fm_vfo.link.power_level(step)
            assert int(power) == step, (power, name)
            with sdr_bench.lease():
                job = sdr_bench.fm_over_start(mhz, 4, bw_khz=12.5)
                fm_vfo.ptt_for(1.5)
                fm_vfo.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
                r = sdr_bench.fm_over_result(job)
            print(fm_measure.describe(r))
            assert len(r["levels"]) == 1, f"power {step} ({name}): {len(r['levels'])} keyed segments: {r}"
            levels.append((step, name, r["levels"][0]))
    finally:
        fm_vfo.link.power_level(start)
    print("power levels (step, name, dBFS):", levels)
    d = [levels[i + 1][2] - levels[i][2] for i in range(count - 1)]
    assert all(x >= 1.0 for x in d), f"a higher power level is not stronger on air: steps {d} dB ({levels})"
    # Order only: a bench board's level isn't linear this close to its quantization floor (10 dB less gain moved a
    # reading 5 dB), so the span between levels is the wattmeter's check (--manual)

