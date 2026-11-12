# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The PA ramp's shape (tools/bench/pa_shape.py) on made-up envelopes, no radio and no bench: tools/ci.sh runs it. A
clean key-up has no fault; each fault the plots show (a carrier in the slot before, a hook, a knee, overshoot, sag,
creep, an early or late edge, a key-down that comes back) is named on an envelope that has only it."""
import math

import pa_shape

DT = 1 / 48.0       # ms, the gateway's 48 kS/s envelope


def env(f, t0=-4.0, n=289):
    """An envelope as the gateway sends it: dB from the burst's level, no lower than -60, one decimal."""
    return {"t0_ms": t0, "dt_ms": DT, "n": 9, "db": [round(max(-60.0, f(t0 + i * DT)), 1) for i in range(n)]}


def edge(t, at=-0.25, width=0.15):
    """A smooth rise through -3 dB at `at` ms (a raised cosine in amplitude over 2 * width)."""
    x = (t - at) / width
    if x <= -1.0:
        return -60.0
    a = 1.0 if x >= 1.0 else 0.5 * (1.0 - math.cos(math.pi * (x + 1.0) / 2.0)) ** 0.5 * math.sqrt(2.0)
    return max(-60.0, 20.0 * math.log10(max(min(a, 1.0), 1e-3)))


def fall(t, at=0.4):
    return 0.0 if t < at else max(-60.0, -200.0 * (t - at))


def faults(up, down=lambda t: fall(t)):
    r = pa_shape.key_up(env(up), env(down, t0=-2.0))
    d = pa_shape.key_down(env(down, t0=-2.0), r[0]["floor_db"])
    return r[0], r[1] + d[1]


def test_clean():
    r, f = faults(edge)
    assert f == [], (f, pa_shape.line(r, f))
    assert -0.35 < r["m3_ms"] < -0.15 and r["sym_rms_db"] < 0.1 and r["hook_db"] < 0.5


def test_pre_slot():
    assert "pre-slot" in faults(lambda t: max(edge(t), -41.0 if t > -1.75 else -60.0))[1]


def test_hook():
    # On at -0.85 ms to -31 dB, back to -35, then the real rise: the plot's fish hook
    hook = lambda t: edge(t) if t > -0.45 else (-31.0 if -0.85 < t < -0.75 else -35.0 if -0.75 <= t else -60.0)
    assert "hook" in faults(hook)[1]


def test_knee():
    # A rise that stalls at -25 dB for 0.2 ms
    knee = lambda t: -60.0 if t < -0.9 else -25.0 if t < -0.6 else edge(t)
    assert "knee" in faults(lambda t: max(knee(t), edge(t, -0.25)))[1]


def test_overshoot_sag_creep():
    assert "overshoot" in faults(lambda t: edge(t) + (2.0 * math.exp(-t / 1.0) if t > -0.1 else 0.0))[1]
    assert "sag" in faults(lambda t: edge(t) - (1.5 if t > -0.2 else 0.0))[1]
    assert "creep" in faults(lambda t: min(edge(t, -0.4), -4.0 + 1.5 * max(0.0, t + 0.4)))[1]


def test_early_late():
    assert "early" in faults(lambda t: edge(t, -0.7))[1]
    assert "late" in faults(lambda t: edge(t, 0.1))[1]


def test_key_down_rebound_and_droop():
    assert "rebound" in faults(edge, lambda t: -15.0 if 0.65 < t < 0.75 else fall(t))[1]
    assert "droop" in faults(edge, lambda t: (-2.0 if -0.8 < t < 0.4 else 0.0) if t < 0.4 else fall(t))[1]


def test_bins():
    r, _ = faults(edge)
    assert len(r["guard_bins"]) == 10 and r["guard_bins"][0] == -60.0 and abs(r["sym_bins"][0]) < 0.5
    assert "guard" in pa_shape.bins_line(r)
