#!/usr/bin/env python3
"""rssi_cal: check a radio's RSSI against the SDR bench, step by step down in level.

A transmitter sends a signal on the radio's frequency at each level of a sweep; the radio's rssi_dbm (and, where the
board has it, its raw RSSI and noise diagnostics line) is read over the debug link while it lasts. One reading with
nothing sent gives the radio's floor. --dbm 0:-100:-4 asks each step's job for that power_dbm at the antenna: on a
bench board whose transmitter turns its gain down for it (an SDR's), the steps are a dB scale, the dBm itself
nominal. --levels 255:0:-16 sends at the board's 0-255 setting instead, which need not be in dB: the radio's readings
only, no fit.

    rssi_cal.py --radio-port PORT --freq 446.125 --bench-api BOARD --dbm 0:-100:-4 [--mode dmr --sensitivity-dbm D]

What it reports:
- slope: the radio's dB per step dB over the steps clear of the floor (least squares); 1.0 is right. The steps
  where the radio stops following (compression at the top, the floor at the bottom) are marked.
- offset: radio dBm less the step's dB. With --ref-dbm (dBm at the radio's input at the top step, from a cabled
  source or a measured path) it becomes the radio's error in dB.
- DMR (--mode dmr): each step is a voice call (--superframes) to --tg; the radio's voice bursts counted (call.rx_voice_bursts)
  against those sent give the decode rate. The first step under 95 % is the dropout; with --sensitivity-dbm (the
  radio's rated sensitivity, the level its decoding should fail at) the dropout gives the absolute anchor
  instead of --ref-dbm: an estimate to within a few dB.

FM tunes the radio's VFO to --freq; DMR selects the first DMR channel of the codeplug on --freq (it plays only
calls to a group in its receive list, so a channel set up for the bench's --tg and --cc); --channel leaves the radio
where it is. Its zone, channel or VFO are put back after. Run it on the host the radio is cabled
to, under the bench's locks and a lease on --freq (the bench boards transmit at the levels asked: full power at the
top). Needs tests/hil (rfoundry_hil.py) and tools/ on the path, which it adds itself.
"""
import argparse
import json
import os
import re
import statistics
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [HERE, os.path.join(ROOT, "tools"), os.path.join(ROOT, "tests", "hil")]

DECODE_OK = 0.95        # DMR: a step at or above this share of voice bursts decoded counts as heard
CLEAR_DB = 6.0          # a step this far over the radio's floor is in the fit
COMPRESS_DB = 1.5       # a top step this far under the line fitted to the others is compression
RAW_RE = re.compile(r"\brssi (\d+) nz (\d+) (sig|-)")


def parse_levels(text):
    """"255:128:-8" (start:stop:step, stop included when on the grid) or "255,224,192" """
    num = lambda x: float(x) if "." in x else int(x)
    if ":" in text:
        a, b, s = (num(x) for x in text.split(":"))
        if s == 0 or (b - a) * s < 0:
            raise ValueError(f"levels {text}: the step must go from {a} toward {b}")
        out, v = [], a
        while (v >= b) if s < 0 else (v <= b):
            out.append(v)
            v = round(v + s, 3)
        return out
    return [num(x.strip()) for x in text.split(",") if x.strip()]


def fit(xs, ys):
    """Least squares y = a + b x: (b, a, the largest residual)"""
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None, None, None
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    a = my - b * mx
    return b, a, max(abs(y - (a + b * x)) for x, y in zip(xs, ys))


def analyze(steps, floor_dbm, ref_dbm=None, sensitivity_dbm=None):
    """The sweep's figures from its steps (dicts with level, ref_db, radio_dbm and, in DMR, decoded): which steps are
    in the fit, slope, offset, compression, the dropout and the radio's error where an anchor gives one."""
    use = [s for s in steps if s.get("ref_db") is not None and s.get("radio_dbm") is not None
           and s["radio_dbm"] >= floor_dbm + CLEAR_DB]
    out = {"floor_dbm": floor_dbm}
    # Compression: drop top steps while the strongest sits under the line through the rest
    use.sort(key=lambda s: -s["ref_db"])
    compressed = []
    while len(use) >= 4:
        b, a, _ = fit([s["ref_db"] for s in use[1:]], [s["radio_dbm"] for s in use[1:]])
        if b is None or a + b * use[0]["ref_db"] - use[0]["radio_dbm"] < COMPRESS_DB:
            break
        compressed.append(use.pop(0)["level"])
    out["compressed_levels"] = compressed
    out["steps_fitted"] = len(use)
    if len(use) >= 2:
        b, a, worst = fit([s["ref_db"] for s in use], [s["radio_dbm"] for s in use])
        if b is not None:
            out.update(slope=round(b, 3), worst_residual_db=round(worst, 2),
                       span_db=round(use[0]["ref_db"] - use[-1]["ref_db"], 1))
            out["offset_db"] = round(statistics.median(s["radio_dbm"] - s["ref_db"] for s in use), 1)
    top = max((s for s in steps if s.get("ref_db") is not None), key=lambda s: s["ref_db"], default=None)
    to_dbm = None               # reference dBFS -> dBm at the radio
    if ref_dbm is not None and top is not None:
        to_dbm = ref_dbm - top["ref_db"]
        out["anchor"] = "ref_dbm"
    heard = [s for s in steps if "decoded" in s]
    if heard:
        heard.sort(key=lambda s: -(s["ref_db"] if s.get("ref_db") is not None else -999))
        drop = next((i for i, s in enumerate(heard) if s["decoded"] < DECODE_OK), None)
        if drop is None:
            out["dropout"] = None
        elif drop == 0:
            out["dropout"] = "at the top step"
        else:
            hi, lo = heard[drop - 1], heard[drop]
            if hi.get("ref_db") is not None and lo.get("ref_db") is not None:
                # interpolate the decode rate's crossing of DECODE_OK between the last good step and the first bad
                f = (hi["decoded"] - DECODE_OK) / max(hi["decoded"] - lo["decoded"], 1e-9)
                at = hi["ref_db"] - f * (hi["ref_db"] - lo["ref_db"])
                out["dropout_ref_db"] = round(at, 1)
                if sensitivity_dbm is not None and to_dbm is None:
                    to_dbm = sensitivity_dbm - at
                    out["anchor"] = "sensitivity"
    if to_dbm is not None:
        errs = [s["radio_dbm"] - (s["ref_db"] + to_dbm) for s in use]
        if errs:
            out["radio_error_db"] = round(statistics.median(errs), 1)
        out["ref_to_dbm_db"] = round(to_dbm, 1)
    return out


def self_test():
    steps = [{"level": 255 - 8 * i, "ref_db": -20.0 - 2 * i, "radio_dbm": -90.0 - 2 * i} for i in range(10)]
    steps[0]["radio_dbm"] = -91.0                       # one dB short at the top: not yet compression
    r = analyze(steps, floor_dbm=-125)
    assert r["compressed_levels"] == [] and abs(r["slope"] - 1.0) < 0.06, r
    steps[0]["radio_dbm"] = -94.0                       # four short: compression
    r = analyze(steps, floor_dbm=-125)
    assert r["compressed_levels"] == [255] and abs(r["slope"] - 1.0) < 1e-6 and r["offset_db"] == -70.0, r
    r = analyze(steps, floor_dbm=-100)                  # only steps 6 dB over the floor are fitted
    assert r["steps_fitted"] == 3, r
    r = analyze(steps, floor_dbm=-125, ref_dbm=-50.0)   # the top step is -50 dBm, where a true radio reads -50
    assert r["radio_error_db"] == -40.0, r              # this one reads ref - 70 = -90 there: 40 dB low
    dmr = [dict(s, decoded=d) for s, d in zip(steps, (1, 1, 1, 1, 1, 0.97, 0.75, 0.2, 0, 0))]
    r = analyze(dmr, floor_dbm=-125, sensitivity_dbm=-120.0)
    # 0.97 at -30 dBFS, 0.75 at -32: 95 % crossed at -30.18, where the radio reads -100.18 dBm and the anchor says
    # -120: the radio reads 19.8 dB high
    assert r["dropout_ref_db"] == -30.2 and r["anchor"] == "sensitivity" and r["radio_error_db"] == 19.8, r
    assert parse_levels("255:191:-32") == [255, 223, 191] and parse_levels("1,2") == [1, 2]
    assert parse_levels("0:-1:-0.25") == [0, -0.25, -0.5, -0.75, -1.0], parse_levels("0:-1:-0.25")
    print("rssi_cal self-test passed")
    return 0


# ── on the bench ──

def read_radio(radio, until, period=0.1):
    """The radio's readings until `until` (monotonic): rssi_dbm, state, and the raw line when the board has one"""
    vals = []
    while time.monotonic() < until:
        core = radio.core()
        r = {"rssi_dbm": int(core["rssi_dbm"]), "state": core.get("state")}
        try:
            _, lines = radio.link.diag()
            m = next((RAW_RE.search(ln) for ln in lines if RAW_RE.search(ln)), None)
            if m:
                r.update(raw=int(m.group(1)), noise=int(m.group(2)), sig=m.group(3) == "sig")
        except Exception:
            pass
        vals.append(r)
        time.sleep(period)
    return vals


def summarize(vals):
    out = {"radio_dbm": statistics.median(v["rssi_dbm"] for v in vals) if vals else None, "reads": len(vals)}
    if vals:
        out["radio_spread_db"] = max(v["rssi_dbm"] for v in vals) - min(v["rssi_dbm"] for v in vals)
        out["states"] = sorted({v["state"] for v in vals if v["state"]})
    raws = [v for v in vals if "raw" in v]
    if raws:
        out["raw"] = statistics.median(v["raw"] for v in raws)
        out["noise"] = statistics.median(v["noise"] for v in raws)
        out["sig"] = sum(v["sig"] for v in raws) / len(raws)
    return out


def counters(radio):
    """The radio's counters (diagnostics lines and the dmr_stats view)"""
    return radio.counters()[1]


def bench_sweep(radio, a):
    """--bench-api: each step a bench job at its level, the reference board (if any) measuring it"""
    from rfoundry_hil import SdrBench
    bench = SdrBench(allow_rf=True, api=a.bench_api, lease_max=3600, wait_max=120)
    steps = []
    by_dbm = a.dbm is not None
    with bench.lease():
        for lvl in parse_levels(a.dbm if by_dbm else a.levels):
            bench.api_level = None if by_dbm else lvl
            power = {"power_dbm": lvl} if by_dbm else {}
            step = {"level": lvl}
            if by_dbm:
                step["ref_db"] = float(lvl)
            if a.mode == "fm":
                tx = bench.fm_tx_start(a.freq, [{"kind": "silence", "s": a.seconds}], max_s=a.seconds + 5, **power)
                vals = read_radio(radio, time.monotonic() + a.seconds - 0.2)
                vals = vals[max(0, len(vals) // 5):]          # skip the first fifth: the squelch opening
                tx.wait(a.seconds + 15)
            else:
                before = counters(radio).get("call.rx_voice_bursts", 0)
                sent = 6 * a.superframes
                box = []
                th = threading.Thread(target=lambda: box.extend(read_radio(radio, time.monotonic() + sent * 0.06 + 0.6)))
                th.start()
                bench.transmit((f"--power-dbm {lvl} " if by_dbm else "") +
                               f"voice --dst {a.tg} --superframes {a.superframes} --calls 1 --headers 8 --terminators 3",
                               a.freq, a.cc, expect_s=(8 + sent + 3) * 0.06)
                th.join()
                vals = [v for v in box if v["state"] not in (None, "idle")] or box
                time.sleep(0.5)
                step["decoded"] = round((counters(radio).get("call.rx_voice_bursts", 0) - before) / sent, 3)
            step.update(summarize(vals))
            steps.append(step)
            print(json.dumps(step), flush=True)
    return steps


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--radio-port", help="the radio's debug link port")
    ap.add_argument("--bench-api", help="the TX board, HOST[:PORT]")
    ap.add_argument("--freq", type=float, help="MHz")
    ap.add_argument("--mode", choices=("fm", "dmr"), default="fm")
    ap.add_argument("--levels", default="255:0:-16", help="without --dbm: the board's levels 0-255, start:stop:step or a list")
    ap.add_argument("--dbm", help="the bench job's power_dbm per step, start:stop:step or a list (the step's dB)")
    ap.add_argument("--seconds", type=float, default=2.5, help="FM: each step's carrier")
    ap.add_argument("--cc", type=int, default=1)
    ap.add_argument("--channel", action="store_true",
                    help="leave the radio on its channel (on --freq) instead of tuning its VFO there")
    ap.add_argument("--tg", type=int, default=202, help="DMR: the group the calls go to")
    ap.add_argument("--superframes", type=int, default=5, help="DMR: each step's call length")
    ap.add_argument("--ref-dbm", type=float, help="dBm at the radio's input at the top step (a cabled reference)")
    ap.add_argument("--sensitivity-dbm", type=float, help="DMR: the radio's rated sensitivity (the anchor)")
    ap.add_argument("--json", help="write the steps and the figures here")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not (a.radio_port and a.freq and a.bench_api):
        ap.error("--radio-port, --freq and --bench-api are needed")
    from rfoundry_hil import Radio

    radio = Radio(a.radio_port)
    snap = radio.snapshot()
    try:
        if a.channel:
            pass
        elif a.mode == "dmr":
            # a channel of the codeplug on --freq (the call's group in its receive list): the VFO plays only its own
            radio.find_channel(round(a.freq * 1e6), mode="dmr")
            time.sleep(1.0)
        else:
            radio.link.vfo(True)
            radio.link.vfo_set(round(a.freq * 1e6))
            radio.link.dmr_fields(mode="fm")
            time.sleep(1.0)
        core = radio.core()
        if abs(int(core["rx_hz"]) - a.freq * 1e6) > 100:
            raise SystemExit(f"the radio receives on {int(core['rx_hz']) / 1e6} MHz, not {a.freq}")
        floor = summarize(read_radio(radio, time.monotonic() + 3.0))
        print(f"floor: {floor['radio_dbm']} dBm (spread {floor['radio_spread_db']} dB)"
              + (f", raw {floor['raw']} noise {floor['noise']}" if "raw" in floor else ""), flush=True)
        steps = bench_sweep(radio, a)
    finally:
        radio.restore(snap)
    figures = analyze(steps, floor["radio_dbm"], a.ref_dbm, a.sensitivity_dbm)
    print("result: " + json.dumps(figures))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"freq": a.freq, "mode": a.mode, "floor": floor, "steps": steps, "figures": figures}, f,
                      indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
