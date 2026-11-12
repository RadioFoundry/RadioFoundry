#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
pa_cal: a radio's DMR PA key-up constants (docs/spec/pa-keyup.md), measured and set in one run.

The radio transmits into a dummy load. A tap through fixed pads feeds an SDR that a bench gateway serves on the bench
line (TCP 7373). The gateway measures every burst's PA ramp itself (`bursts` with `raw` and `ramp`), so no I/Q leaves
it; this tool keys the radio over the debug link and fits the constants from the ramps.

    pa_cal.py --radio PORT --bench HOST --freq 446.05 --pad-db 57 --max-w 5 --allow-rf

One --freq per band with its own PA (--freq 446.05,146.535). Per band:

  1. open loop   the drive alone, written at the window's start, at about six drives from the lowest power level's
                 to the top level's (the levels' own and some between). Each key gives the time that drive takes
                 to bring the output up. One more key checks the board's earliest point.
  2. fit         the key-up law's dead zone, knee and slope from those times (fit()).
  3. verify      every power level with the fitted constants, as the radio will key it. What is left of the error
                 of a level that comes up on its own drive goes back into the knee and slope (refit()); a level too
                 slow for the window gets the boost its own measurements ask for (want(), solve()), an overshoot
                 a longer settle time.
  4. report      the constants as the board file's initializer, the debug link command and a JSON line (--out).

--plot DIR also draws each band (pa_cal_plot.py, needs matplotlib): the ramps at the lowest, a middle and the top power
level before and after, against the slot's timing, and the fit. It keys those levels once more with the board's
constants, and asks the gateway for each burst's envelope (`trace`), when it has that.

Every key is short (--key-s), at a frequency the gateway's session holds, and the drive never goes over the radio's
own top level. The radio's constants, its drive and its lowest power level are restored at the end, also after an
error. Nothing is written to the radio's flash: the constants go into its board file.

`plan()` is the firmware's rule (core/radio/pa_keyup.c) in Python, for the report and the tests.
"""
import argparse
import json
import math
import os
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))

import pa_shape  # noqa: E402

TARGET_US = -250        # the -3 dB point against the first symbol's leading edge (pa-keyup.md 5)
TOL_US = 60
BOOST_TARGET_US = -500  # a boosted level: its own drive sits on the PA's turn-on edge, where a few counts are
                        # half a millisecond, so it is aimed early by that much; the window bounds how early
EDGE_MAX_US = 100       # a level whose key-up moves more than this per 1 % of drive is boosted, late or not
EARLY_MAX_US = -600     # verify: no level up earlier than this (the ramp belongs in the 1.5 ms before the burst)
OVER_MAX_DB = pa_shape.PEAK_DB  # over the level at any point of the key-up
KEY_SETUP_S = 0.25          # a key's set-up and its PTT requests over the debug link, at most
KEY_GAP_S = 0.35            # between the keys of one capture: the radio's key-down, and the next key's set-up
LINK_BACK_S = 15.0          # how long a debug link lost while keying may take to answer again
REPLUG_S = 300.0            # after a key run (--key-run): past LINK_BACK_S the tool asks for a replug and waits this long
OWN_OVER_TOL_DB = 1.0       # over what the level's drive overshoots by itself: the key-up's doing
SYM1_MIN_DB = -3.0
# A level whose output creeps or sags: within 6 dB at once, then its last dB over milliseconds. A level's own key-up
# is timed by when it is within 1 dB for good (pa_shape's up1_ms), less UP1_US: so it is late until it is at its level
# by the first symbol, and gets the boost that brings it there. An open-loop drive (the fit's points) and a key without
# an envelope are timed by the edge: the point CREEP_US after the -6 dB crossing, when the -3 dB point comes later (a
# plain edge's is 0.04 to 0.13 ms after it)
CREEP_US = 150
UP1_US = 100
SYM1_CREEP_MIN_DB = -4.5    # a creeping level's first symbol: under its settled level by the creep, not by the key-up
GUARD_US = 1250             # the slot starts this long before the first symbol (ETSI TS 102 361-1, 4.2)
ON_AIR_DB = "-20"           # on the air: the carrier within 20 dB of the burst's level
OFF_HOLD_MS = 0.3           # off the air: under that level this long
END_MIN_DB = -3.0           # the carrier at the last symbol's end: lower, and the key-down cut the symbol
BOOST_MAX_PCT = 120     # PA_KEYUP_BOOST_MAX_PCT
NAMES = ("dead_pm", "knee_us", "slope_us", "window_us", "settle_us", "pa_us", "ease_pm")
EASE_START_PCT = 80     # the fastest approach to start from: this much of the lowest overshooting level's own drive
EASE_STEP_PCT = 85      # and of itself, each round an eased level still overshoots
FEATURES = ("bursts_raw", "bursts_ramp")


class Stop(Exception):
    pass


# ---- the rule and the fit (pure) ---------------------------------------------------------------------------------

def plan(c, d, top, ref, max_drive, ahead=0):
    """pa_keyup_drives(): c the six constants (a dict), all integer math as the firmware's. Returns a dict with
    boost (0: none), drive, boost_us, pa_us, drive_us. ahead: the board's earliest point, which bounds an eased
    level's start as pa_keyup_plan() does (0: unbounded, as pa_keyup_drives())."""
    dead = ref * c["dead_pm"] // 1000
    over = d - dead if d > dead else 0
    span = top - dead if top > dead else 0
    q = c["knee_us"] * span + c["slope_us"] * over
    lead = q // over if over else c["window_us"]
    b = d
    ease = ref * c.get("ease_pm", 0) // 1000
    if ease and over > ease:
        step = c["slope_us"] * (over - ease) // over
        start = min((c["knee_us"] * span + c["slope_us"] * ease) // ease + step, ahead or 0xFFFF)
        on = min(c["pa_us"], start)
        return {"boost": dead + ease, "drive": d, "boost_us": start, "pa_us": on, "drive_us": min(step, on)}
    if lead > c["window_us"]:
        lead = c["window_us"]
        room = c["window_us"] - c["settle_us"]
        b = min(dead + (q + room - 1) // room, top * BOOST_MAX_PCT // 100, max_drive)
        if ease:
            b = min(b, dead + ease)
    pa = min(c["pa_us"], lead)
    k = {"boost": 0, "drive": d, "boost_us": 0, "pa_us": pa, "drive_us": 0}
    if b > d or pa < lead:
        k["boost"] = max(b, d)
        k["boost_us"] = lead
        k["drive_us"] = min(c["settle_us"] if b > d else pa, pa)
    return k


def fit(points, top, ref):
    """The law's dead zone, knee and slope from open-loop points [(drive, us)]: us is the time the drive, written by
    itself, takes to bring the output up (T(d, d) = knee * (top - dead) / (d - dead) + slope). Returns
    (dead_pm, knee_us, slope_us, rms_us). The dead zone is searched (it is a part of ref, so 0 without one), the
    other two are a line through 1 / (d - dead). Times that don't fall with the drive give no knee: every level is
    keyed the slowest one's time ahead."""
    pts = sorted(points)
    ds, ts = [p[0] for p in pts], [p[1] for p in pts]
    flat = (0, 0, int(round(max(ts))), 0.0)
    if len(pts) < 3 or max(ts) - min(ts) < 2 * TOL_US:
        return flat
    best = None
    pms = range(0, min(65535, (ds[0] - 1) * 1000 // ref) + 1) if ref else (0,)
    for pm in pms:
        dead = ref * pm // 1000
        x = [(top - dead) / (d - dead) for d in ds]
        mx, mt = statistics.fmean(x), statistics.fmean(ts)
        sxx = sum((v - mx) ** 2 for v in x)
        if sxx <= 0:
            continue
        knee = sum((v - mx) * (t - mt) for v, t in zip(x, ts)) / sxx
        slope = mt - knee * mx
        if slope < 0:                       # the line through the origin's side: all knee
            slope, knee = 0.0, sum(v * t for v, t in zip(x, ts)) / sum(v * v for v in x)
        if knee <= 0:
            continue
        err = math.sqrt(statistics.fmean((knee * v + slope - t) ** 2 for v, t in zip(x, ts)))
        if best is None or err < best[3] - 0.5:     # a tie: the smaller dead zone
            best = (pm, knee, slope, err)
    if best is None or best[1] > 65535 or best[2] > 65535:
        return flat
    return best[0], int(round(best[1])), int(round(best[2])), best[3]


def fit_at(points, top, ref, dead_pm):
    """The knee and the slope at a given dead zone, from the open-loop points above it: (knee_us, slope_us), or None
    when under two points are left or they give no knee."""
    dead = ref * dead_pm // 1000
    pts = [(d, t) for d, t in points if d > dead]
    if len(pts) < 2:
        return None
    x = [(top - dead) / (d - dead) for d, _ in pts]
    ts = [t for _, t in pts]
    mx, mt = statistics.fmean(x), statistics.fmean(ts)
    sxx = sum((v - mx) ** 2 for v in x)
    if sxx <= 0:
        return None
    knee = sum((v - mx) * (t - mt) for v, t in zip(x, ts)) / sxx
    slope = mt - knee * mx
    if slope < 0:
        slope, knee = 0.0, sum(v * t for v, t in zip(x, ts)) / sum(v * v for v in x)
    if knee <= 0 or knee > 65535 or slope > 65535:
        return None
    return int(round(knee)), int(round(slope))


def refit(c, rows, top, ref):
    """The knee and the slope again from verified key-ups. rows: [(drive, plan, late_us)], late_us how much later than
    the target the level came up with constants c. A level late by e needed e more at the drive it was coming up on
    (the boost, or its own), so its charge q = knee * span + slope * over was that much short; the two are a least
    squares line through every level's corrected q. Returns (knee_us, slope_us)."""
    dead = ref * c["dead_pm"] // 1000
    span = max(top - dead, 1)
    xs, qs = [], []
    for d, k, late in rows:
        over = d - dead
        if over <= 0:
            continue
        b = k["boost"] if k["boost"] > d else d
        xs.append(over)
        qs.append(c["knee_us"] * span + c["slope_us"] * over + late * (b - dead))
    if not xs:
        return c["knee_us"], c["slope_us"]
    mx, mq = statistics.fmean(xs), statistics.fmean(qs)
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx > 0 and c["knee_us"] != 0:
        slope = sum((x - mx) * (q - mq) for x, q in zip(xs, qs)) / sxx
        knee = (mq - slope * mx) / span
    else:                                   # one level, or a board with no knee: the slope alone
        knee, slope = c["knee_us"], (mq - c["knee_us"] * span) / mx
    if knee < 0:
        knee, slope = 0.0, sum(x * q for x, q in zip(xs, qs)) / sum(x * x for x in xs)
    if slope < 0:
        slope, knee = 0.0, mq / span
    return min(int(round(knee)), 65535), min(int(round(slope)), 65535)


def want(seen, d, limit, target=BOOST_TARGET_US, dead=0):
    """The boost a slow level needs, from what it did at the drives tried so far: seen {drive written at the window's
    start: the -3 dB point, us}, d its own drive (in seen, late). The answer is read off the level's own
    measurements, not off the fitted constants: between the two drives around the target, or along the last two when
    none is early (or late) enough yet. The time falls as 1 / (drive - dead) (the law's shape, dead the dead zone's
    drive by the constants so far), so that is the scale the reading is taken on: a straight line over the drive
    asks for far too much boost near the dead zone."""
    xs = sorted(seen)
    if xs and xs[0] <= dead:
        dead = 0                            # a drive in the dead zone gave a burst: no such scale, the drive's own
    u = lambda b: 1.0 / (b - dead)
    back = lambda v: dead + 1.0 / v
    for lo, hi in zip(xs, xs[1:]):
        if seen[lo] >= target >= seen[hi]:
            if seen[lo] == seen[hi]:
                return lo
            return int(round(back(u(lo) + (u(hi) - u(lo)) * (seen[lo] - target) / (seen[lo] - seen[hi]))))
    if len(xs) < 2:
        return min(d + max(d // 50, 2), limit)
    lo, hi = xs[-2], xs[-1]
    if seen[hi] >= seen[lo]:                # no earlier on the higher drive: a small step up
        return min(hi + max(d // 50, 2), limit)
    v = u(hi) + (u(hi) - u(lo)) * (seen[hi] - target) / (seen[lo] - seen[hi])
    b = back(v) if v > 0 else limit
    # Past the drives tried the reading is a guess, and close over the dead zone a wrong one overshoots by dB: at
    # most twice the last step, so the next round has the target between two measurements
    return max(d, min(int(round(max(hi - 2 * (hi - lo), min(hi + 2 * (hi - lo), b)))), limit))


def solve(own, c, top, ref, max_drive, boosts):
    """The constants that key every level right: own {drive: us} the time a level that comes up on its own drive
    needs from the write, boosts {drive: the boost a slow level needs}. Both are lines in the knee and slope once the
    dead zone is set (own: T(d, d) = us; boosted: T(d, b) = window - settle), so the dead zone is searched and the
    other two are the least squares over the levels, in us. Each candidate is then scored by what the radio would do
    with it (plan()): a level's lead off its time, a boost off the one wanted (as the time that moves the level), and
    a level keyed the other way than measured counts in full. Returns the constants (c with dead_pm, knee_us and
    slope_us replaced) and the worst level's error, us."""
    room = c["window_us"] - c["settle_us"]
    lowest = min(list(own) + list(boosts)) if own or boosts else 0
    best = None
    for pm in range(0, min(65535, (lowest - 1) * 1000 // ref) + 1) if ref and lowest else (0,):
        dead = ref * pm // 1000
        rows = [((top - dead) / (d - dead), 1.0, t) for d, t in own.items()]
        rows += [((top - dead) / (b - dead), (d - dead) / (b - dead), room) for d, b in boosts.items() if b > dead]
        if len(rows) < 2:
            continue
        a11 = sum(r[0] * r[0] for r in rows); a12 = sum(r[0] * r[1] for r in rows); a22 = sum(r[1] * r[1] for r in rows)
        b1 = sum(r[0] * r[2] for r in rows); b2 = sum(r[1] * r[2] for r in rows)
        det = a11 * a22 - a12 * a12
        knee, slope = ((b1 * a22 - b2 * a12) / det, (a11 * b2 - a12 * b1) / det) if abs(det) > 1e-9 else (-1.0, -1.0)
        if knee < 0 or slope < 0:           # one of the two alone
            k1, s1 = b1 / a11 if a11 else 0.0, b2 / a22 if a22 else 0.0
            e1 = sum((r[0] * k1 - r[2]) ** 2 for r in rows); e2 = sum((r[1] * s1 - r[2]) ** 2 for r in rows)
            knee, slope = (k1, 0.0) if e1 <= e2 else (0.0, s1)
        if knee > 65535 or slope > 65535:
            continue
        t = dict(c, dead_pm=pm, knee_us=int(round(knee)), slope_us=int(round(slope)))
        worst = 0.0
        total = 0.0
        for d, us in own.items():
            # The law's time against the level's: past the window both mean a boost, and the error there is what
            # the boost's time moves by
            law = (knee * (top - dead) + slope * (d - dead)) / (d - dead)
            e = abs(law - us) * (room / us if us > c["window_us"] and law > c["window_us"] else 1.0)
            worst, total = max(worst, e), total + e
        for d, b in boosts.items():
            k = plan(t, d, top, ref, max_drive)
            e = room if k["boost"] <= d else abs(k["boost"] - b) * room / max(b - dead, 1)
            worst, total = max(worst, e), total + e
        if best is None or (round(worst), total) < (round(best[1]), best[2] - 0.5):   # a tie: the smaller dead zone
            best = (t, worst, total)
    return best[:2] if best else (c, None)


def edge_us(c, d, top, ref):
    """How far a level's own key-up moves when its drive moves 1 %, us, by the law: the measure of a drive on the
    PA's turn-on edge, where no fixed time holds from one key-up (or one temperature) to the next."""
    dead = ref * c["dead_pm"] // 1000
    over = d - dead
    if over <= 0:
        return 1 << 30
    return (c["knee_us"] * max(top - dead, 0) + c["slope_us"] * over) * d / (100.0 * over * over)


def grid(levels, top, n):
    """The open-loop drives: the levels' own, and drives between them up to n in all, spread evenly over 1 / drive
    (the law is a line there), never over top."""
    ds = sorted({d for d in levels if 0 < d <= top})
    if not ds:
        return []
    # Levels that share one drive (they differ elsewhere, a PA gain): drives under it give the law its span
    low = ds[0] if ds[0] < top else max(top * 6 // 10, 1)
    lo, hi = 1.0 / low, 1.0 / top
    want = [int(round(1.0 / (lo + (hi - lo) * i / (n - 1)))) for i in range(n)] if n > 1 else []
    for d in want:
        if len(ds) >= n:
            break
        if all(abs(d - e) * 25 > e for e in ds):       # not within 4 % of one already there
            ds.append(d)
    return sorted(ds)


FAULT_US = 50       # a level's fault (a knee, a step) in score(), as much as being this late


def score(res):
    """How far a set's verified levels miss (lower is nearer): how late each is, 100 us per dB it runs past the
    overshoot limit and FAULT_US for each level with a fault. Not the count of levels with a fault first: that took a
    set with the lowest level 0.5 ms late over one with a level 0.4 dB further over."""
    return sum(max(m["rise_us"], 0) + 100 * max((m["over_db"] or 0) - OVER_MAX_DB, 0) + (FAULT_US if m["bad"] else 0)
               for m in res.values())


def judge(m, boosted=False, own_db=0.0):
    """What is wrong with a verified level's measurement (a list of words; empty: fine). A boosted level may be
    early: it starts at the window, never before. own_db: the overshoot the level's drive gives written alone (the
    open loop). That much is the radio's power loop, which no key-up constant changes: it is noted, not counted."""
    bad = []
    if m["rise_us"] > 0:
        bad.append("late")
    if m["rise_us"] < EARLY_MAX_US and not boosted:
        bad.append("early")
    if (m["over_db"] or 0) > max(OVER_MAX_DB, own_db + OWN_OVER_TOL_DB):
        bad.append("overshoot")
    if m["sym1_db"] is not None and m["sym1_db"] < (SYM1_CREEP_MIN_DB if m.get("creep") else SYM1_MIN_DB):
        bad.append("sym1")
    if m.get("on_us") is not None and m["on_us"] < -GUARD_US:
        bad.append("before-slot")   # on the air in the slot before, boosted or not
    return bad + [w for w in m.get("faults", []) if w not in bad]


# ---- the bench line and the radio ----------------------------------------------------------------------------------

class Bench:
    """The gateway's side: a session that holds the frequency, and one raw burst capture per key."""

    def __init__(self, a, log):
        import bench_line
        self.bl, self.a, self.log = bench_line, a, log
        self.where = bench_line.address(a.bench)
        self.session = self.keep = None
        info = bench_line.request(self.where, {"op": "info"})
        have = info.get("features", [])
        if isinstance(have, dict):          # name: true or false
            have = [k for k, v in have.items() if v]
        # The envelopes are what the ramp's shape is judged on (pa_shape.py), plot or not
        self.trace = bool(have.get("bursts_trace") if isinstance(have, dict) else "bursts_trace" in have)
        if not self.trace:
            print("  the gateway sends no burst envelopes (bursts_trace): timing only, the ramps' shape isn't judged")
        need = FEATURES + (("rx_port",) if a.rx_port else ())
        missing = [f for f in need if f not in have]
        if missing:
            raise Stop(f"the bench gateway at {a.bench} lacks {', '.join(missing)} (its info lists {', '.join(have)})")

    def open(self, mhz):
        bl = self.bl
        self.keep = bl.Request(self.where, {"op": "session", "label": "pa-cal", "expect_s": 120, "max_s": 600},
                               log=self.log)
        self.keep.first.wait(10)
        if self.keep.error or not self.keep.replies:
            raise Stop(f"no bench session: {self.keep.error or 'no answer'}")
        self.session = self.keep.replies[0]["session"]
        r = bl.request(self.where, {"op": "claim", "session": self.session, "freqs": mhz, "wait_max": self.a.wait_s},
                       timeout=self.a.wait_s + 30)
        if "claimed" not in r:
            raise Stop(f"the bench gateway didn't give {mhz} MHz: {r}")

    def close(self):
        if self.keep is not None:
            self.keep.cancel()
            self.keep = None

    def measure(self, mhz, key):
        """One capture around key() (which keys the radio and returns): the bursts' ramps as one measurement."""
        return summarize(self.capture(mhz, key, self.a.key_s), self.a)

    def measure_many(self, mhz, n, key):
        """One capture around key(), which keys the radio n times with a pause between: a measurement per key, or
        None when the capture doesn't show n keys (the caller keys them one by one). A key starts at a burst with
        none in the 90 ms before it (the record's `first`)."""
        a = self.a
        replies = self.capture(mhz, key, n * (a.key_s + KEY_GAP_S + KEY_SETUP_S) + 0.3)
        rest = [r for r in replies if "ramp" not in r]
        groups = []
        for r in replies:
            if "ramp" in r:
                if r["ramp"].get("first") or not groups:
                    groups.append([])
                groups[-1].append(r)
        if len(groups) != n:
            return None
        return [summarize(g + rest, a) for g in groups]

    def capture(self, mhz, key, key_s):
        """The replies of one raw burst capture around key(), which keys the radio for key_s in all and returns."""
        a = self.a
        req = {"op": "bursts", "session": self.session, "freq": mhz, "seconds": key_s + 0.9 + max(a.arm_s - 1.9, 0.0), "raw": True,
               "ramp": True, "cc": a.cc, "rate": a.rate, "gain": a.gain, "floor_s": 0.2}
        if a.rx_port:
            req["rx_port"] = a.rx_port
        if a.env_bw:
            req["env_bw"] = a.env_bw
        if self.trace:
            req["trace"] = True
        rq = self.bl.Request(self.where, req, log=self.log)
        try:
            rq.first.wait(15)
            if rq.error or not rq.replies or "ready" not in rq.replies[0]:
                raise Stop(f"the capture didn't start: {rq.error or rq.replies[:1]}")
            ready = rq.replies[0]
            if a.rx_port and ready.get("rx_port", a.rx_port) != a.rx_port:
                raise Stop(f"the capture is on receive port {ready.get('rx_port')}, not {a.rx_port}")
            time.sleep(a.arm_s)             # the capture starts some time after its ready, then learns the floor
            key()
            if not rq.wait(key_s + 30):
                raise Stop("the capture didn't end")
            if rq.error and rq.error.code != "no_signal":
                raise Stop(f"the capture failed: {rq.error}")
        finally:
            rq.cancel()
        cap = next((r["captured"] for r in rq.replies if "captured" in r), {})
        if cap.get("gain") is not None and abs(float(cap["gain"]) - a.gain) > 0.5:     # what the chip reports
            raise Stop(f"the capture ran at {cap['gain']} dB of gain, not {a.gain:g}: its levels can't be trusted")
        return rq.replies


def med(v):
    v = [x for x in v if x is not None]
    return statistics.median(v) if v else None


def mid_trace(ramps, name):
    """The bursts' envelopes (`trace` at the burst's start, `tail` at its end) as one: the median, point by point,
    over those on the commonest time base. None without any."""
    ts = [r[name] for r in ramps if r.get(name) and r[name].get("db")]
    if not ts:
        return None
    key = lambda t: (round(t["t0_ms"], 3), round(t["dt_ms"], 5), len(t["db"]))
    best = max({key(t) for t in ts}, key=lambda k: sum(key(t) == k for t in ts))
    ts = [t for t in ts if key(t) == best]
    return {"t0_ms": ts[0]["t0_ms"], "dt_ms": ts[0]["dt_ms"], "n": len(ts),
            "db": [round(statistics.median(v), 1) for v in zip(*(t["db"] for t in ts))]}


def off_air_us(tail):
    """When a key-down's envelope (`tail`, from the last symbol's end) leaves the air: the last point before it stays
    more than ON_AIR_DB under the burst's level for OFF_HOLD_MS, us. A weak signal's noise comes back over that level
    in single points long after the carrier is gone. A signal so weak that the noise after it is within 3 dB of that
    level is judged 6 dB over its noise instead, as long as that is a fall of 6 dB or more. None without an envelope;
    its end when the carrier never left."""
    if not tail or not tail.get("db"):
        return None
    db, n = tail["db"], max(1, int(round(OFF_HOLD_MS / tail["dt_ms"])))
    th = float(ON_AIR_DB)
    floor = statistics.median(db[-max(1, int(round(0.5 / tail["dt_ms"]))):])
    if th - 3.0 < floor <= -12.0:
        th = floor + 6.0
    for i in range(1, len(db) - n + 1):
        if db[i - 1] >= th and all(v < th for v in db[i:i + n]):
            return (tail["t0_ms"] + (i - 1) * tail["dt_ms"]) * 1000.0
    return None if db[0] < th else (tail["t0_ms"] + (len(db) - 1) * tail["dt_ms"]) * 1000.0


def end_db(tail):
    """A key-down's level at the last symbol's end, dB from the burst's. None without an envelope there."""
    if not tail or not tail.get("db"):
        return None
    i = int(round(-tail["t0_ms"] / tail["dt_ms"]))
    return tail["db"][i] if 0 <= i < len(tail["db"]) else None


def overrun(m):
    """What a key-down did wrong: still on the air after the slot's end, or down before the last symbol's. No key-up
    constant moves either: they fail the band and start no refit."""
    bad = ["after-slot"] if m.get("off_us") is not None and m["off_us"] > GUARD_US else []
    return bad + (["cut-short"] if m.get("end_db") is not None and m["end_db"] < END_MIN_DB else [])


def summarize(replies, a):
    """A capture's replies as one measurement: the medians over its bursts, the first of the key-up left out when
    there are others (it follows the radio's whole key-up, not a slot's)."""
    ramps = [r["ramp"] for r in replies if "ramp" in r]
    cap = next((r["captured"] for r in replies if "captured" in r), {})
    done = next((r["done"] for r in replies if "done" in r), {})
    use = [r for r in ramps if not r.get("first")] or ramps
    at = lambda db: med([(r.get("rise_ms") or {}).get(db) for r in use])
    rise3, rise6, on = at("-3"), at("-6"), at(ON_AIR_DB)
    creep = rise6 is not None and (rise3 is None or (rise3 - rise6) * 1000.0 > CREEP_US)
    rise = rise6 + CREEP_US / 1000.0 if creep else rise3
    settled = med([r.get("settled_db") for r in use])
    off = (done.get("envelope") or {}).get("dbm_offset")
    off = a.dbfs_to_dbm if a.dbfs_to_dbm is not None else off
    sh = shape_of(use)
    up1 = None
    if {"creep", "sag"} & set(sh.get("faults", [])) and sh["shape"].get("up1_ms") is not None:
        up1 = max(sh["shape"]["up1_ms"] - UP1_US / 1000.0, rise if rise is not None else -1e9) * 1000.0
    return {"n": len(use), "rise_us": None if rise is None else rise * 1000.0, "creep": creep, "up1_rise_us": up1,
            "rise3_us": None if rise3 is None else rise3 * 1000.0, "on_us": None if on is None else on * 1000.0,
            "over_db": med([r.get("overshoot_db") for r in use]), "sym1_db": med([r.get("sym1_db") for r in use]),
            "settled_dbfs": settled, "dbm": None if settled is None or off is None else settled + off,
            "clip_pct": max([cap.get("clip_pct") or 0] + [1 if r.get("clipped") else 0 for r in use]),
            "peak_dbfs": cap.get("peak_dbfs"), "trace": mid_trace(use, "trace"), "tail": mid_trace(use, "tail"),
            "off_us": off_air_us(mid_trace(use, "tail")), "end_db": end_db(mid_trace(use, "tail")),
            "edge_res_us": ((done.get("envelope") or {}).get("ramp") or {}).get("edge_res_us"), **sh}


def shape_of(use):
    """The bursts' ramp shapes (pa_shape.py): the median envelope's numbers ("shape", "down") and faults ("faults"),
    and how far the bursts differ (the -3 dB point's spread, the worst burst's first-symbol RMS). Empty without
    envelopes."""
    tr, tl = mid_trace(use, "trace"), mid_trace(use, "tail")
    up = pa_shape.key_up(tr, tl)
    if not up:
        return {}
    down = pa_shape.key_down(tl, up[0]["floor_db"])
    each = [pa_shape.key_up(r["trace"], r.get("tail")) for r in use if r.get("trace") and r["trace"].get("db")]
    m3 = [e[0]["m3_ms"] for e in each if e and e[0]["m3_ms"] is not None]
    sh = dict(up[0], m3_sd_us=round(statistics.pstdev(m3) * 1000.0) if len(m3) > 1 else 0,
              worst_rms_db=max([e[0].get("sym_rms_db", 0.0) for e in each if e] or [0.0]))
    faults = up[1] + (down[1] if down else [])
    if sh["m3_sd_us"] > pa_shape.JITTER_US:
        faults.append("jitter")
    return {"shape": sh, "down": down[0] if down else None, "faults": faults}


class Radio:
    """The radio's side, over the debug link."""

    def __init__(self, a):
        import rfoundry_debug
        self.a, self.link = a, rfoundry_debug.DebugLink(a.radio)
        self.err = rfoundry_debug.DebugLinkError
        self.keyrun_max = rfoundry_debug.PA_KEYRUN_MAX
        self.drives = []        # each level's drive at the band's frequency (the view's tx_drive), for --key-run
        self.keep_last = False  # --key-run restart: this key run reattaches (the band's first key)

    def cli(self, cmd):
        """A debug link command line (--arm, --disarm) on its own connection."""
        if not cmd:
            return
        self.link.close()
        try:
            p = subprocess.run([sys.executable, os.path.join(HERE, "..", "rfoundry_debug.py"), "--port", self.a.radio,
                                *cmd.split()], capture_output=True, text=True, timeout=30)
            if p.returncode:
                raise Stop(f"debug link: {cmd}: {p.stderr.strip() or p.stdout.strip()}")
        finally:
            import rfoundry_debug
            self.link = rfoundry_debug.DebugLink(self.a.radio)

    def view(self):
        return self.link.view()

    def ptt_for(self, seconds):
        """One key. A radio whose debug link drops while it transmits (its own RF in the USB cable) still ends the key
        itself: the firmware's PTT time-out is set just past the hold. So a lost release is waited out, the link
        opened again, and the radio checked idle before the run goes on."""
        try:
            self.link.ptt_for(seconds)
        except self.err as e:
            print(f"  the debug link dropped during the key ({e}): waiting for it", flush=True)
            self.reconnect()
            if self.view().get("state") == "tx":
                self.link.ptt(False)
                raise Stop("the radio was still keyed after the debug link came back: released")
            self.drops = getattr(self, "drops", 0) + 1

    def reconnect(self, wait_s=LINK_BACK_S):
        import rfoundry_debug
        t0 = time.monotonic()
        end, told = t0 + wait_s, False
        while True:
            if not told and wait_s > LINK_BACK_S and time.monotonic() - t0 > LINK_BACK_S:
                print(f"  the radio's USB port didn't come back: replug it (waiting {wait_s:g} s)", flush=True)
                told = True
            time.sleep(1.0)
            try:
                self.link.close()
            except Exception:
                pass
            try:
                self.link = rfoundry_debug.DebugLink(self.a.radio)
                self.link.version()
                return
            except (self.err, OSError):
                if time.monotonic() > end:
                    raise Stop(f"the debug link did not come back within {wait_s:g} s: restart the radio")

    def ints(self, v, key, n, least=None):
        try:
            x = [int(s) for s in v[key].split("/")]
        except (KeyError, ValueError):
            x = []
        if least is not None and least <= len(x) < n:
            x += [0] * (n - len(x))     # an older firmware's shorter list: the values it lacks are 0
        if len(x) != n:
            raise Stop(f"the radio's view has no {key} with {n} values ({v.get(key)!r}): its firmware doesn't key "
                       "by this rule, or is older than this tool")
        return x

    def tune(self, mhz):
        a = self.a
        self.mhz = mhz
        self.link.tx_limit(mhz=[mhz])
        if a.channel:                       # a radio without a VFO: the channel it is on, checked
            v = self.view()
            if v.get("mode") != "dmr" or abs(int(v.get("tx_hz", 0)) - round(mhz * 1e6)) > 50:
                raise Stop(f"--channel: the radio's channel is {v.get('mode')} on {int(v.get('tx_hz', 0)) / 1e6:g} MHz, "
                           f"not DMR on {mhz:g}: put it on a DMR channel that transmits there")
            return
        self.link.vfo(True)
        self.link.vfo_set(round(mhz * 1e6))
        self.link.dmr_fields("dmr", a.cc, a.ts)

    def key_run(self, specs, key_s, gap_s):
        """Keys specs [(constants, drive)] by RADIO op 40: the radio keys each run's drives by itself, one run per
        stretch of equal constants, and the link is quiet while it transmits. A link lost during a run is waited out
        (the radio carries on) and opened again; the run is then checked whole by the view's pa_keyrun.last."""
        i = 0
        while i < len(specs):
            c = specs[i][0]
            j = i
            while j < len(specs) and specs[j][0] == c and j - i < self.keyrun_max:
                j += 1
            drives = [d for _, d in specs[i:j]]
            if c is None:
                self.link.pa_keyup()
            cv = None if c is None else [c.get(n, 0) for n in NAMES]
            t0 = time.monotonic()
            run_s = len(drives) * (key_s + gap_s + 1.0)
            try:
                # The radio's USB port comes back after the run by itself: a host that dropped it during the radio's
                # own transmission finds it again with no replug
                # The band's first key reads the top drive and the anchor after it (pa_keyup.last), which a restart
                # clears: that one key reattaches
                restart = self.a.key_run == "restart" and not self.keep_last
                self.link.pa_keyrun(drives, round(key_s * 1000), round(gap_s * 1000), cv, reattach=not restart,
                                    restart=restart)
            except (self.err, OSError) as e:
                self.drops = getattr(self, "drops", 0) + 1
                print(f"  the debug link dropped during a key run ({e}): waiting for the run to end", flush=True)
                time.sleep(max(run_s - (time.monotonic() - t0), 0.0))
            self.reconnect(REPLUG_S)          # a host that ignores the port's return needs a replug
            if restart:
                self.tune(self.mhz)          # a restart cleared the test TX limit and the VFO's test settings
                i = j
                continue                    # and the count: the capture shows the keys
            n, of = (int(x) for x in self.link.view(["features"]).get("pa_keyrun.last", "0/0").split("/")[:2])
            if (n, of) != (len(drives), len(drives)):
                raise Stop(f"the key run keyed {n} of {of} (asked {len(drives)})")
            i = j

    def setup(self, c, level, drive=0):
        self.link.pa_keyup(*([c.get(n, 0) for n in NAMES] if c else []))
        self.link.power_level(level)
        self.link.tx_drive(drive)

    def restore(self):
        try:
            self.link.version()
        except self.err:
            self.reconnect()
        self.link.tx_drive(0)
        self.link.pa_keyup()
        self.link.power_level("lowest")


# ---- the run --------------------------------------------------------------------------------------------------------

class Cal:
    def __init__(self, a):
        self.a = a
        self.rows = []          # every key, for --out
        self.bench = Bench(a, self.say)
        self.radio = Radio(a)
        self.keys_n = 0
        try:
            v = self.radio.link.version()
        except Exception:       # a name for the result and the plots only
            v = {}
        self.who = {"board": v.get("board", ""), "git": v.get("git", "")}

    def say(self, s):
        print(s, flush=True)

    def watts(self, m):
        if m.get("dbm") is None:
            return "-" if m.get("settled_dbfs") is None else f"{m['settled_dbfs']:.1f} dBFS"
        w = 10 ** ((m["dbm"] + self.a.pad_db) / 10) / 1000
        if w < 0.001:   # an over-the-air path: --pad-db isn't its loss, so no power to give
            return "-"
        return f"{w:.1f} W" if w >= 1 else f"{w * 1000:.0f} mW"

    def key_fn(self, specs):
        """What keys specs [(constants, level, drive)] in one capture: by a key run (--key-run), or one at a time"""
        a = self.a
        if a.key_run:
            runs = [(c, d or self.radio.drives[l]) for c, l, d in specs]
            return lambda: self.radio.key_run(runs, a.key_s, KEY_GAP_S)

        def run():
            for c, l, d in specs:
                self.radio.setup(c, l, d)
                self.radio.ptt_for(a.key_s)
                if len(specs) > 1:
                    time.sleep(KEY_GAP_S)
        if len(specs) == 1:
            self.radio.setup(*specs[0])
            return lambda: self.radio.ptt_for(a.key_s)
        return run

    def key(self, mhz, tag, c, level, drive=0, need=True):
        m = self.bench.measure(mhz, self.key_fn([(c, level, drive)]))
        self.keys_n += 1
        if need and not m["n"]:             # one capture without bursts may be the capture's: once more
            self.say(f"  {tag:9s} level {level}: no bursts, keyed again")
            m = self.bench.measure(mhz, self.key_fn([(c, level, drive)]))
            self.keys_n += 1
        return self.note(m, mhz, tag, c, level, drive, need)

    def keys(self, mhz, tag, specs, need=True):
        """Several keys in one capture: specs [(constants, level, drive)]; a measurement each, in order. The capture
        costs more than the keys, so a step's keys go together; if it doesn't show them all they go one by one."""
        a = self.a
        if len(specs) < 2 or not a.batch:
            return [self.key(mhz, tag, c, l, d, need) for c, l, d in specs]

        out = []
        for i in range(0, len(specs), a.batch):
            part = specs[i:i + a.batch]
            ms = self.bench.measure_many(mhz, len(part), self.key_fn(part)) if len(part) > 1 else None
            if ms is None:
                if len(part) > 1:
                    self.keys_n += len(part)
                    self.say(f"  {tag:9s} the capture didn't show {len(part)} keys: one by one")
                out += [self.key(mhz, tag, c, l, d, need) for c, l, d in part]
            else:
                self.keys_n += len(part)
                out += [self.note(m, mhz, tag, c, l, d, need) for m, (c, l, d) in zip(ms, part)]
        return out

    def note(self, m, mhz, tag, c, level, drive, need):
        """A key's measurement into the run's record and the log, with the stops every key is checked for."""
        a = self.a
        m.update(tag=tag, mhz=mhz, level=level, drive=drive, c=c)
        if not drive and m.get("up1_rise_us") is not None:
            # A level's own key-up (not an open-loop drive, whose edge the fit wants) is timed by its arrival
            m.update(rise_us=m["up1_rise_us"], creep=True)
        self.rows.append(m)
        f = lambda v, n=1: "-" if v is None else f"{v:.{n}f}"
        self.say(f"  {tag:9s} level {level} drive {drive or '-':>5} {self.watts(m):>8s}  n={m['n']:2d}  "
                 f"{'edge ' if m.get('creep') else '-3 dB'} {f(m['rise_us'], 0):>6s} us  over {f(m['over_db']):>4s} dB  "
                 f"sym1 {f(m['sym1_db']):>5s} dB  on air {f(m.get('on_us'), 0):>6s} us  off {f(m.get('off_us'), 0):>5s} us")
        if m["clip_pct"]:
            raise Stop(f"the SDR's input clipped ({m['clip_pct']} %): more pads or less gain")
        if m["dbm"] is not None and m["dbm"] > a.max_in_dbm:
            raise Stop(f"{m['dbm']:.1f} dBm into the SDR, over {a.max_in_dbm:g}: more pads")
        if need and (not m["n"] or m["rise_us"] is None):
            v = self.radio.view()
            raise Stop(f"no bursts heard at {mhz:g} MHz (radio tx_hz {v.get('tx_hz')}, tx_deny {v.get('tx_deny')}, "
                       f"txlim.refused {v.get('txlim.refused')}): check the path, the gain and the color code")
        return m

    def band(self, mhz):
        a, radio = self.a, self.radio
        self.say(f"{mhz:g} MHz")
        self.bench.open([mhz])
        radio.tune(mhz)
        v = radio.view()
        eases = len(v.get("pa_keyup", "").split("/")) >= len(NAMES)    # the firmware has the fastest approach
        board = dict(zip(NAMES, radio.ints(v, "pa_keyup", len(NAMES), 6)))
        ahead, max_drive = radio.ints(v, "pa_keyup.limits", 2)
        drives = [int(x) for x in v.get("tx_drive", "").split("/") if x]
        radio.drives = drives
        if not drives or not any(drives):
            raise Stop("the radio has no power calibration here (the view's tx_drive)")
        levels = [l for l in (range(len(drives)) if a.levels == "all" else map(int, a.levels.split(",")))
                  if drives[l]]
        # The first key, at the lowest level with the board's constants: the path, the SDR's input level, and the top
        # drive and the anchor the radio plans against (pa_keyup.last)
        radio.keep_last = True
        try:
            m = self.key(mhz, "check", None, levels[0])
        finally:
            radio.keep_last = False
        v = radio.view()
        last = radio.ints(v, "pa_keyup.last", 7)
        top, ref = last[5], last[6]
        # A board with a set of constants per band selects the band's at the key-up: the view before it showed the
        # band keyed last
        board = dict(zip(NAMES, radio.ints(v, "pa_keyup", len(NAMES), 6)))
        if top == 0 or max(drives[l] for l in levels) > top:
            raise Stop(f"the radio's top drive {top} is under a level's ({drives}): its view is not this key-up's")
        self.say(f"  drives {[drives[l] for l in levels]}, top {top}, anchor {ref}, limits {ahead} us / {max_drive}; "
                 f"board {board}" + (f"; edges to {m['edge_res_us']} us" if m.get("edge_res_us") else ""))

        # Every level as the board keys it now. Constants that hold are kept: a board may time its key-up by more
        # than the rule's constants say (a ramp of its own), and a radio that is right needs no new ones
        shown = sorted({levels[0], levels[len(levels) // 2], levels[-1]})   # for the plots
        old = dict(zip(NAMES, a.before + [0])) if a.plot and a.before else None   # constants to compare with, not the board's
        asis = {levels[0]: m}
        asis.update(zip(levels[1:], self.keys(mhz, "board" if old else "before", [(None, l, 0) for l in levels[1:]],
                                              need=False)))
        m["tag"] = "board" if old else "before"
        for l, x in asis.items():
            x["plan"] = plan(board, drives[l], top, ref, max_drive, ahead)
            # A level keyed at its own drive overshoots by what its drive does. A firmware with the fastest approach
            # cures that (ease_pm), so it counts; on an older one it is the power loop's, no constant's
            boosted = x["plan"]["boost"] > drives[l]
            x["own_over_db"] = 0.0 if boosted or eases else x["over_db"] or 0.0
            x["bad"] = (judge(x, boosted, x["own_over_db"]) if x["n"] and x["rise_us"] is not None else ["none"])
        for l in shown if old else []:
            self.key(mhz, "before", old, l, need=False)["plan"] = plan(old, drives[l], top, ref, max_drive, ahead)
        if not a.refit and not any(x["bad"] for x in asis.values()):
            self.say("  the board's constants hold at every level: kept (--refit measures new ones)")
            self.bench.close()
            for x in asis.values():
                x["after"] = True
            return {"mhz": mhz, "ok": not any(overrun(x) for x in asis.values()), "kept": True, "shown": shown, "window_us": board["window_us"],
                    "target_us": TARGET_US, "constants": board, "board": board, "top": top, "ref": ref,
                    "ahead_us": ahead, "reach_us": ahead, "max_drive": max_drive, "fit_rms_us": None, "open": [],
                    "levels": {l: {"drive": drives[l], "plan": x["plan"], "rise_us": x["rise_us"],
                                   "over_db": x["over_db"], "sym1_db": x["sym1_db"], "power": self.watts(x),
                                   "creep": x.get("creep"), "rise3_us": x.get("rise3_us"), "on_us": x.get("on_us"),
                                   "off_us": x.get("off_us"), "end_db": x.get("end_db"), "bad": overrun(x), "own_over_db": x["own_over_db"], "shape": x.get("shape"), "faults": x.get("faults", [])} for l, x in asis.items()}}

        window = min(a.window_us or board["window_us"] or ahead, ahead)
        # Open loop at the window the radio will use, not at the board's earliest point: the same points of its burst
        # sequence as a real key-up, so whatever the sequence adds is in the times
        fixed = lambda w: {"dead_pm": 0, "knee_us": 0, "slope_us": w, "window_us": w, "settle_us": 0, "pa_us": 0xFFFF,
                           "ease_pm": 0}
        pts, top_rise, own_over = [], None, {}
        limit = top * a.max_pct // 100
        ds = grid([drives[l] for l in levels if drives[l] <= limit], limit, a.points)
        specs = [(fixed(window), max([l for l in levels if drives[l] <= d] or levels[:1]), d) for d in ds]
        for d, m in zip(ds, self.keys(mhz, "open", specs, need=False)):
            if m["n"] and m["rise_us"] is not None:
                pts.append((d, window + m["rise_us"] - TARGET_US))
                own_over[d] = m["over_db"] or 0.0
                top_rise = m["rise_us"]
        # The board's earliest point, checked: the highest drive again from there has to come up that much earlier
        reach = ahead
        if ahead > window and top_rise is not None:
            m = self.key(mhz, "limit", fixed(ahead), levels[-1], pts[-1][0], need=False)
            if m["rise_us"] is not None:
                reach = int(round(window + top_rise - m["rise_us"]))
                if reach < ahead - 2 * TOL_US:
                    self.say(f"  the board's earliest point is {ahead} us, but a drive written there acts {reach} us ahead: "
                             "its burst sequence can't run a point that early")
        if len(pts) < 2:
            raise Stop("under two drives gave bursts: nothing to fit")
        dead_pm, knee, slope, rms = fit(pts, top, ref)
        if knee == 0 and slope > window:
            # No level comes up faster on a higher drive, so a boost gains nothing and only overshoots: every level
            # on its own drive from the window's start, which is what the open loop just measured
            self.say(f"  the times don't fall with the drive, and the slowest needs {slope} us: no boost, "
                     f"every level from the window's start ({window} us)")
            slope = window
        c = {"dead_pm": dead_pm, "knee_us": knee, "slope_us": slope, "window_us": window,
             "settle_us": min(a.settle_us if a.settle_us is not None else board["settle_us"] or window // 5,
                              max(window - 1, 0)),
             "pa_us": a.pa_us if a.pa_us is not None else board["pa_us"], "ease_pm": 0}
        # A level that runs far past its power on its own drive came up too fast: the fastest approach starts under
        # the lowest such level's drive, and every level over it comes up on that drive first (pa-keyup.md 2)
        if a.ease_pm is not None:
            c["ease_pm"] = a.ease_pm
        elif eases and ref:
            dead = ref * dead_pm // 1000
            fast = [d for d in sorted(own_over) if own_over[d] > OVER_MAX_DB - 0.5 and d > dead
                    and d in [drives[l] for l in levels]]
            if fast:
                c["ease_pm"] = max((fast[0] - dead) * EASE_START_PCT * 10 // ref, 1)
                self.say(f"  drive {fast[0]} runs {own_over[fast[0]]:.1f} dB past its level written alone: the fastest "
                         f"approach is {c['ease_pm']}/1000 of the anchor over the dead zone")
        if eases:
            own_over = {}       # no level is excused its own drive's overshoot: the approach is the cure
        self.say(f"  fit: dead {dead_pm}/1000 of the anchor ({ref * dead_pm // 1000}), knee {knee} us, slope {slope} us "
                 f"(rms {rms:.0f} us over {len(pts)} drives)")

        # What each level did at each drive written at the window's start: its own (the open loop) and every boost
        seen = {l: {} for l in levels}
        for d, t in pts:
            for l in levels:
                if drives[l] == d:
                    seen[l][d] = t - window + TARGET_US
        late = TARGET_US + 2 * TOL_US      # a level this late on its own drive, once, is slow: it gets a boost
        # and so does one on the turn-on edge (edge_us), by the first fit: its own time doesn't hold
        edgy = [l for l in levels if edge_us(c, drives[l], top, ref) > EDGE_MAX_US]
        if edgy and len(edgy) < len(levels):
            self.say("  on the PA's turn-on edge, boosted whatever their own time: level " +
                     ", ".join(str(l) for l in edgy))
            limit = min(top * BOOST_MAX_PCT // 100, max_drive)
            base = {d: t for d, t in pts if t <= window + TOL_US and d in [drives[l] for l in levels]
                    and d not in [drives[l] for l in edgy]}
            if base:
                c, _ = solve(base, c, top, ref, max_drive,
                             {drives[l]: want(seen[l], drives[l], limit, dead=ref * c["dead_pm"] // 1000) for l in edgy})
            else:
                edgy = []
        else:
            edgy = []
        ok, res, good, near, slow_set = False, {}, None, None, set()
        eased = lambda cc, l: 0 < plan(cc, drives[l], top, ref, max_drive, ahead)["boost"] < drives[l]
        for rnd in range(a.rounds + 1):
            own, res, over, fast = [], {}, 0.0, False
            for l, m in zip(levels, self.keys(mhz, "verify", [(c, l, 0) for l in levels])):
                d = drives[l]
                k = plan(c, d, top, ref, max_drive, ahead)
                b = max(k["boost"], d)
                m["bad"] = judge(m, b > d, own_over.get(d, 0.0))
                m["own_over_db"] = own_over.get(d, 0.0)
                res[l] = m
                if 0 < k["boost"] < d:
                    # Eased: its time is the approach drive's, not its own, so it is not a point of the law's line
                    fast = fast or "overshoot" in m["bad"]
                elif b > d:
                    seen[l][b] = m["rise_us"]
                    # An overshoot with the level up early is too much boost, which the next round takes back; only
                    # one that is on time needs the boost to end earlier
                    if "overshoot" in m["bad"] and m["rise_us"] >= (BOOST_TARGET_US if l in edgy else TARGET_US) - TOL_US:
                        over = max(over, (m["over_db"] or 0) - OVER_MAX_DB)
                    if b >= min(top * BOOST_MAX_PCT // 100, max_drive) and m["rise_us"] > 0:
                        m["bad"].append("window")   # at the boost's ceiling and late: nothing in the constants helps
                else:
                    # as keyed, lead ahead: what the drive would do from the window's start, the latest seen
                    # (the drive's own point: it may lead the PA's, on a board whose PA point is later than the window)
                    at = m["rise_us"] - (window - k["boost_us" if k["boost"] else "pa_us"])
                    seen[l][d] = max(seen[l].get(d, at), at)
                    own.append((d, k, m["rise_us"] - TARGET_US))
            ok = not any(m["bad"] for m in res.values())
            latest = lambda r: max(m["rise_us"] for m in r.values())
            if not ok and (near is None or score(res) < score(near[1])):
                near = (c, res)             # the nearest miss, should no round hold
            if ok and (good is None or latest(res) < latest(good[1])):
                good = (c, res)             # constants that hold: a round after them may only make it better
            elif good:
                # More margin for a boosted level cost something else (a boost's overshoot, or its latest level's
                # time): the set that held best stands
                c, res = good
                ok = True
                self.say(f"  that round was worse: back to dead {c['dead_pm']}, knee {c['knee_us']} us, "
                         f"slope {c['slope_us']} us, settle {c['settle_us']} us")
                break
            # A level gets its boost read off its own measurements (below) once it is on the edge, or slow on its
            # own drive and not right as the law keys it; it stays one. The others keep the law's line
            slow_set |= {l for l in levels if l in edgy or (seen[l].get(drives[l], TARGET_US) > late and res[l]["bad"])}
            slow = sorted(slow_set)
            # What each level that comes up on its own drive needs from the write, by its latest measurement
            own_us = {drives[l]: window + seen[l][drives[l]] - TARGET_US for l in levels
                      if l not in slow and drives[l] in seen[l]}
            # With boosted levels the others are left on the law's line: every level up with room to spare is the end
            # and no overshoot within half a dB of the limit, which the next key-up may cross
            done = ok and all((res[l]["rise_us"] <= -TOL_US and
                               (res[l]["over_db"] or 0) <= max(OVER_MAX_DB - 0.5, res[l]["own_over_db"] + OWN_OVER_TOL_DB))
                              if slow or eased(c, l) else abs(res[l]["rise_us"] - TARGET_US) <= TOL_US for l in levels)
            if done or rnd == a.rounds:
                break
            # Two errors, two cures. A level too slow for the window is boosted, and close over the dead zone the
            # loop answers a boost far more steeply than the law's line says: the boost it needs is read off its own
            # measurements (want()), and the constants are solved to give exactly that boost (solve()). The levels
            # that come up on their own drive keep the line through the open-loop points; without slow levels what
            # is left of their error goes back into the knee and slope (refit())
            was = c
            c = dict(c)
            if slow:
                if over:                    # an overshoot needs the time to die: the boost ends earlier
                    # (100 us, and as much again per 3 dB over the limit: a boost far too long is not cured in steps)
                    c["settle_us"] = min(c["settle_us"] + 100 * min(4, 1 + int(over // 3)), c["window_us"] * 2 // 3)
                    # What the boosts did so far was with a longer boost: the same charge now takes a higher one
                    dead = ref * was["dead_pm"] // 1000
                    r0, r1 = was["window_us"] - was["settle_us"], c["window_us"] - c["settle_us"]
                    for l in slow:
                        seen[l] = {(b if b == drives[l] else dead + (b - dead) * r0 // r1): t
                                   for b, t in seen[l].items()}
                limit = min(top * BOOST_MAX_PCT // 100, max_drive)
                # An edge level is aimed early, for the room its jitter needs; a level that is only slow at the target
                c, off = solve(own_us, c, top, ref, max_drive,
                               {drives[l]: want(seen[l], drives[l], limit, BOOST_TARGET_US if l in edgy else TARGET_US,
                                                 ref * was["dead_pm"] // 1000) for l in slow})
            elif any(abs(r[2]) > TOL_US for r in own):
                c["knee_us"], c["slope_us"] = refit(c, own, top, ref)
            if c["ease_pm"] and ref:
                # The approach drive stays where it was measured when the dead zone under it moves; an eased level
                # that still runs past its power gets a slower one
                over = ref * was["dead_pm"] // 1000 + ref * was["ease_pm"] // 1000 - ref * c["dead_pm"] // 1000
                c["ease_pm"] = max(-(-max(over, 1) * 1000 // ref) * (EASE_STEP_PCT if fast else 100) // 100, 1)
            if c == was:
                break                       # nothing left to change
            self.say(f"  round {rnd + 1}: dead {c['dead_pm']}, knee {c['knee_us']} us, slope {c['slope_us']} us, "
                     f"settle {c['settle_us']} us" + (f", ease {c['ease_pm']}" if c["ease_pm"] else ""))
        if not ok and near:
            c, res = near                   # no round held: the nearest one is the result to look at
        if not ok and not old and all(x["n"] and x["rise_us"] is not None for x in asis.values()) and \
                score(asis) <= score(res):
            # Nor is it nearer than the board's own: those stand
            self.say("  no round is nearer than the board's constants: they stand")
            c, res = board, asis
            for l in levels:
                res[l].setdefault("own_over_db", 0.0)
        if a.confirm and ok:
            for l, m in zip(levels, self.keys(mhz, "confirm", [(c, l, 0) for l in levels])):
                boosted = plan(c, drives[l], top, ref, max_drive, ahead)["boost"] > drives[l]
                again = judge(m, boosted, own_over.get(drives[l], 0.0))
                if again or (not boosted and abs(m["rise_us"] - res[l]["rise_us"]) > 100):
                    ok = False
                    self.say(f"  level {l} doesn't repeat ({' '.join(again) or 'moved'}): {res[l]['rise_us']:.0f} us, "
                             f"{res[l]['over_db'] or 0:.1f} dB over, then {m['rise_us']:.0f} us, {m['over_db'] or 0:.1f} dB")
        self.bench.close()
        for l in levels:
            res[l]["after"] = True      # the measurement the result stands on, for the plots
        return {"mhz": mhz, "ok": ok and not any(overrun(res[l]) for l in levels), "shown": shown, "window_us": window, "target_us": TARGET_US, "constants": c, "board": board, "top": top, "ref": ref, "ahead_us": ahead, "reach_us": reach,
                "max_drive": max_drive, "fit_rms_us": rms, "open": pts,
                "levels": {l: {"drive": drives[l], "plan": plan(c, drives[l], top, ref, max_drive, ahead),
                               "rise_us": res[l]["rise_us"], "over_db": res[l]["over_db"], "sym1_db": res[l]["sym1_db"],
                               "power": self.watts(res[l]), "bad": res[l]["bad"] + overrun(res[l]),
                               "off_us": res[l].get("off_us"), "end_db": res[l].get("end_db"),
                               "creep": res[l].get("creep"), "rise3_us": res[l].get("rise3_us"),
                               "on_us": res[l].get("on_us"),
                               "own_over_db": res[l]["own_over_db"], "shape": res[l].get("shape"), "faults": res[l].get("faults", [])} for l in levels}}

    def run(self):
        a, t0, out = self.a, time.monotonic(), []
        top_in = 10 * math.log10(a.max_w * 1000) - a.pad_db
        if top_in > a.max_in_dbm:
            raise Stop(f"{a.max_w:g} W through {a.pad_db:g} dB is {top_in:.1f} dBm into the SDR, over {a.max_in_dbm:g}: "
                       f"add {math.ceil(top_in - a.max_in_dbm)} dB of pads, or leave the top levels out (--levels)")
        self.radio.cli(a.arm)
        for mhz in a.freq:
            r = self.band(mhz)
            out.append(r)
            c = r["constants"]
            self.say(f"  {'PASS' if r['ok'] else 'CHECK'} {mhz:g} MHz")
            for l, x in r["levels"].items():
                note = (f" (its drive's own overshoot, {x['own_over_db']:.1f} dB written alone: the PA's loop, not the key-up)"
                        if x["own_over_db"] > OVER_MAX_DB and "overshoot" not in x["bad"] else "")
                if x.get("creep"):
                    note += (f" (creeps: -3 dB at {x['rise3_us']:+.0f} us, first symbol {x['sym1_db']:.1f} dB; timed by "
                             f"its edge)" if x.get("rise3_us") is not None else " (creeps; timed by its edge)")
                self.say(f"    level {l} ({x['power']}): {'edge' if x.get('creep') else '-3 dB'} {x['rise_us']:+.0f} us, "
                         f"over {x['over_db'] or 0:.1f} dB, on air {x.get('on_us') or 0:+.0f} us"
                         + (f", off {x['off_us']:+.0f} us" if x.get("off_us") is not None else "") +
                         f"  {' '.join(x['bad']) or 'OK'}{note}")
                sh = x.get("shape")
                if sh:
                    self.say(f"      {pa_shape.line(sh, x.get('faults', []))}; bursts: -3 dB spread {sh['m3_sd_us']} us")
                    self.say(f"      {pa_shape.bins_line(sh)}")
            self.say("    { " + ", ".join(f".{n} = {c[n]}" for n in NAMES) + " }")
            self.say("    rfoundry_debug.py radio pa-keyup " + " ".join(str(c[n]) for n in NAMES))
        self.say(f"{self.keys_n} keys, {time.monotonic() - t0:.0f} s")
        return out

    def end(self):
        self.bench.close()
        try:
            self.radio.restore()
            self.radio.cli(self.a.disarm)
            self.say("the radio's constants, drive and lowest power level restored")
        except Exception as e:      # the link gone: say so, the radio restarts with its own
            self.say(f"RESTORE FAILED ({e}): restart the radio")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--radio", required=True, help="the radio's debug link port (a device or a pyserial URL)")
    ap.add_argument("--bench", required=True, help="the bench gateway, HOST[:PORT] (the bench line, 7373)")
    ap.add_argument("--freq", required=True, type=lambda s: [float(x) for x in s.split(",")],
                    help="MHz, one per band with its own PA; each a frequency the radio may key here, into the load")
    ap.add_argument("--allow-rf", action="store_true", help="required: the radio transmits")
    g = ap.add_argument_group("the path")
    g.add_argument("--pad-db", type=float, required=True, help="the pads between the radio and the SDR")
    g.add_argument("--max-w", type=float, required=True, help="the radio's highest output, W (checked before any key)")
    g.add_argument("--max-in-dbm", type=float, default=-13.0, help="never more than this into the SDR")
    g.add_argument("--dbfs-to-dbm", type=float, default=None,
                   help="dBm at the SDR's input = dBFS + this, at --gain (default: the gateway's own figure, if any)")
    g.add_argument("--rx-port", type=int, default=0, help="the SDR's receive port the pads feed (default: its own)")
    g.add_argument("--gain", type=float, default=0.0, help="the capture's receive gain, dB, manual")
    g.add_argument("--rate", type=int, default=1200000, help="the raw capture's rate, S/s")
    g.add_argument("--env-bw", type=int, default=0, help="the ramp's bandwidth, Hz (default: the gateway's)")
    g.add_argument("--arm-s", type=float, default=1.9, help="from the capture's ready to the key, s (a raw capture starts about 1.5 s after it)")
    g.add_argument("--wait-s", type=float, default=60.0, help="how long to wait for the frequency on the bench")
    g = ap.add_argument_group("the run")
    g.add_argument("--levels", default="all", help="power level numbers, low to high (default all)")
    g.add_argument("--max-pct", type=int, default=100, help="open-loop drives up to this %% of the top level's (<= 100)")
    g.add_argument("--points", type=int, default=6, help="open-loop drives")
    g.add_argument("--batch", type=int, default=6,
                   help="keys per capture (0 or 1: a capture each); a step's keys share one, which is most of a key's time")
    g.add_argument("--key-s", type=float, default=0.5, help="each key, s (about 8 bursts)")
    g.add_argument("--key-run", nargs="?", const="reattach", choices=("reattach", "restart"),
                   help="the radio keys each capture's keys by itself (RADIO op 40), the debug link quiet while it "
                        "transmits, then its USB port leaves the bus and comes back (reattach, the default) or the "
                        "radio restarts (restart): for a radio whose own RF upsets its USB link")
    g.add_argument("--refit", action="store_true",
                   help="measure new constants even when the board's hold at every level")
    g.add_argument("--rounds", type=int, default=4, help="verify rounds that may change the constants")
    g.add_argument("--no-confirm", dest="confirm", action="store_false", help="skip the last pass that checks they repeat")
    g.add_argument("--window-us", type=int, default=0, help="the window (default: the board's, within its limit)")
    g.add_argument("--settle-us", type=int, default=None, help="the settle time to start from (default: the board's)")
    g.add_argument("--pa-us", type=int, default=None, help="the PA's point (default: the board's)")
    g.add_argument("--ease-pm", type=int, default=None,
                   help="the fastest approach, in 1/1000 of the anchor over the dead zone (default: measured; 0: none)")
    g.add_argument("--channel", action="store_true",
                   help="key on the channel the radio is on (a radio without a VFO); --cc is that channel's")
    g.add_argument("--cc", type=int, default=1)
    g.add_argument("--ts", type=int, default=1)
    g.add_argument("--arm", default="", help="a debug link command before the first key (a radio with a TX arm option)")
    g.add_argument("--disarm", default="", help="and after the last")
    g.add_argument("--out", default="", help="a JSON file: the result and every key's measurement")
    g.add_argument("--plot", default="", metavar="DIR", help="draw each band there (PNG; needs matplotlib)")
    g.add_argument("--before", default=None, type=lambda s: [int(x) for x in s.replace(",", " ").split()],
                   help="the plots' \"before\": these six constants instead of the board's (dead knee slope window settle pa)")
    a = ap.parse_args(argv)
    if not a.allow_rf:
        ap.error("the radio transmits: give --allow-rf when it is on a dummy load and the frequency is yours to key")
    if a.before is not None and len(a.before) != 6:
        ap.error("--before takes six constants")
    if not 1 <= a.max_pct <= 100 or a.points < 3 or not 0.2 <= a.key_s <= 5:
        ap.error("--max-pct 1..100, --points 3 or more, --key-s 0.2..5")
    cal, res, rc = None, [], 1
    try:
        cal = Cal(a)
        res = cal.run()
        rc = 0 if res and all(r["ok"] for r in res) else 1
    except Stop as e:
        print(f"STOPPED: {e}")
        rc = 2
    except Exception as e:      # the bench line's and the debug link's own errors, by their text
        print(f"STOPPED: {type(e).__name__}: {e}")
        rc = 2
    finally:
        if cal is not None:
            cal.end()
            if a.out:
                with open(a.out, "w") as f:
                    json.dump({"radio": cal.who, "bands": res, "keys": cal.rows}, f, indent=1)
            if a.plot and res:
                try:
                    import pa_cal_plot
                    for f in pa_cal_plot.draw({"radio": cal.who, "bands": res, "keys": cal.rows}, a.plot):
                        print(f"plot: {f}")
                except ImportError as e:
                    print(f"no plots: {e} (pip install matplotlib)")
    return rc


if __name__ == "__main__":
    sys.exit(main())
