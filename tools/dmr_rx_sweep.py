#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""DMR receive sweep: tune a receiver register against the SDR bench, fast.

    tools/dmr_rx_sweep.py --allow-rf --port PORT --bench-api HOST --freq MHZ --cc N --tg TG --ambe FILE
                          [--reg CHIP:REG[:PAGE] --values V,V,...] [--offsets=-1000:1000:500] [--dbm=D,D,...]
                          [--devs 0.9,1.0,1.1] [--dwell S] [--repeat N] [--per-call] [--out FILE.jsonl]

The radio receives on its VFO (DMR, --cc, slot 1, --tg the talk target) and never transmits. For each bench level
(--dbm, dBm at the SDR's antenna; default its own), deviation (--devs, a share of the standard's; default 1) and
carrier offset, the bench sends one long group call of the speech file, repeated, and the radio steps through every
register value during it (CHIP_REG, debug link RADIO op 37: written in RAM and read back; the radio's next receiver
set-up writes its own value again). A warm-up call goes first, uncounted. Each call starts at the radio's own value
and waits for the radio to receive it (a call that never arrives is skipped); the order of values rotates from call to
call, and the next call waits until the radio has let the last one go (a call that starts while the radio still
follows the last one's slot timing can be decoded at that timing to its end). A write can cost the radio its sync, so
each value waits until frames flow again (up to 2 s: "no sync" otherwise), then counts --dwell seconds:

    in      voice frames that reached the voice layer, of the 50 a second sent
    bad     frames the FEC couldn't correct
    bits    FEC-corrected bits a good frame (a clean receiver near sensitivity: about 0.1)

    sync    seconds from the write until frames flowed again

One call covers every value: a grid of 4 values x 3 offsets x 2 deviations x 3 levels takes about three minutes.
--per-call writes each value before a call of its own instead, so the radio syncs at it: slower, for a setting that
may cost a call's start.

Choose where to sweep. At full level a receiver's errors come from what doesn't depend on the signal (an overloaded
stage, the transmitter's own flaws), and a small change in a setting barely shows. Near sensitivity a fraction of a dB,
or a few hundred Hz, is a clear difference: add a level where the radio's own setting loses a few frames. The summary
ranks each value by its worst cell over everything swept: the setting to pick is the one that holds up best for
transmitters a little off frequency or off deviation, not the best cell at the center.

Without --reg the radio's own settings are measured. The register's value at the start is written back at the end.
Use a speech file, never silence: a receiver can flip bits in ways only some frame contents show.
docs/spec/dmr-voice.md 7.1 has the receive level window this tunes.
"""
import argparse
import json
import os
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests", "hil"))
from rfoundry_hil import Radio, SdrBench  # noqa: E402

FRAMES_S = 50               # 3 AMBE frames a 60 ms burst
START_S = 1.5               # the call's header and the radio's sync, before the first value
START_WAIT_S = 4.0          # then at most this long for frames to flow
KEYS = ("voice.rx_in", "voice.rx_bad", "voice.rx_bit_errors")


def parse_reg(text):
    """CHIP:REG[:PAGE] (numbers in any base Python reads: 0x44) -> (chip, reg, page)"""
    p = [int(x, 0) for x in text.split(":")]
    if len(p) not in (2, 3):
        raise argparse.ArgumentTypeError("--reg is CHIP:REG or CHIP:REG:PAGE")
    return p[0], p[1], p[2] if len(p) == 3 else 0


def parse_offsets(text):
    """FROM:TO:STEP or a comma list, Hz"""
    if ":" in text:
        lo, hi, st = (int(x) for x in text.split(":"))
        if st <= 0 or hi < lo:
            raise argparse.ArgumentTypeError("--offsets FROM:TO:STEP with FROM <= TO and STEP > 0")
        return list(range(lo, hi + 1, st))
    return [int(x) for x in text.split(",")]


def long_file(path, seconds, tmpdir):
    """The speech file repeated to last seconds; returns (path, superframes)"""
    data = open(path, "rb").read()
    need = int(seconds * FRAMES_S) + 18
    out = data * -(-need // (len(data) // 9))
    p = os.path.join(tmpdir, "sweep.ambe")
    with open(p, "wb") as f:
        f.write(out)
    return p, -(-(len(out) // 9) // 18)


def counters(L):
    try:
        v = L.view(["dmr_stats"])
    except Exception:           # one missed reply (a busy radio) shouldn't end a sweep
        time.sleep(0.2)
        v = L.view(["dmr_stats"])
    return {k: int(v.get(k, 0)) for k in KEYS}


def cell(a, b, dwell):
    d = {k: b[k] - a[k] for k in KEYS}
    good = max(0, d["voice.rx_in"] - d["voice.rx_bad"])
    sent = round(dwell * FRAMES_S)
    return {"in": min(d["voice.rx_in"], sent), "sent": sent, "bad": d["voice.rx_bad"],
            "bits": round(d["voice.rx_bit_errors"] / good, 3) if good else None}


def add(c, d):
    if c is None:
        return d
    bits = None
    good_c, good_d = c["in"] - c["bad"], d["in"] - d["bad"]
    if good_c + good_d:
        bits = round(((c["bits"] or 0) * good_c + (d["bits"] or 0) * good_d) / (good_c + good_d), 3)
    return {"in": c["in"] + d["in"], "sent": c["sent"] + d["sent"], "bad": c["bad"] + d["bad"], "bits": bits,
            "rssi": d.get("rssi", c.get("rssi")),
            "sync": None if c.get("sync") is None or d.get("sync") is None else max(c["sync"], d["sync"])}


def badness(c):
    """A cell's rank, worst first: frames that never arrived or failed, then corrected bits"""
    return ((c["sent"] - c["in"] + c["bad"]) / c["sent"], c["bits"] if c["bits"] is not None else 99.0)


def show(c):
    if not c["in"]:
        return "no sync" if c.get("sync") is None else "no frames"
    bits = "-" if c["bits"] is None else f"{c['bits']:.2f}"
    rssi = f" {c['rssi']:4d} dBm" if c.get("rssi") is not None else ""
    return f"{100 * c['in'] / c['sent']:3.0f}% {bits:>5} b/f {c['bad']:2d} bad{rssi}"


def label(cond):
    dbm, dev, off = cond
    return ("" if dbm is None else f"{dbm:+.0f} dBm ") + f"dev {dev:.2f} {off:+5d} Hz"


def name(v):
    return "radio" if v is None else f"0x{v:04X}"


def reg_write(L, regt, v):
    """Write a register value and check it read back; one retry on a missed reply"""
    for attempt in (0, 1):
        try:
            got = L.chip_reg(regt[0], regt[1], v, page=regt[2])
            break
        except Exception:
            if attempt:
                raise
            time.sleep(0.2)
    if got != v:
        raise RuntimeError(f"wrote 0x{v:04X}, read back 0x{got:04X}")


def wait_flowing(L, timeout=2.0):
    """Seconds until the voice layer gets frames again (two polls in a row with new frames), None after timeout"""
    t0 = time.monotonic()
    last, runs = counters(L)["voice.rx_in"], 0
    while time.monotonic() - t0 < timeout:
        time.sleep(0.1)
        n = counters(L)["voice.rx_in"]
        runs = runs + 1 if n > last else 0
        last = n
        if runs >= 2:
            return round(time.monotonic() - t0, 2)
    return None


def measure(a, L, sync):
    """One value's cell: --dwell seconds of counters once frames flow (sync: seconds until they did, None: never)"""
    if sync is None:
        c = {"in": 0, "sent": round(a.dwell * FRAMES_S), "bad": 0, "bits": None}
    else:
        c0 = counters(L)
        time.sleep(a.dwell)
        c = cell(c0, counters(L), a.dwell)
        c["rssi"] = int(L.view(["core"]).get("rssi_dbm", 0))
    c["sync"] = sync
    return c


def record(a, out, cond, v, c, regt):
    if not out:
        return
    dbm, dev, off = cond
    rec = {"t": time.time(), "freq": a.freq, "dbm": dbm, "dev": dev, "offset_hz": off, "value": v, **c}
    if regt:
        rec.update(chip=regt[0], reg=regt[1], page=regt[2])
    out.write(json.dumps(rec) + "\n")
    out.flush()


def per_call(a, L, bench, ambe, cond, values, calls, table, out, regt):
    """One short call a value, the value written before the call: the radio's sync at the start of a call is part of
    what a setting decides (a call it syncs to badly stays bad to its end)"""
    dbm, dev, off = cond
    k = calls % len(values)
    for v in values[k:] + values[:k]:
        if v is not None:
            reg_write(L, regt, v)
        wait_quiet(L)
        with bench.lease():
            job = bench.voice_call(a.freq, a.cc, a.tg, calls=1, ambe=ambe, superframes=superframes_for(a),
                                   offset_hz=0 if a.detune else off, background=True)
            c = measure(a, L, wait_flowing(L, timeout=START_S + 3.0))
            job.kill()
        table[(cond, v)] = add(table.get((cond, v)), c)
        c["quiet"] = wait_quiet(L)
        record(a, out, cond, v, c, regt)


def superframes_for(a):
    return -(-round((START_S + 3.0 + a.dwell + 1.0) * FRAMES_S) // 18)


def wait_quiet(L, timeout=6.0):
    """Wait until the radio stops counting frames (the last call is over on air and in the radio): a call that starts
    while the radio still follows the previous one arrives at another slot timing. Returns the seconds it took."""
    t0 = time.monotonic()
    last, still = counters(L)["voice.rx_in"], 0
    while time.monotonic() - t0 < timeout:
        time.sleep(0.2)
        n = counters(L)["voice.rx_in"]
        still = still + 1 if n == last else 0
        last = n
        if still >= 3:
            return round(time.monotonic() - t0, 2)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--allow-rf", action="store_true", help="the bench transmits: required")
    ap.add_argument("--port", required=True, help="the radio's debug link port")
    ap.add_argument("--bench-api", required=True, help="the bench board, HOST[:PORT]")
    ap.add_argument("--freq", type=float, required=True, help="MHz, a frequency the bench may transmit on")
    ap.add_argument("--cc", type=int, default=1)
    ap.add_argument("--tg", type=int, default=9)
    ap.add_argument("--ambe", required=True, help="a speech recording, 9-byte AMBE frames")
    ap.add_argument("--reg", type=parse_reg, help="CHIP:REG[:PAGE] to sweep (CHIP_REG numbering, debuglink.md)")
    ap.add_argument("--values", help="comma list of register values")
    ap.add_argument("--offsets", type=parse_offsets, default=[0],
                    help="carrier offsets, Hz: FROM:TO:STEP or a list (--offsets=-1000:1000:500 with a leading minus)")
    ap.add_argument("--detune", action="store_true",
                    help="make each offset by tuning the radio's receiver away from --freq instead of moving the bench "
                         "carrier (works with any bench board; the radio reports where it is tuned)")
    ap.add_argument("--dbm", help="bench levels, dBm at the SDR's antenna, comma list (--dbm=-40,-70; default its own)")
    ap.add_argument("--level", type=int, help="a W12-type board's transmit level 0-255 (one for the run; default its own)")
    ap.add_argument("--devs", default="1", help="deviations, a share of the standard's, comma list (0.5-1.5)")
    ap.add_argument("--dwell", type=float, default=1.0, help="seconds a value is counted (default 1: 50 frames)")
    ap.add_argument("--repeat", type=int, default=1, help="calls a level, deviation and offset (cells add up)")
    ap.add_argument("--per-call", action="store_true",
                    help="write each value before a call of its own, so the radio syncs at it (slower; catches values "
                         "that cost the call's start)")
    ap.add_argument("--out", help="append one JSON line a cell")
    a = ap.parse_args()
    if not a.allow_rf:
        ap.error("the bench transmits: pass --allow-rf (on a frequency you may use)")
    if (a.reg is None) != (a.values is None):
        ap.error("--reg and --values go together")
    if os.path.getsize(a.ambe) % 9 or not os.path.getsize(a.ambe):
        ap.error(f"{a.ambe}: not a 9-byte-frame .ambe file")
    values = [int(v, 0) for v in a.values.split(",")] if a.values else [None]
    dbms = [float(x) for x in a.dbm.split(",")] if a.dbm else [None]
    devs = [float(x) for x in a.devs.split(",")]
    tmp = tempfile.mkdtemp(prefix="dmr-rx-sweep-")
    # Long enough for the slowest start and every value's sync wait (and for a --per-call call, superframes_for)
    ambe, superframes = long_file(a.ambe, START_S + START_WAIT_S + len(values) * (2.0 + a.dwell) + 1.0, tmp)

    radio = Radio(a.port)
    L = radio.link
    bench = SdrBench(allow_rf=True, api=a.bench_api, api_level=a.level)
    L.vfo(True)
    L.vfo_set(round(a.freq * 1e6))
    L.dmr_fields("dmr", a.cc, 1)
    L.dmr_target(a.tg, False)
    time.sleep(0.5)
    print(f"radio on {int(L.view(['core']).get('rx_hz', 0)) / 1e6:.5f} MHz")
    orig = None
    if a.reg:
        chip, reg, page = a.reg
        orig = L.chip_reg(chip, reg, page=page)
        print(f"chip {chip} reg 0x{reg:02X} page {page}: 0x{orig:04X} at the start")
    out = open(a.out, "a") if a.out else None
    table = {}
    conds = [(d, s, o) for d in dbms for s in devs for o in a.offsets]
    t_start = time.monotonic()
    calls = 0
    # A warm-up call, not counted: the first call after the radio's set-up came out wrong once in a while
    bench.power_dbm = dbms[0]
    with bench.lease():
        job = bench.voice_call(a.freq, a.cc, a.tg, calls=1, ambe=ambe, superframes=superframes, background=True)
        time.sleep(START_S)
        wait_flowing(L, timeout=START_WAIT_S)
        job.kill()
    wait_quiet(L)
    try:
        for cond in conds:
            dbm, dev, off = cond
            bench_off = off
            if a.detune:                    # the carrier off + Hz from the receiver = the receiver off - Hz
                L.vfo_set(round(a.freq * 1e6) - off)
                time.sleep(0.3)
                rx = int(L.view(["core"]).get("rx_hz", 0))
                if rx != round(a.freq * 1e6) - off:
                    raise RuntimeError(f"radio tuned to {rx} Hz, not {round(a.freq * 1e6) - off}")
                bench_off = 0
            bench.power_dbm = dbm
            bench.dev_scale = None if dev == 1 else dev
            for _ in range(a.repeat):
                if a.per_call:
                    per_call(a, L, bench, ambe, cond, values, calls, table, out, a.reg)
                    calls += 1
                    continue
                if orig is not None:
                    reg_write(L, a.reg, orig)       # every call starts at the radio's own value
                with bench.lease():
                    job = bench.voice_call(a.freq, a.cc, a.tg, calls=1, ambe=ambe, superframes=superframes,
                                           offset_hz=bench_off, background=True)
                    time.sleep(START_S)
                    if wait_flowing(L, timeout=START_WAIT_S) is None:    # the call never came: say so, count nothing
                        job.kill()
                        print(f"{label(cond)}: the call didn't start on the radio, skipped", flush=True)
                        wait_quiet(L)
                        continue
                    # The order rotates from call to call, so no value always follows the same one
                    k = calls % len(values)
                    calls += 1
                    for v in values[k:] + values[:k]:
                        if v is not None:
                            reg_write(L, a.reg, v)
                        c = measure(a, L, wait_flowing(L))
                        table[(cond, v)] = add(table.get((cond, v)), c)
                        record(a, out, cond, v, c, a.reg)
                    job.kill()              # the rest of the call isn't needed
                wait_quiet(L)
            if all((cond, v) in table for v in values):
                print(f"{label(cond)}: " + "  ".join(f"{name(v)} {show(table[(cond, v)])}" for v in values),
                      flush=True)
    finally:
        if a.detune:                        # the VFO keeps its frequency: put it back on --freq
            L.vfo_set(round(a.freq * 1e6))
        if orig is not None:
            reg_write(L, a.reg, orig)
            print(f"0x{reg:02X} back to 0x{orig:04X}")
        if out:
            out.close()
        bench.remove_run_dirs()
        os.remove(ambe)
        os.rmdir(tmp)
    # Pick the value whose worst cell is best: the one that holds up for transmitters off frequency or deviation
    print("\nvalue      worst cell (where)                                mean bits a frame")
    conds = [k for k in conds if all((k, v) in table for v in values)]
    if not conds:
        sys.exit("no call reached the radio")
    worst = {v: max(conds, key=lambda k: badness(table[(k, v)])) for v in values}
    for v in sorted(values, key=lambda v: badness(table[(worst[v], v)])):
        bits = [table[(k, v)]["bits"] for k in conds if table[(k, v)]["bits"] is not None]   # no-sync cells left out
        print(name(v).ljust(11) + f"{show(table[(worst[v], v)]):>24}  {label(worst[v]):<26}"
              + (f"{sum(bits) / len(bits):.3f}" if bits else "-"))
    print(f"\n{time.monotonic() - t_start:.0f} s")


if __name__ == "__main__":
    main()
