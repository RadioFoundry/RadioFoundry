#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The D-STAR bench session's scriptable steps (docs/testing/dstar-bench.md): drive the radio over its debug link,
capture what it sends with an RTL-SDR, and analyze the captures. Each run writes a JSON record beside its captures,
so the session leaves the numbers, not just a pass.

    tools/dstar_bench.py watch [--seconds 120] [--out DIR]
        receive: log every change of the radio's dstar view (headers, messages, polarity, counters) while someone
        keys a D-STAR radio; prints one line per over
    tools/dstar_bench.py txpath --mhz 145.375 --sdr-mhz 145.350 --allow-rf [--out DIR] [--capture CMD]
        transmit path: tones from 100 Hz to 3.6 kHz, a 100 Hz square wave and the GMSK preamble through the flat path,
        each captured; prints the frequency response, the high-pass fitted to it, the deviation scale and the
        tx_1khz to set, and checks the preamble's deviation (+-1.2 kHz)
    tools/dstar_bench.py over --seconds 5 --allow-rf [--capture ...]
        one ordinary D-STAR over (the radio's mic, UR and header as set), captured: our frame sync found in the
        capture says the transmit polarity; deviation and the radio's counters recorded
    tools/dstar_bench.py analyze CAPTURE.cs8 --kind tone|square|dots|over --offset HZ [--hz F] [--level PCT]
        one capture again, offline (numpy only)
    tools/dstar_bench.py cal tx_1khz 9000 | tx_invert 1 | rx_invert -1 | tx_hpf 290 | rx_hpf 0
        set the calibration (stored on the radio; -1 = the board's own)

Nothing keys without --allow-rf (AGENTS.md rule 8). The radio must be on a D-STAR channel (or the test signals
refuse: PTT on another channel sends FM). The SDR command defaults to rtl_sdr; --capture takes a template with
{hz} {rate} {n} {out} for another receiver. The radio and the SDR share nothing else: a dummy load and a small
pick-up antenna, the lowest power.
"""
import argparse
import json
import os
import shlex
import subprocess
import sys
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

RATE = 1536000
TONES_HZ = [100, 150, 200, 300, 400, 600, 1000, 1500, 2000, 2500, 3000, 3600]
LEVEL = 50                  # test signals at half the 1 kHz scale: room for a scale that is too small by 2
KEY_S = 2.0                 # each test signal keyed this long
SYNC = [1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0]    # the frame sync (docs/spec/dstar-modem.md 3.3)
DEFAULT_CAPTURE = "rtl_sdr -f {hz} -s {rate} -n {n} {out}"


def np_mod():
    import numpy as np
    return np


def link_open(port):
    import rfoundry_debug
    return rfoundry_debug.DebugLink(port)


# --- capture ---

def capture(template, hz, seconds, out):
    """Start the SDR in the background; returns (thread, result dict). The capture file is CS8 at RATE"""
    n = int(RATE * seconds)
    cmd = template.format(hz=int(hz), rate=RATE, n=n, out=shlex.quote(out))
    res = {"cmd": cmd, "rc": None}

    def run():
        res["rc"] = subprocess.call(cmd, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t = threading.Thread(target=run)
    t.start()
    with open(out + ".json", "w") as f:
        json.dump({"center": hz, "rate": RATE, "fmt": "cs8"}, f)
    return t, res


def discriminator(path, offset):
    """The capture's 16 kHz discriminator output (Hz) and its keyed segments [(t0, t1)] in seconds"""
    import sdr_fm_check as s
    s.set_rate(RATE)
    s.set_format("cs8")
    audio, power = s.demodulate(path, offset, 12.5, 384000 - 384000 % (s.D1 * s.D2 * s.D3))
    if len(power) == 0:
        return audio, s.FS_AUDIO, []
    _, segs = s.find_segments(power, 15.0, 0.3)
    return audio, s.FS_AUDIO, [(a * s.POWER_WIN_S, b * s.POWER_WIN_S) for a, b in segs]


def keyed(audio, fs, segs, edge=0.15):
    """The longest keyed segment, its edges left out, carrier offset removed; None when nothing was keyed"""
    np = np_mod()
    if not segs:
        return None, 0.0
    t0, t1 = max(segs, key=lambda s: s[1] - s[0])
    x = audio[int((t0 + edge) * fs):int((t1 - edge) * fs)].astype(np.float64)
    if len(x) < fs * 0.3:
        return None, 0.0
    carrier = float(np.median(x))
    return x - carrier, carrier


# --- analysis ---

def tone_fit(x, fs, hz):
    """Amplitude (Hz of deviation) of a sine at hz in x, least squares in 100 ms blocks (the median)"""
    np = np_mod()
    blk = int(0.1 * fs)
    n = np.arange(blk) / fs
    a = np.stack([np.cos(2 * np.pi * hz * n), np.sin(2 * np.pi * hz * n)], axis=1)
    amps = []
    for i in range(0, len(x) - blk + 1, blk):
        c = np.linalg.lstsq(a, x[i:i + blk], rcond=None)[0]
        amps.append(float(np.hypot(c[0], c[1])))
    return float(np.median(amps)) if amps else 0.0


def fit_hpf(points):
    """A first-order high-pass |H| = k f / sqrt(f^2 + fc^2) fitted to [(f, amplitude)]: (k, fc) by a scan of fc"""
    np = np_mod()
    f = np.array([p[0] for p in points], float)
    a = np.array([p[1] for p in points], float)
    best = (0.0, 0.0, float("inf"))
    for fc in np.arange(0.0, 800.0, 1.0):
        g = f / np.sqrt(f * f + fc * fc)
        k = float((g @ a) / (g @ g))
        err = float(np.sum((20 * np.log10(np.maximum(k * g, 1e-9)) - 20 * np.log10(np.maximum(a, 1e-9))) ** 2))
        if err < best[2]:
            best = (k, float(fc), err)
    return best[0], best[1]


def square_droop(x, fs, hz):
    """A square wave through a first-order high-pass decays by exp(-t / tau) within each half period: tau from the
    mean level of each half's first and last fifth, fc = 1 / (2 pi tau); also the plateau's height (Hz)"""
    np = np_mod()
    half = fs / (2.0 * hz)
    ratios, heights = [], []
    # the half periods by the zero crossings of a smoothed copy
    s = np.sign(np.convolve(x, np.ones(5) / 5, mode="same"))
    edges = np.where(np.diff(s) != 0)[0]
    for a, b in zip(edges, edges[1:]):
        if abs((b - a) - half) > half * 0.2:
            continue
        n = b - a
        first = np.mean(np.abs(x[a + n // 10:a + n // 5]))
        last = np.mean(np.abs(x[b - n // 5:b - n // 10]))
        if first > 0 and last > 0:
            ratios.append(last / first)
            heights.append(first)
    if not ratios:
        return None, None
    r = float(np.median(ratios))
    dt = 0.7 * half / fs        # from the middle of the first fifth to the middle of the last: 0.7 of a half
    tau = -dt / np.log(r) if 0 < r < 1 else float("inf")
    return (1.0 / (2 * np.pi * tau) if np.isfinite(tau) else 0.0), float(np.median(heights))


def find_sync(x, fs):
    """Our frame sync in the discriminator output, sliced at 4800 bit/s at the best of 16 phases: (polarity, t) of
    the first match in either polarity, or (0, None)"""
    np = np_mod()
    sps = fs / 4800.0
    best = (0, None)
    for ph in range(16):
        idx = (np.arange(int((len(x) - 1) / sps)) * sps + ph * sps / 16).astype(int)
        idx = idx[idx < len(x)]
        bits = (x[idx] > 0).astype(int)
        for pol, want in ((1, SYNC), (-1, [1 - b for b in SYNC])):
            w = np.array(want)
            for i in range(0, len(bits) - len(w)):
                if np.array_equal(bits[i:i + len(w)], w):
                    t = idx[i] / fs
                    if best[1] is None or t < best[1]:
                        best = (pol, t)
                    break
    return best


def analyze(path, kind, offset, hz=1000, level=LEVEL):
    np = np_mod()
    audio, fs, segs = discriminator(path, offset)
    x, carrier = keyed(audio, fs, segs)
    rec = {"capture": path, "kind": kind, "segments": segs, "carrier_hz": carrier}
    if x is None:
        rec["error"] = "nothing keyed in the capture"
        return rec
    if kind == "tone":
        rec["hz"] = hz
        rec["dev_hz"] = tone_fit(x, fs, hz)
        rec["expected_hz"] = 1000.0 * level / 100.0
    elif kind == "square":
        fc, height = square_droop(x, fs, hz)
        rec["hpf_hz_from_droop"] = fc
        rec["plateau_hz"] = height
    elif kind in ("dots", "over"):
        a = np.abs(x)
        rec["dev_peak_hz"] = float(np.percentile(a, 99.5))
        rec["dev_p90_hz"] = float(np.percentile(a, 90))
        pol, t = find_sync(x, fs) if kind == "over" else (0, None)
        if kind == "over":
            rec["sync_polarity"] = pol
            rec["sync_t"] = t
    return rec


# --- the radio ---

def need_rf(a):
    if not a.allow_rf:
        sys.exit("dstar_bench: this keys the transmitter: pass --allow-rf (dummy load, lowest power)")


def check_dstar(link):
    v = link.dstar()
    if v.get("dstar.built") != "1":
        sys.exit("dstar_bench: the radio's firmware has no D-STAR (dstar.built=0)")
    if v.get("dstar.on") != "1":
        sys.exit("dstar_bench: put the radio on a D-STAR channel first (dstar.on=0)")
    return v


def keyed_capture(link, a, name, seconds, test=None, hz=1000, level=LEVEL):
    """Set the test signal, start the SDR, key for `seconds`, stop; returns the capture's path"""
    if test is not None:
        link.dstar_test(test, hz, level)
    out = os.path.join(a.out, name + ".cs8")
    t, res = capture(a.capture, a.sdr_mhz * 1e6, seconds + 2.0, out)
    time.sleep(0.8)              # the SDR's start-up
    link.ptt(True, max_ms=int((seconds + 1) * 1000))
    time.sleep(seconds)
    link.ptt(False)
    t.join()
    if test is not None:
        link.dstar_test("off")
    return out


def cmd_txpath(a):
    need_rf(a)
    link = link_open(a.port)
    before = check_dstar(link)
    offset = (a.mhz - a.sdr_mhz) * 1e6
    os.makedirs(a.out, exist_ok=True)
    rec = {"t": time.strftime("%Y-%m-%d %H:%M:%S"), "mhz": a.mhz, "view_before": before, "tones": []}
    for hz in TONES_HZ:
        path = keyed_capture(link, a, "tone_%d" % hz, KEY_S, "tone", hz)
        r = analyze(path, "tone", offset, hz)
        rec["tones"].append(r)
        print("tone %5d Hz: %7.0f Hz deviation (%.0f expected at the board's scale)" %
              (hz, r.get("dev_hz", 0.0), r.get("expected_hz", 0.0)))
    pts = [(r["hz"], r["dev_hz"]) for r in rec["tones"] if r.get("dev_hz")]
    if len(pts) >= 4:
        k, fc = fit_hpf(pts)
        rec["fit"] = {"k_hz": k, "hpf_hz": fc}
        tx_1khz = int(before.get("dstar.set_tx_1khz", "0")) or int(before.get("dstar.board_tx_1khz", "0"))
        if tx_1khz and k > 0:
            # k is the deviation the level's samples give above the high-pass: scale to 1 kHz at 100 %
            rec["tx_1khz_new"] = round(tx_1khz * (1000.0 * LEVEL / 100.0) / k)
        print("first-order high-pass fitted: fc %.0f Hz, flat deviation %.0f Hz at %d %% of the scale" % (fc, k, LEVEL))
        if "tx_1khz_new" in rec:
            print("set: tools/dstar_bench.py cal tx_1khz %d   and   cal tx_hpf %d" % (rec["tx_1khz_new"], round(fc)))
    path = keyed_capture(link, a, "square_100", KEY_S, "square", 100)
    rec["square"] = analyze(path, "square", offset, 100)
    print("square 100 Hz: high-pass from the droop %s Hz" % (
        "%.0f" % rec["square"]["hpf_hz_from_droop"] if rec["square"].get("hpf_hz_from_droop") is not None else "?"))
    path = keyed_capture(link, a, "dots", KEY_S, "dots")
    rec["dots"] = analyze(path, "dots", offset)
    print("GMSK preamble: peak %.0f Hz (+-1200 Hz wanted for a long run; 1010 peaks about 0.88 of it)" %
          rec["dots"].get("dev_peak_hz", 0.0))
    rec["view_after"] = link.dstar()
    with open(os.path.join(a.out, "txpath.json"), "w") as f:
        json.dump(rec, f, indent=1)
    print("record: %s" % os.path.join(a.out, "txpath.json"))
    return 0


def cmd_over(a):
    need_rf(a)
    link = link_open(a.port)
    before = check_dstar(link)
    link.dstar_reset()
    os.makedirs(a.out, exist_ok=True)
    offset = (a.mhz - a.sdr_mhz) * 1e6
    path = keyed_capture(link, a, "over", a.seconds)
    time.sleep(0.5)
    after = link.dstar()
    r = analyze(path, "over", offset)
    rec = {"t": time.strftime("%Y-%m-%d %H:%M:%S"), "view_before": before, "view_after": after, "analysis": r}
    print("deviation peak %.0f Hz, frame sync %s" % (r.get("dev_peak_hz", 0.0), {
        1: "found, as sent (tx_invert right)", -1: "found inverted: set tx_invert", 0: "not found"}[r.get("sync_polarity", 0)]))
    print("frames encoded %s, underruns %s, overflows %s, mic peak %s" % (
        after.get("dstar.tx_frames"), after.get("dstar.tx_underruns"), after.get("dstar.tx_overflows"),
        after.get("dstar.tx_peak")))
    with open(os.path.join(a.out, "over.json"), "w") as f:
        json.dump(rec, f, indent=1)
    return 0


WATCH_KEYS = ("dstar.rx", "dstar.my", "dstar.suffix", "dstar.ur", "dstar.rpt1", "dstar.rpt2", "dstar.msg",
              "dstar.heard_slow", "dstar.polarity", "dstar.hdr_corrected")


def cmd_watch(a):
    link = link_open(a.port)
    check_dstar(link)
    os.makedirs(a.out, exist_ok=True)
    log = open(os.path.join(a.out, "watch.jsonl"), "a")
    last = {}
    end = time.time() + a.seconds
    print("watching for %d s: key a D-STAR radio now" % a.seconds)
    while time.time() < end:
        v = link.dstar()
        cur = {k: v.get(k) for k in WATCH_KEYS + ("dstar.rx_overs", "dstar.rx_frames", "dstar.rx_bad",
                                                   "dstar.rx_bit_errors", "dstar.rx_played", "dstar.rx_concealed")}
        if any(cur.get(k) != last.get(k) for k in WATCH_KEYS):
            log.write(json.dumps({"t": time.time(), **cur}) + "\n")
            log.flush()
            if cur["dstar.rx"] == "0" and last.get("dstar.rx") == "1":
                print("over: %s/%s UR %s via %s>%s msg %r polarity %s%s frames %s bad %s corrected bits %s" % (
                    cur["dstar.my"], cur["dstar.suffix"], cur["dstar.ur"], cur["dstar.rpt1"], cur["dstar.rpt2"],
                    cur["dstar.msg"], cur["dstar.polarity"], " (late entry)" if cur["dstar.heard_slow"] == "1" else "",
                    cur["dstar.rx_frames"], cur["dstar.rx_bad"], cur["dstar.rx_bit_errors"]))
            last = cur
        time.sleep(0.3)
    print("log: %s" % os.path.join(a.out, "watch.jsonl"))
    return 0


def cmd_cal(a):
    link = link_open(a.port)
    link.dstar_cal(a.field, a.value)
    v = link.dstar()
    print(" ".join("%s=%s" % (k, v.get(k)) for k in v if k.startswith("dstar.set_") or k.startswith("dstar.board_")))
    return 0


def cmd_analyze(a):
    print(json.dumps(analyze(a.capture, a.kind, a.offset, a.hz, a.level), indent=1))
    return 0


def self_test():
    """The analysis on made-up captures (no radio, no SDR): a tone at a known deviation, a square wave through a 290 Hz
    first-order high-pass, and an NRZ bit stream carrying the frame sync, inverted and not. Skips without numpy"""
    try:
        np = np_mod()
    except ImportError:
        print("dstar_bench self-test: skipped (no numpy)")
        return 0
    import tempfile
    tmp = tempfile.mkdtemp(prefix="dstar-bench-", dir=os.environ.get("TMPDIR"))
    fs = float(RATE)
    off = -25000.0
    fails = []

    def write(name, dev_hz):
        # FM at the channel, off the tuner center; carrier for 0.3 s each side with nothing on it
        pad = np.zeros(int(0.3 * fs))
        f = np.concatenate((pad, dev_hz, pad)) + off
        ph = 2 * np.pi * np.cumsum(f) / fs
        amp = np.concatenate((np.zeros(len(pad) // 2), np.full(len(f) - len(pad), 0.8), np.zeros(len(pad) // 2)))
        iq = amp[:len(ph)] * np.exp(1j * ph)
        raw = np.empty(2 * len(iq), np.int8)
        raw[0::2] = np.clip(np.round(iq.real * 127), -127, 127)
        raw[1::2] = np.clip(np.round(iq.imag * 127), -127, 127)
        path = os.path.join(tmp, name + ".cs8")
        raw.tofile(path)
        return path

    t = np.arange(int(1.5 * fs)) / fs
    r = analyze(write("tone", 500.0 * np.sin(2 * np.pi * 1000 * t)), "tone", off, 1000)
    if not 480 < r.get("dev_hz", 0) < 520:
        fails.append("tone: %r" % r.get("dev_hz"))
    # a square through a first-order high-pass at 290 Hz (a one-pole filter on the deviation)
    sq = 800.0 * np.sign(np.sin(2 * np.pi * 100 * t))
    a = np.exp(-2 * np.pi * 290.0 / fs)
    hp = np.empty_like(sq)
    prev_x = prev_y = 0.0
    for i, v in enumerate(sq):
        prev_y = a * (prev_y + v - prev_x)
        prev_x = v
        hp[i] = prev_y
    r = analyze(write("square", hp), "square", off, 100)
    if not 220 < (r.get("hpf_hz_from_droop") or 0) < 360:
        fails.append("square: %r" % r.get("hpf_hz_from_droop"))
    rng = np.random.default_rng(1)
    for pol in (1, -1):
        bits = list(rng.integers(0, 2, 3000)) + SYNC + list(rng.integers(0, 2, 3000))
        nrz = np.repeat([1200.0 * (2 * b - 1) * pol for b in bits], int(fs / 4800))
        w = np.hanning(int(fs / 4800))
        nrz = np.convolve(nrz, w / w.sum(), mode="same")	# shaped, as GMSK is: no overshoot for the peak figure
        r = analyze(write("over%d" % pol, nrz), "over", off)
        if r.get("sync_polarity") != pol:
            fails.append("sync polarity %d: %r" % (pol, r.get("sync_polarity")))
        if not 1000 < r.get("dev_peak_hz", 0) < 1400:
            fails.append("over deviation: %r" % r.get("dev_peak_hz"))
    import shutil
    shutil.rmtree(tmp, ignore_errors=True)
    for f in fails:
        print("FAIL dstar_bench self-test: " + f)
    print("dstar_bench self-test: %s" % ("FAILED" if fails else "passed"))
    return 1 if fails else 0


def main():
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", help="the radio's debug link port (default: the one found)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("txpath", "over"):
        p = sub.add_parser(name)
        p.add_argument("--mhz", type=float, required=True, help="the channel the radio is on")
        p.add_argument("--sdr-mhz", type=float, help="the SDR's tuner center (default: 25 kHz below, off its DC spike)")
        p.add_argument("--capture", default=DEFAULT_CAPTURE, help="SDR command template ({hz} {rate} {n} {out})")
        p.add_argument("--out", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "dstar-bench"))
        p.add_argument("--allow-rf", action="store_true")
        if name == "over":
            p.add_argument("--seconds", type=float, default=5.0)
    p = sub.add_parser("watch")
    p.add_argument("--seconds", type=int, default=120)
    p.add_argument("--out", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "dstar-bench"))
    p = sub.add_parser("cal")
    p.add_argument("field", choices=["tx_1khz", "tx_invert", "rx_invert", "tx_hpf", "rx_hpf"])
    p.add_argument("value", type=int)
    p = sub.add_parser("analyze")
    p.add_argument("capture")
    p.add_argument("--kind", choices=["tone", "square", "dots", "over"], required=True)
    p.add_argument("--offset", type=float, required=True, help="channel minus tuner center, Hz")
    p.add_argument("--hz", type=float, default=1000.0)
    p.add_argument("--level", type=float, default=LEVEL)
    a = ap.parse_args()
    if getattr(a, "mhz", None) is not None and a.sdr_mhz is None:
        a.sdr_mhz = a.mhz - 0.025
    return {"txpath": cmd_txpath, "over": cmd_over, "watch": cmd_watch, "cal": cmd_cal,
            "analyze": cmd_analyze}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
