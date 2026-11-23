#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Transmit power calibration with a wattmeter (docs/testing/power-calibration.md).

    tools/power_cal.py --allow-rf MHZ key STEP...            key each step, ask for the meter's reading
    tools/power_cal.py --allow-rf MHZ zero --hi D --lo D      bisect for the zero point and the cliff
    tools/power_cal.py --allow-rf MHZ check --margin M        the short check: every level, then the margin

A STEP is a PA drive in the board's units (debug link RADIO op 36), or Ln for power level n as the radio drives it;
STEP:S keys it for S seconds (default 2). The radio goes to the VFO on MHZ, FM, no sub-audio tone, with the test TX
limit on MHZ alone. --no-prompt keys the steps without asking (another person reads the meter); check then takes the readings
with --readings W,W,... afterwards. Each reading is the average power at the end of the key-up, in watts.
"""
import argparse
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rfoundry_debug as dl  # noqa: E402

GAP_S = 3.0             # between key-ups: the PA and the supply recover
DARK_W = 0.02           # under this the carrier is off: between the cliff and the zero point
RESOLUTION = 5          # bisection stops when each bound is known to this many counts


def parse_watts(name):
    """A level name in watts ("50mW" 0.05, "25W" 25), None when the name holds no power"""
    m = re.fullmatch(r"\s*([0-9.]+)\s*(m?)W\s*", name or "")
    return float(m.group(1)) / (1000 if m.group(2) else 1) if m else None


def db(a, b):
    return 10 * math.log10(a / b) if a > 0 and b > 0 else float("-inf")


def in_tolerance(watts, target):
    """The short check's tolerance: ±1 dB, ±2 dB under 1 W"""
    return abs(db(watts, target)) <= (2.0 if target < 1.0 else 1.0)


class Radio:
    def __init__(self, link, mhz):
        self.link, self.hz = link, round(mhz * 1e6)

    def view(self):
        return self.link.view(["core"])

    def wait(self, pred, what, t=6.0):
        end = time.monotonic() + t
        while time.monotonic() < end:
            v = self.view()
            if pred(v):
                return v
            time.sleep(0.2)
        raise SystemExit(f"power_cal: timeout waiting for {what}")

    def setup(self):
        if self.view().get("vfo") != "1":
            self.link.vfo(True)
            self.wait(lambda v: v.get("vfo") == "1", "the VFO")
        try:
            self.link.dmr_fields(mode="fm")
        except dl.DebugLinkError:
            pass            # a build without DMR: FM already
        self.wait(lambda v: v.get("mode") == "fm", "FM")
        self.link.tone(1, "none", 0)
        self.link.tone(2, "none", 0)
        self.link.vfo_set(self.hz)
        self.wait(lambda v: v.get("rx_hz") == str(self.hz), "the frequency")
        self.link.tx_limit(mhz=[self.hz / 1e6])

    def finish(self):
        try:
            self.link.ptt(False)
            self.link.tx_drive(0)
            # The level is set back once the transmission is over (an FM tail runs on after PTT)
            self.wait(lambda v: v.get("state") not in ("tx", "tx_tail"), "the end of the transmission")
            self.set_level(0)
        finally:
            self.link.tx_limit()

    def levels(self):
        """[(name, drive)] per power level at this frequency (the core view's power names and tx_drive)"""
        v = self.view()
        n = int(v.get("power_count", "0"))
        drives = [int(x) for x in v.get("tx_drive", "").split("/") if x]
        if len(drives) != n:
            raise SystemExit("power_cal: the radio has no tx_drive (firmware without RADIO op 36 for this board)")
        names = []
        for lv in range(n):
            self.set_level(lv)
            names.append(self.view().get("power_name", "?"))
        return list(zip(names, drives))

    def set_level(self, n):
        for _ in range(12):
            p = int(self.view()["power"])
            if p == n:
                return
            self.link.power_step(1 if n > p else -1)
            time.sleep(0.3)
        raise SystemExit(f"power_cal: power level {n} not set")

    def key(self, drive, hold, level=0):
        """One key-up: the level, the drive override (0: the level's), PTT for hold seconds"""
        self.set_level(level)
        self.link.tx_drive(drive)
        self.link.ptt(True, int(hold * 1000) + 2500)
        try:
            time.sleep(0.4)
            st = self.view().get("state")
            time.sleep(max(0.0, hold - 0.4))
        finally:
            self.link.ptt(False)
        if st != "tx":
            raise SystemExit(f"power_cal: the radio didn't transmit (state {st}): TX limit, calibration?")


class Session:
    def __init__(self, radio, prompt):
        self.radio, self.prompt, self.n = radio, prompt, 0
        self.rows = []

    def step(self, label, drive, level=0, hold=2.0):
        if self.n:
            time.sleep(GAP_S)
        self.n += 1
        print(f"step {self.n}: {label}", flush=True)
        self.radio.key(drive, hold, level)
        w = None
        if self.prompt:
            while w is None:
                try:
                    w = float(input("  watts? "))
                except ValueError:
                    print("  a number in watts")
        self.rows.append((self.n, label, drive, w))
        return w


def cmd_key(r, s, args):
    for st in args.steps:
        st, _, hold = st.partition(":")
        hold = float(hold or 2.0)
        if hold <= 0 or hold > 8:
            raise SystemExit("power_cal: a key-up lasts 0-8 s")
        if st.upper().startswith("L"):
            s.step(f"level {int(st[1:])}", 0, int(st[1:]), hold)
        else:
            s.step(f"drive {int(st)}", int(st), 0, hold)


def cmd_zero(r, s, args):
    """Bisect between hi (on the curve) and lo (past the cliff). A reading is on the curve (a new hi), dark (between the
    cliff and the zero point) or past the cliff (a new lo). Once a dark drive is known, the zero point is bisected
    between it and hi, the cliff between lo and the lowest dark drive"""
    if not s.prompt:
        raise SystemExit("power_cal: zero needs the readings as it goes (no --no-prompt)")
    hi, lo = args.hi, args.lo
    dark_hi = dark_lo = None

    def probe(d):
        nonlocal hi, lo, dark_hi, dark_lo
        w = s.step(f"drive {d}", d)
        if w >= args.cliff_w:
            lo = d
        elif w < DARK_W:
            dark_hi = d if dark_hi is None else max(dark_hi, d)
            dark_lo = d if dark_lo is None else min(dark_lo, d)
        else:
            hi = d

    while dark_hi is None and hi - lo > RESOLUTION:
        probe((hi + lo) // 2)
    if dark_hi is None:
        print(f"no dark drive between the cliff ({lo}) and the curve ({hi}): no margin, "
              f"the lowest level stays above {hi}")
        return 1
    while hi - dark_hi > RESOLUTION:
        probe((hi + dark_hi) // 2)
    while dark_lo - lo > RESOLUTION:
        probe((dark_lo + lo) // 2)
    print(f"zero point Z = {dark_hi} (dark; {hi} on the curve), cliff C = {lo} ({dark_lo} dark)")
    return 0


def cmd_check(r, s, args):
    lv = r.levels()
    low = lv[0][1] - args.margin + 5        # 5 counts above the model's highest cliff: dark on a radio like it
    if args.readings:                       # the readings of an earlier --no-prompt run: judged, nothing keyed
        watts = [float(x) for x in args.readings.split(",")]
        if len(watts) != len(lv) + 1:
            raise SystemExit(f"power_cal: {len(lv) + 1} readings expected")
    else:
        for i, (name, drive) in enumerate(lv):
            s.step(f"level {i} {name} (drive {drive})", 0, i)
        s.step(f"lowest level - margin + 5 (drive {low})", low)
        if not s.prompt:
            print("then: check --margin M --readings W,W,... (the levels in order, then the margin step; keys nothing)")
            return 0
        watts = [w for *_, w in s.rows]
    ok = True
    for (name, drive), w in zip(lv, watts):
        t = parse_watts(name)
        good = t is None or in_tolerance(w, t)
        ok &= good
        print(f"{name:>8} drive {drive:5}: {w:7.2f} W {'' if t is None else f'{db(w, t):+5.1f} dB'}"
              f"  {'ok' if good else 'OUT'}")
    good = watts[-1] < DARK_W
    ok &= good
    verdict = "ok" if good else "OUT (cliff higher: full procedure 3.2, 3.7)"
    print(f"  margin drive {low:5}: {watts[-1]:7.2f} W  {verdict}")
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default as rfoundry_debug.py)")
    ap.add_argument("--allow-rf", action="store_true", help="required: the radio transmits into the wattmeter")
    ap.add_argument("--no-prompt", action="store_true", help="key the steps without asking for readings")
    ap.add_argument("mhz", type=float, help="the transmit frequency")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("key", help="key each step")
    p.add_argument("steps", nargs="+", help="DRIVE or Ln (power level n), optionally :SECONDS")
    p = sub.add_parser("zero", help="bisect for the zero point and the cliff (procedure 3.2)")
    p.add_argument("--hi", type=int, required=True, help="a drive on the curve (the lowest factory row)")
    p.add_argument("--lo", type=int, required=True, help="a drive past the cliff (half of --hi)")
    p.add_argument("--cliff-w", type=float, default=5.0, help="a reading at least this is past the cliff (default 5)")
    p = sub.add_parser("check", help="the short check (procedure 4)")
    p.add_argument("--margin", type=int, required=True, help="the model's margin in drive counts")
    p.add_argument("--readings", help="judge these readings (watts, comma separated) from an earlier --no-prompt run; "
                   "keys nothing")
    args = ap.parse_args()
    if not args.allow_rf:
        raise SystemExit("power_cal: transmits; pass --allow-rf (a dummy load and a wattmeter on the radio)")
    link = dl.DebugLink(args.port)
    r = Radio(link, args.mhz)
    s = Session(r, not args.no_prompt)
    rc = 0
    try:
        r.setup()
        rc = {"key": cmd_key, "zero": cmd_zero, "check": cmd_check}[args.cmd](r, s, args) or 0
    finally:
        r.finish()
    if s.prompt and args.cmd == "key":
        print("drive/level, watts: " + ", ".join(f"{lb.split()[-1]} {w}" for _, lb, _, w in s.rows))
    return rc


if __name__ == "__main__":
    sys.exit(main())
