#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
pa_shape: what a DMR burst's PA ramp looks like, in numbers and named faults (docs/spec/pa-keyup.md 6.3), from the
envelopes the bench gateway sends (`trace` about the burst's start, `tail` about its end: dB from the burst's settled
level, 0 ms at the first symbol's leading edge / the last symbol's trailing edge; RFG docs/spec/bench-api.md).

A clean key-up:
  1. nothing in the slot before: the envelope at its floor until the slot starts, 1.25 ms before the first symbol
  2. one rise: no flat stretch on the way up (a knee or a step) and no fall back (a hook)
  3. placed late in the guard time: up shortly before the first symbol, not as early as the slot allows
  4. at the burst's level from the first symbol on: the first symbols neither over it (overshoot), under it (sag)
     nor still climbing to it (creep). A DMR carrier's envelope is flat (4FSK), so what moves there is the PA
  5. the same every burst (pa_cal.py: the spread of the -3 dB point over a capture's bursts)
A clean key-down: level to the last symbol's end, one fall, nothing after it.

    pa_shape.py RUN.json [...]      the faults and a binned profile of every level of saved pa_cal runs (--out)
"""
import json
import math
import statistics
import sys

SYM_MS = 1.0 / 4.8          # one symbol (4800 symbols/s)
GUARD_MS = 1.25             # the slot starts this long before the first symbol (ETSI TS 102 361-1, 4.2)
SYMS = 10                   # the first symbols judged (the gateway's trace ends 2.0 ms in: 9.6 of them)

PRE_SLOT_DB = -45.0         # the slot before: at most this, or 6 dB over the floor when that is higher
ON_DB = 10.0                # on the air: this far over the floor
RISE_LOW_DB = -40.0         # the rise is judged from here (or ON_DB over the floor) ...
RISE_HIGH_DB = -6.0         # ... to here: above it the approach (creep) is judged instead
KNEE_DB_PER_100US = 1.5     # a knee: the rise slower than this ...
KNEE_MS = 0.12              # ... for this long
HOOK_DB = 2.0               # a hook: the rise falls back this far before it goes on
EARLY_MS = -0.60            # the -3 dB point no earlier than this ...
LATE_MS = -0.10             # ... and no later
EARLY_ON_MS = -0.80         # and on the air (-30 dB) no earlier than this
APPROACH_MS = 0.40          # -6 dB to within 1 dB of the level: longer is a creep
SYM_RMS_DB = 0.5            # the first symbols: RMS from the burst's level
SYM_MAX_DB = 1.0            # and any point's distance from it
SYM_MEAN_DB = 0.5           # their mean over (overshoot) or under (sag) it
PEAK_DB = 1.5               # the highest point of the key-up
TREND_DB_PER_MS = 0.5       # still climbing (creep) or falling across the first symbols
DROOP_DB = 1.0              # key-down: the last symbols this far off the level
REBOUND_DB = 6.0            # key-down: back up this far over -30 dB once under it
JITTER_US = 50              # the -3 dB point's spread over a capture's bursts (pa_cal.py)

FAULTS = ("pre-slot", "hook", "knee", "early", "late", "creep", "overshoot", "sag", "unsettled", "peak",
          "droop", "rebound", "after-slot", "jitter")


def _pts(tr):
    return [(tr["t0_ms"] + i * tr["dt_ms"], v) for i, v in enumerate(tr["db"])]


def _cross(pts, level, start=None):
    """The first time at or after start the envelope is at or over level, ms, between points; None if never."""
    prev = None
    for t, v in pts:
        if start is not None and t < start:
            continue
        if v >= level:
            if prev is None or prev[1] >= level:
                return t
            return prev[0] + (t - prev[0]) * (level - prev[1]) / (v - prev[1])
        prev = (t, v)
    return None


def _rise_from(pts, level, until):
    """The last time before until (ms) the envelope rises through level: where the rise to until starts from there,
    not a noise spike earlier. None if it is over level all along or until is None."""
    if until is None:
        return None
    hit = None
    for (t1, v1), (t2, v2) in zip(pts, pts[1:]):
        if t2 > until:
            break
        if v1 < level <= v2:
            hit = t1 + (t2 - t1) * (level - v1) / (v2 - v1)
    return hit


def _pmean(vs):
    """Mean power of dB values, dB."""
    return 10.0 * math.log10(sum(10.0 ** (v / 10.0) for v in vs) / len(vs)) if vs else None


def _bins(pts, lo, hi, width):
    """Mean power per bin from lo to hi ms, dB, one decimal; None where a bin has no point."""
    out, a = [], lo
    while a < hi - 1e-9:
        vs = [v for t, v in pts if a <= t < a + width]
        m = _pmean(vs)
        out.append(None if m is None else round(m, 1))
        a += width
    return out


def _floor(trace, tail):
    head = [v for t, v in _pts(trace) if t < -3.0]
    back = [v for t, v in _pts(tail) if t > 2.0] if tail and tail.get("db") else []
    return min([statistics.median(x) for x in (head, back) if x] or [-60.0])


def key_up(trace, tail=None):
    """A key-up envelope's numbers (dict) and its faults (list of FAULTS words); None without an envelope."""
    if not trace or not trace.get("db"):
        return None
    pts, dt = _pts(trace), trace["dt_ms"]
    floor = _floor(trace, tail)
    r = {"floor_db": round(floor, 1)}
    r["pre_db"] = max([v for t, v in pts if t < -GUARD_MS] or [floor])
    r["on_ms"] = _cross(pts, floor + ON_DB)
    r["m3_ms"] = _cross(pts, -3.0)
    # The other points of the rise that ends at -3 dB; -30 dB is ON_DB over the floor where the floor is that high
    for name, lvl in (("m30_ms", max(-30.0, floor + ON_DB)), ("m10_ms", -10.0), ("m6_ms", -6.0)):
        r[name] = _rise_from(pts, lvl, r["m3_ms"])
    # The rise, from RISE_LOW_DB (or ON_DB over the floor) to RISE_HIGH_DB: its slowest stretch and how far it falls back
    lo = max(floor + ON_DB, RISE_LOW_DB)
    a = _cross(pts, lo)
    b = _cross(pts, RISE_HIGH_DB, a) if a is not None else None
    knee, hook, top = None, 0.0, None
    if a is not None and b is not None:
        seg = [(t, v) for t, v in pts if a - dt <= t <= b + dt]
        n = max(2, int(round(KNEE_MS / dt)))
        for i in range(len(seg) - n):
            (t1, v1), (t2, v2) = seg[i], seg[i + n]
            mid = [v for _, v in seg[i:i + n + 1]]
            if lo <= min(mid) and max(mid) <= RISE_HIGH_DB and (v2 - v1) / ((t2 - t1) * 10.0) < KNEE_DB_PER_100US:
                knee = (round(t1, 3), round(statistics.median(mid), 1))
                break
        for t, v in seg:
            top = v if top is None else max(top, v)
            if top - v > hook:
                hook = top - v
    r["knee"] = knee                        # (ms, dB) where the rise first stalls, or None
    r["hook_db"] = round(hook, 1)           # the most it fell back on the way up
    r["rise_ms"] = None if r["m30_ms"] is None or r["m3_ms"] is None else round(r["m3_ms"] - r["m30_ms"], 3)
    # The approach: -6 dB to the last time more than 1 dB under the level before the first symbols settle
    win = [(t, v) for t, v in pts if 0.0 <= t < SYMS * SYM_MS]
    out = [t for t, v in pts if t >= (r["m6_ms"] if r["m6_ms"] is not None else 0.0) and v < -1.0]
    r["approach_ms"] = None if r["m6_ms"] is None else round((out[-1] + dt if out else r["m6_ms"]) - r["m6_ms"], 3)
    # Up for good: within 1 dB of the level from here on (the envelope's end when it never is)
    r["up1_ms"] = None if r["m6_ms"] is None else round(out[-1] + dt if out else r["m6_ms"], 3)
    if win:
        vs = [v for _, v in win]
        tm = statistics.mean(t for t, _ in win)
        sxx = sum((t - tm) ** 2 for t, _ in win)
        r.update({"syms": round(len(win) * dt / SYM_MS, 1), "sym_mean_db": round(statistics.mean(vs), 2),
                  "sym_rms_db": round(math.sqrt(sum(v * v for v in vs) / len(vs)), 2),
                  "sym_max_db": max(abs(v) for v in vs),
                  "sym_trend_db_ms": round(sum((t - tm) * v for t, v in win) / sxx, 2) if sxx else 0.0})
        late = [t for t, v in pts if t >= 0.0 and abs(v) > SYM_MAX_DB]
        r["settle_ms"] = round(late[-1], 3) if late else 0.0
    r["peak_db"] = max([v for t, v in pts if t >= (r["on_ms"] if r["on_ms"] is not None else -GUARD_MS)] or [floor])
    r["guard_bins"] = _bins(pts, -GUARD_MS, 0.0, 0.125)       # ten bins of the guard time
    r["sym_bins"] = _bins(pts, 0.0, min(SYMS * SYM_MS, pts[-1][0] + dt), SYM_MS)
    return r, key_up_faults(r)


def key_up_faults(r):
    f = []
    if r["pre_db"] > max(PRE_SLOT_DB, r["floor_db"] + 6.0):
        f.append("pre-slot")
    if r["hook_db"] >= HOOK_DB:
        f.append("hook")
    if r["knee"]:
        f.append("knee")
    m3 = r["m3_ms"]
    if m3 is not None and (m3 < EARLY_MS or (r["m30_ms"] is not None and r["m30_ms"] < EARLY_ON_MS)):
        f.append("early")
    if m3 is None or m3 > LATE_MS:
        f.append("late")
    if "sym_rms_db" in r:
        if r["approach_ms"] is not None and r["approach_ms"] > APPROACH_MS and r["sym_trend_db_ms"] > TREND_DB_PER_MS:
            f.append("creep")
        elif r["sym_mean_db"] > SYM_MEAN_DB:
            f.append("overshoot")
        elif r["sym_mean_db"] < -SYM_MEAN_DB:
            f.append("sag")
        if (r["sym_rms_db"] > SYM_RMS_DB or r["sym_max_db"] > SYM_MAX_DB) and not set(f) & {"creep", "overshoot", "sag"}:
            f.append("unsettled")
    if r["peak_db"] > PEAK_DB and "overshoot" not in f:
        f.append("peak")
    return f


def key_down(tail, floor=None):
    """A key-down envelope's numbers and faults; None without an envelope."""
    if not tail or not tail.get("db"):
        return None
    pts, dt = _pts(tail), tail["dt_ms"]
    if floor is None:
        floor = statistics.median([v for t, v in pts if t > 2.0] or [-60.0])
    r = {"floor_db": round(floor, 1)}
    last = [v for t, v in pts if -SYMS * SYM_MS <= t < 0.0]
    r["end_mean_db"] = round(statistics.mean(last), 2) if last else None
    r["end_max_db"] = max(abs(v) for v in last) if last else None
    fall = lambda lvl: next((t for t, v in pts if t >= 0.0 and v < lvl), None)
    r["f3_ms"], r["f30_ms"] = fall(-3.0), fall(-30.0)
    back = [v for t, v in pts if r["f30_ms"] is not None and r["f30_ms"] < t <= GUARD_MS]
    r["rebound_db"] = round(max(back) + 30.0, 1) if back else 0.0
    r["after_db"] = max([v for t, v in pts if t > GUARD_MS] or [floor])
    f = []
    if r["end_max_db"] is not None and r["end_max_db"] > DROOP_DB:
        f.append("droop")
    if r["rebound_db"] >= REBOUND_DB and max(back) > floor + 6.0:
        f.append("rebound")
    if r["after_db"] > max(PRE_SLOT_DB, floor + 6.0):
        f.append("after-slot")
    return r, f


def line(r, f):
    """One key-up's numbers and faults on one line, for a log."""
    ms = lambda x: "-" if x is None else f"{x:+.2f}"
    s = (f"floor {r['floor_db']:.0f} pre {r['pre_db']:.0f} | -30 {ms(r['m30_ms'])} -3 {ms(r['m3_ms'])} ms "
         f"rise {'-' if r['rise_ms'] is None else round(r['rise_ms'] * 1000)} us hook {r['hook_db']:.1f}")
    if r["knee"]:
        s += f" knee {r['knee'][1]:.0f} dB @{r['knee'][0]:+.2f}"
    if "sym_rms_db" in r:
        s += (f" | {r['syms']:g} sym: mean {r['sym_mean_db']:+.2f} rms {r['sym_rms_db']:.2f} max {r['sym_max_db']:.1f} "
              f"trend {r['sym_trend_db_ms']:+.1f}/ms peak {r['peak_db']:+.1f} settled {r['settle_ms']:.2f}")
    return s + "  " + (" ".join(f) or "clean")


def bins_line(r):
    """The guard time and the first symbols as binned levels (dB): 125 us bins, then one per symbol."""
    b = lambda xs: " ".join("  ." if x is None else f"{x:3.0f}" for x in xs)
    return f"guard {b(r['guard_bins'])} | sym {b(r['sym_bins'])}"


def _pick(run):
    """Per band and level, the key the result stands on (pa_cal.py's tags, best first)."""
    rank = {"after": 4, "verify": 3, "board": 2, "confirm": 1, "before": 0}
    best = {}
    for k in run.get("keys", []):
        if not k.get("trace") or k.get("tag") not in rank:
            continue
        key = (round(k["mhz"], 4), k["level"])
        if key not in best or rank[k["tag"]] >= rank[best[key]["tag"]]:
            best[key] = k
    return best


def main(argv):
    for path in argv:
        run = json.load(open(path))
        who = run.get("radio") or {}
        print(f"== {path} {who.get('board', '')} {who.get('git', '')}")
        for (mhz, lvl), k in sorted(_pick(run).items()):
            up = key_up(k["trace"], k.get("tail"))
            down = key_down(k.get("tail"), up[0]["floor_db"] if up else None)
            faults = (up[1] if up else []) + (down[1] if down else [])
            print(f"  {mhz:g} L{lvl} {k['tag']:6s} {line(up[0], faults) if up else 'no envelope'}")
            if up:
                print(f"           {bins_line(up[0])}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
