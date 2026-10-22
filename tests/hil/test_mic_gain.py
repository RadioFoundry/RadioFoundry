# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""FM microphone gain against deviation, measured off air by the SDR bench (radio-features.md 3.6). Manual and opt-in:
--manual -s --allow-rf, a person at the radio.

No audio source can stand in for the microphone here: the tones the HIL tools can send (the TX tone generator, the
prompt FIFO, the PCM stream) all join the audio path after the codec ADC, so they never pass through the mic gain.
On the DM-1801 Pro the programming cable (the debug link) also cuts the microphone while it is plugged in. So each
over goes like this:

1. With the cable in, the test sets the FM VFO on --fm-mhz (lowest power, no sub-audio), the bandwidth and the Mic
   gain step (the debug link's SETTING op, RAM only), and prints which over is next.
2. The SDR bench starts a --mic-gain-window recording (default 30 s) and the test prints "GO".
3. The person unplugs the cable, keys PTT, gives a steady tone (a whistle or a tone from a phone held at the
   microphone, the same distance and level every time) or reads the same sentence for about 5 s, releases PTT and
   plugs the cable back in.
4. The test analyzes the recording (tools/sdr_fm_check.py: the voice band peak and rms deviation of the longest keyed
   segment, and the strongest tone), waits for the debug link to answer again (up to 3 min) and reads the step back.

One over per step and bandwidth: --mic-gain-steps (default 3,5,7) at 12.5 kHz, then the same at 25 kHz. At the end
the test prints a table: bandwidth, step, offset (dB), the modem's GCR_ADCL after the over (DM-1801 Pro diagnostics,
"cd 20" line), segment length, voice peak (99.5 %) and rms, tone and its deviation.

Pass: every over has a keyed segment of at least 1 s, and the debug link reads back the step that was set. The
deviation numbers are for tuning the board's FM levels (nominal peak 2.5 kHz at 12.5 kHz, 5 kHz at 25 kHz), so they
are printed, not checked. Skipped when the board has no mic gain hook (set.mic_gain_board=0). The step and the VFO
are put back afterwards.
"""
import re
import time

import pytest

from odr_debug import DebugLink, DebugLinkError, as_int
from test_fm_tx import fm_vfo  # noqa: F401  (the fixture)

LINK_BACK_S = 180.0
MIN_SEGMENT_S = 1.0


def features(radio):
    return radio.link.view(["features"])


def wait_link(radio, timeout=LINK_BACK_S):
    """The cable is back: the debug link answers again (reopened if the port went away)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            radio.ping()
            return
        except (DebugLinkError, OSError):
            try:
                radio.link.close()
            except (DebugLinkError, OSError):
                pass
            try:
                radio.link = DebugLink(radio.port, timeout=1.5)
            except (DebugLinkError, OSError):
                pass
            time.sleep(1.0)
    raise AssertionError(f"the debug link didn't answer within {timeout:.0f} s: is the cable back in?")


def adcl(radio):
    """GCR_ADCL (codec register 0x25) from the board's codec dump in the diagnostics ("cd 20 ..."), or None."""
    try:
        _, lines = radio.link.diag()
    except DebugLinkError:
        return None
    for ln in lines:
        m = re.match(r"\s*cd 20 ([0-9a-fA-F]{32})", ln)
        if m:
            return int(m.group(1)[10:12], 16)
    return None


def segments(text):
    """Per keyed segment of sdr_fm_check.py's report: length, voice peak / rms, tones (Hz, deviation)."""
    out = []
    for block in re.split(r"\n(?=segment \d+:)", text):
        m = re.match(r"segment \d+: ([\d.]+)-([\d.]+) s", block)
        if not m:
            continue
        seg = {"start": float(m.group(1)), "len": float(m.group(2)) - float(m.group(1))}
        v = re.search(r"voice 300-3000 Hz: peak (\d+) Hz \(99\.5%\), (\d+) Hz \(99\.9%\), rms (\d+) Hz", block)
        if v:
            seg["peak"], seg["peak999"], seg["rms"] = int(v.group(1)), int(v.group(2)), int(v.group(3))
        seg["tones"] = [(float(f), int(d)) for f, d in
                        re.findall(r"tone ([\d.]+) Hz [\d.]+-[\d.]+ s \([\d.]+ s\), deviation (\d+) Hz", block)]
        out.append(seg)
    return out


@pytest.mark.manual
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_mic_gain_deviation(fm_vfo, sdr_bench, pytestconfig):
    radio = fm_vfo
    L = radio.link
    f = features(radio)
    if "set.mic_gain" not in f:
        pytest.skip("firmware without the mic gain view keys")
    if f.get("set.mic_gain_board") != "1":
        pytest.skip("this board has no mic gain in its audio path (hal_radio_t.mic_gain): FM isn't changed")
    steps = [int(s) for s in pytestconfig.getoption("--mic-gain-steps").split(",") if s.strip()]
    assert steps and all(0 <= s <= 10 for s in steps), f"--mic-gain-steps out of 0-10: {steps}"
    window = pytestconfig.getoption("--mic-gain-window")
    mhz = pytestconfig.getoption("--fm-mhz")
    step0 = as_int(f["set.mic_gain"], 5)
    overs = [(wide, s) for wide in (False, True) for s in steps]
    rows = []
    try:
        for n, (wide, step) in enumerate(overs, 1):
            bw_khz = 25.0 if wide else 12.5
            L.bandwidth(wide)
            time.sleep(0.06)
            L.tone(1, "none", 0)
            time.sleep(0.06)
            L.setting("mic_gain", step)
            radio.wait_for(lambda v: v.get("ch.bw") == ("25k" if wide else "12k5"), sections=("channel",),
                           what="bandwidth")
            radio.wait_for(lambda v: v.get("set.mic_gain") == str(step), sections=("features",), what="mic gain step")
            print(f"\n=== over {n}/{len(overs)}: {bw_khz:g} kHz, mic gain step {step} ({(step - 5) * 2:+d} dB), "
                  f"{mhz} MHz ===")
            rdir = sdr_bench.run_dir(f"hil_mic_gain_{n}")
            with sdr_bench.lease():
                cap = sdr_bench.capture_start(rdir, mhz, seconds=window)
                print(f"GO: {window:.0f} s from now. Unplug the cable, key PTT, the tone or sentence for about 5 s, "
                      "release, plug the cable back in.", flush=True)
                t0 = time.time()
                sdr_bench.capture_wait(timeout=window + 120)
            print(f"recording done ({time.time() - t0:.0f} s); analyzing")
            rc, text = sdr_bench.analyze_fm(cap, bw_khz=bw_khz, ppm=pytestconfig.getoption("--sdr-ppm"), freq=mhz,
                                            tones=True)
            print(text)
            segs = [s for s in segments(text) if s["len"] >= MIN_SEGMENT_S]
            seg = max(segs, key=lambda s: s["len"]) if segs else None
            wait_link(radio)
            back = features(radio).get("set.mic_gain")
            reg = adcl(radio)
            rows.append({"bw": bw_khz, "step": step, "back": back, "adcl": reg, "seg": seg,
                         "capture": f"~/{sdr_bench.dir}/{rdir}/"})
            print(f"link back: step {back}, GCR_ADCL {'?' if reg is None else f'0x{reg:02X}'}", flush=True)
    finally:
        try:
            wait_link(radio, timeout=30.0)
            L.setting("mic_gain", step0)
        except (AssertionError, DebugLinkError):
            pass

    print("\n  bw kHz  step  dB   GCR_ADCL  len s  peak Hz  rms Hz  tone")
    for r in rows:
        s = r["seg"] or {}
        reg = "?" if r["adcl"] is None else f"0x{r['adcl']:02X}({r['adcl'] & 0x3F})"
        tone = s.get("tones") and max(s["tones"], key=lambda t: t[1])
        tone_txt = f"{tone[0]:.0f} Hz {tone[1]} Hz pk" if tone else "-"
        print(f"  {r['bw']:6g}  {r['step']:4d}  {(r['step'] - 5) * 2:+3d}  {reg:9s} "
              f"{s.get('len', 0):5.1f}  {s.get('peak', '-')!s:>7}  {s.get('rms', '-')!s:>6}  {tone_txt}")
    missing = [r for r in rows if r["seg"] is None]
    assert not missing, "no keyed segment of 1 s or more in: " + ", ".join(
        f"{r['bw']:g} kHz step {r['step']} ({r['capture']})" for r in missing)
    wrong = [r for r in rows if r["back"] != str(r["step"])]
    assert not wrong, "the step read back after the over differs (did the radio restart?): " + ", ".join(
        f"set {r['step']}, read {r['back']}" for r in wrong)
