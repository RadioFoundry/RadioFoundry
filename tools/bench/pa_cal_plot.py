#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
pa_cal_plot: a pa_cal.py run as pictures (docs/spec/pa-keyup.md 6). Needs matplotlib; pa_cal.py itself doesn't.

    pa_cal.py ... --plot DIR            draws at the end of the run
    pa_cal_plot.py RESULT.json DIR [NAME]   draws a run's --out file again; NAME: the radio in the titles, in
                                            place of the board the run recorded

Per band two files:

  pa_cal_<MHz>_ramps.png   the burst's envelope at the lowest, a middle and the top power level over each other, a
                           color each: the key-up against the slot's start and the 1.25 ms guard time (ETSI TS
                           102 361-1, shaded and named along the top), the key-up window, the
                           planned drive points and the first symbol; the key-down against the last symbol and the
                           slot's end. When the run changed the constants, the board's (before) are dashed; a run
                           that kept them has one measurement and draws one. Without envelopes from the gateway
                           (`trace`) only the measured -3 dB points are drawn.
  pa_cal_<MHz>_fit.png     the open-loop time of each drive with the fitted law, and every level's -3 dB point
                           before and after.

Times are from the first symbol's start (the key-up) or the last symbol's end (the key-down), ms.
"""
import json
import os
import sys

# A DMR burst is 27.5 ms in a 30 ms slot: 1.25 ms of guard on either side (ETSI TS 102 361-1, 4.2)
GUARD_MS = 1.25
BEFORE, AFTER, INK, FAINT = "#8a8f98", "#1f6fd1", "#30343b", "#c9ced6"
GUARD, OTHER, SLOT = "#f6e3a1", "#e4e7ec", "#b3261e"     # the guard time, the neighbor slot, the slot boundary
STRIP_DB = 8                                            # the timing strip sits above this, dB
LEVELS = ("#1f6fd1", "#d9622b", "#7a3fb8", "#12857a", "#5b6470")    # a color per shown power level


def law(c, d, top, ref):
    """The key-up law's time for a drive d written alone, us; None in the dead zone."""
    dead = ref * c["dead_pm"] // 1000
    return None if d <= dead else (c["knee_us"] * max(top - dead, 0) + c["slope_us"] * (d - dead)) / (d - dead)


def _curve(ax, t, color, label, before=False):
    if not t:
        return False
    x = [t["t0_ms"] + i * t["dt_ms"] for i in range(len(t["db"]))]
    ax.plot(x, t["db"], color=color, lw=1.1 if before else 1.7, ls="--" if before else "-", alpha=0.55 if before else 1,
            label=label)
    return True


def _marks(ax, start):
    """The slot's own timing (ETSI TS 102 361-1, 4.2: a 27.5 ms burst centered in a 30 ms slot), shaded and named
    in a strip along the top."""
    lo, hi = ax.get_xlim()
    # key-up: the slot before | guard | burst; key-down: burst | guard | the slot after
    edge, sym = (-GUARD_MS, 0.0) if start else (GUARD_MS, 0.0)
    other = (lo, edge) if start else (edge, hi)
    guard = (edge, sym) if start else (sym, edge)
    burst = (sym, hi) if start else (lo, sym)
    ax.axvspan(*other, color=OTHER, lw=0, label="the neighbor slot")
    ax.axvspan(*guard, color=GUARD, lw=0, label="ETSI guard time, 1.25 ms")
    ax.axvline(edge, color=SLOT, lw=1.6, label="slot boundary (ETSI)")
    ax.axvline(sym, color=INK, lw=1.0, label="first symbol" if start else "last symbol's end")
    ax.axhline(STRIP_DB, color=INK, lw=0.6)
    for (x0, x1), text, color in ((other, "the slot before" if start else "the slot after", BEFORE),
                                  (guard, "guard\n1.25 ms", "#8a6d00"),
                                  (burst, "our burst: 27.5 ms of symbols", INK)):
        ax.text((x0 + x1) / 2, STRIP_DB + 5.5, text, ha="center", va="center", fontsize=8, color=color,
                fontweight="bold" if "guard" in text else "normal", linespacing=0.95)
    ax.annotate("", xy=(guard[0], STRIP_DB + 1.2), xytext=(guard[1], STRIP_DB + 1.2),
                arrowprops={"arrowstyle": "<->", "color": "#8a6d00", "lw": 1.0, "shrinkA": 0, "shrinkB": 0})
    ax.text(edge, -44, " slot start" if start else "slot end ", rotation=90, va="bottom",
            ha="right" if start else "left", fontsize=8, color=SLOT, fontweight="bold")
    ax.axhline(-3, color=FAINT, lw=0.8)
    ax.axhline(0, color=FAINT, lw=0.8)


def _points(ax, k, color, y, alpha=1.0):
    """A key-up's planned points as ticks under the curves: the boost (^), the PA (s), the drive (v)."""
    if not k:
        return
    if k.get("boost"):
        ax.plot([-k["boost_us"] / 1000], [y], "^", color=color, ms=6, alpha=alpha)
        ax.plot([-k["drive_us"] / 1000], [y], "v", color=color, ms=6, alpha=alpha)
    ax.plot([-k["pa_us"] / 1000], [y], "s", color=color, ms=5, mfc="none", alpha=alpha)


def _power(lv, l):
    """A level's measured power, or its number when the run measured none ("-"; "0 mW" in older results: an
    over-the-air path, where --pad-db isn't the loss)"""
    p = (lv or {}).get("power") or "-"
    return f"level {l}" if p == "-" or p.startswith("0 ") else p


def _pair(mine, l):
    """A level's keys: (before, after). before is None when the run kept the board's constants: one measurement."""
    after = next((k for k in reversed(mine) if k.get("after") and k.get("level") == l), None)
    before = next((k for k in mine if k.get("tag") == "before" and k.get("level") == l), None)
    return (None if before is after else before), after


def ramps(plt, band, keys, path, who=""):
    """One picture: the shown levels over each other, a color each; the constants before as dashed lines, only when
    the run changed them."""
    from matplotlib.lines import Line2D
    shown = band.get("shown") or sorted(int(l) for l in band["levels"])[:3]
    mhz = band["mhz"]
    mine = [k for k in keys if k.get("mhz") == mhz]
    pairs = [(l, band["levels"].get(l) or band["levels"].get(str(l))) + _pair(mine, l) for l in shown]
    compared = any(b for _, _, b, _ in pairs)
    fig, (up, down) = plt.subplots(1, 2, figsize=(12, 5.2), gridspec_kw={"width_ratios": [3, 2]})
    up.set_xlim(-4.0, 2.0)
    down.set_xlim(-2.0, 4.0)
    for ax, name in ((up, "trace"), (down, "tail")):
        ax.set_ylim(-45, STRIP_DB + 9)
        _marks(ax, ax is up)
        drawn = False
        for i, (l, lv, before, after) in enumerate(pairs):
            color = LEVELS[i % len(LEVELS)]
            drawn |= _curve(ax, before and before.get(name), color, None, before=True)
            drawn |= _curve(ax, after and after.get(name), color, None)
        if not drawn:
            ax.text(0.5, 0.5, "no envelope from the gateway", transform=ax.transAxes, ha="center", color=BEFORE)
        ax.grid(True, color="#e6e8ec", lw=0.5)
    up.axvline(-band["window_us"] / 1000, color=INK, lw=0.8, ls="--", label="key-up window")
    up.axvline(band["target_us"] / 1000, color="#2a9d5c", lw=0.8, ls="--", label="-3 dB target")
    names, note = [], []
    for i, (l, lv, before, after) in enumerate(pairs):
        color = LEVELS[i % len(LEVELS)]
        for k, old in ((before, True), (after, False)):
            if k and k.get("rise_us") is not None:
                # A creeping level is timed by its edge, under -3 dB: the dot goes on the curve there
                t, y = k.get("trace"), -3.0
                if k.get("creep") and t:
                    at = int(round((k["rise_us"] / 1000 - t["t0_ms"]) / t["dt_ms"]))
                    y = t["db"][at] if 0 <= at < len(t["db"]) else y
                up.plot([k["rise_us"] / 1000], [y], "o", color=color, ms=5, mfc="none" if old else color,
                        alpha=0.55 if old else 1)
        y = -42 + 3 * i
        _points(up, before and before.get("plan"), color, y, 0.45)
        _points(up, lv and lv.get("plan"), color, y)
        # The level by its place and drive: the measured power is only as good as the run's --pad-db
        power = "lowest level" if i == 0 else "top level" if i == len(pairs) - 1 else f"level {l}"
        meas = f", {_power(lv, l)}" if _power(lv, l) != f"level {l}" else ""
        names.append(Line2D([], [], color=color, lw=1.7, label=f"{power} (drive {lv['drive'] if lv else '?'}{meas})"))
        words = [f"{w}{'edge ' if k.get('creep') else ''}{k['rise_us']:+.0f} us, {k.get('over_db') or 0:.1f} dB over"
                 for k, w in ((before, "before "), (after, "after " if compared else ""))
                 if k and k.get("rise_us") is not None]
        note.append((f"{power}: " + "; ".join(words), color))
    for i, (text, color) in enumerate(note):
        up.text(0.01, 0.80 - 0.05 * i, text, transform=up.transAxes, va="top", fontsize=8, color=color)
    up.set_ylabel("dB from the burst's level")
    up.set_xlabel("ms from the first symbol")
    down.set_xlabel("ms from the last symbol's end")
    up.set_title("key-up (-3 dB point, overshoot)", fontsize=10, loc="left")
    down.set_title("key-down", fontsize=10, loc="left")
    if compared:
        names += [Line2D([], [], color=INK, lw=1.1, ls="--", alpha=0.55, label="before"),
                  Line2D([], [], color=INK, lw=1.7, label="after (this run's)")]
    up.legend(handles=names, loc="lower right", fontsize=8, frameon=True, framealpha=0.9, edgecolor=FAINT)
    h, lab = up.get_legend_handles_labels()
    h += [Line2D([], [], marker="^", ls="", color=INK), Line2D([], [], marker="s", ls="", color=INK, mfc="none"),
          Line2D([], [], marker="v", ls="", color=INK), Line2D([], [], marker="o", ls="", color=INK)]
    lab += ["first drive written (boost or eased)", "PA on", "drive to the level's", "measured -3 dB point (or edge)"]
    fig.legend(h, lab, loc="lower center", ncol=5, fontsize=8, frameon=False)
    c = band["constants"]
    state = "kept: the board's constants hold" if band.get("kept") else "PASS" if band["ok"] else "CHECK"
    # Two lines: one ran off a long radio name's figure
    fig.suptitle(f"{who}{mhz:g} MHz key-up  ({state})\ndead {c['dead_pm']}/1000, knee {c['knee_us']} us, "
                 f"slope {c['slope_us']} us, window {c['window_us']} us, settle {c['settle_us']} us"
                 + (f", ease {c['ease_pm']}/1000" if c.get("ease_pm") else ""), fontsize=10.5)
    fig.tight_layout(rect=(0, 0.09, 1, 0.96))
    fig.savefig(path, dpi=130)
    plt.close(fig)


def fit(plt, band, keys, path, who=""):
    mhz, c, top, ref = band["mhz"], band["constants"], band["top"], band["ref"]
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 4))
    pts = band.get("open") or []
    if pts:
        lo, hi = min(d for d, _ in pts), max(max(d for d, _ in pts), top)
        xs = [lo + (hi - lo) * i / 200 for i in range(201)]
        ys = [law(c, x, top, ref) for x in xs]
        a1.plot([x for x, y in zip(xs, ys) if y], [y for y in ys if y], color=AFTER, lw=1.5, label="the fitted law")
        a1.plot([d for d, _ in pts], [t for _, t in pts], "o", color=INK, ms=5, label="open loop: the drive alone")
        a1.set_ylim(0, max(max(t for _, t in pts) * 1.15, band["window_us"] * 1.3))
    a1.axhline(band["window_us"], color=AFTER, lw=0.8, ls="--", label="key-up window: slower levels are boosted")
    dead = ref * c["dead_pm"] // 1000
    if dead:
        a1.axvline(dead, color=BEFORE, lw=0.8, ls=":", label="dead zone's end")
    for l, lv in band["levels"].items():
        a1.axvline(lv["drive"], color=FAINT, lw=0.6)
        a1.text(lv["drive"], a1.get_ylim()[1] * 0.97, f" {_power(lv, l)}", rotation=90, va="top", fontsize=7, color=BEFORE)
    a1.set_xlabel("drive (the radio's DAC counts)")
    a1.set_ylabel("us from the drive's write to the level")
    a1.set_title("time to come up against drive", fontsize=10, loc="left")
    a1.legend(fontsize=8, frameon=False)
    a1.grid(True, color="#e6e8ec", lw=0.5)

    mine = [k for k in keys if k.get("mhz") == mhz]
    names = sorted(band["levels"], key=int)
    for i, l in enumerate(names):
        lv = band["levels"][l]
        b = next((k for k in mine if k.get("tag") in ("before", "check") and k.get("level") == int(l)), None)
        if b and b.get("rise_us") is not None and not b.get("after"):     # a kept run has one measurement, not two
            a2.plot([i - 0.12], [b["rise_us"]], "o", color=BEFORE, ms=7, label="before" if i == 0 else None)
        a2.plot([i + 0.12], [lv["rise_us"]], "o", color=AFTER, ms=7, label="this run" if i == 0 else None)
    a2.axhspan(-600, 0, color="#e3f3e8", lw=0, label="in time: up before the first symbol")
    a2.axhline(0, color=INK, lw=1.0)
    a2.axhline(band["target_us"], color="#2a9d5c", lw=0.8, ls="--", label="target")
    a2.set_xticks(range(len(names)))
    a2.set_xticklabels([_power(band["levels"][l], l) for l in names], fontsize=8)
    a2.set_ylabel("-3 dB point, us (negative: before the first symbol)")
    a2.set_title("each level's key-up", fontsize=10, loc="left")
    a2.legend(fontsize=8, frameon=False)
    a2.grid(True, color="#e6e8ec", lw=0.5)
    fig.suptitle(f"{who}{mhz:g} MHz", fontsize=11)
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    fig.savefig(path, dpi=130)
    plt.close(fig)


def draw(result, out_dir, name=""):
    """Every band of a run into out_dir; returns the files. ImportError without matplotlib. name: the radio in the
    titles, in place of the board the run recorded."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    os.makedirs(out_dir, exist_ok=True)
    files = []
    radio = result.get("radio") or {}
    who = name or radio.get("board", "")
    who = (f"{who} ({radio['git']}), " if radio.get("git") else f"{who}, ") if who else ""
    for band in result.get("bands", []):
        band = dict(band, levels={int(l): v for l, v in band["levels"].items()})
        band.setdefault("window_us", band["constants"]["window_us"])
        band.setdefault("target_us", -250)
        stem = os.path.join(out_dir, f"pa_cal_{band['mhz']:g}")
        ramps(plt, band, result.get("keys", []), stem + "_ramps.png", who)
        fit(plt, band, result.get("keys", []), stem + "_fit.png", who)
        files += [stem + "_ramps.png", stem + "_fit.png"]
    return files


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) not in (2, 3):
        print(__doc__)
        return 2
    with open(argv[0]) as f:
        result = json.load(f)
    try:
        for name in draw(result, argv[1], argv[2] if len(argv) == 3 else ""):
            print(name)
    except ImportError as e:
        print(f"{e}: pip install matplotlib")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
