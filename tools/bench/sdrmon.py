#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
sdrmon: the DMR carrier floor and off-slot residual of a 1 ms power envelope (dB). tests/hil/test_sdrmon_floor.py
checks both on synthetic envelopes.
"""
import numpy as np

FS_CH = 48000.0
SPS = 10
BURST = 132 * SPS                  # 27.5 ms


CARRIER_BELOW_DB = 30.0     # dB under the bursts: no carrier, however far over the floor (an off slot's residual)
QUIET_DB = -110.0           # dB: a 1 ms window of exact zeros (log of the envelope's 1e-12 guard is -120)


def carrier_floor(env1: np.ndarray, threshold_db: float) -> tuple:
    """(noise, base) in dB from the 1 ms power envelope env1. noise: the floor, from the windows that hold anything (a
    channel output that rounds a quiet channel to exact zeros would put it at the bottom of the scale; with nothing
    else quiet, the off slot's residual is then the floor). base: what the carrier thresholds (base + threshold_db)
    and the ramps' ends stand on: the floor, raised to CARRIER_BELOW_DB + threshold_db under the bursts' level when the
    receiver is so close that a radio's off-slot residual stands over the floor. That residual is judged by
    off_slot_residual, not counted as carrier."""
    live = env1[env1 > QUIET_DB]
    if len(live) == 0:
        return float(np.percentile(env1, 10)), float(np.percentile(env1, 10))
    noise = float(np.percentile(live, 10))
    level = float(np.percentile(live, 90))
    return noise, max(noise, level - CARRIER_BELOW_DB - threshold_db)


def off_slot_residual(env1: np.ndarray, starts, quiet: float, burst: int = BURST, fs: float = FS_CH):
    """The carrier between a radio's bursts in one slot: (median of the off part minus the median burst level in dB,
    whether the off part stands over the quiet level) over burst pairs 55-65 ms apart, None without one. The off part
    is the middle of the gap (5 ms clear of each burst). quiet: the capture's level with nothing keyed (the 10th
    percentile of the whole envelope, exact zeros included); at it (under quiet + 3 dB) the residual can't be measured,
    and the value is only a bound."""
    ms5, pairs = int(fs * 0.005), []
    starts = sorted(int(s) for s in starts)
    for a, b in zip(starts, starts[1:]):
        if not 55e-3 * fs <= b - a <= 65e-3 * fs or b - ms5 > len(env1):
            continue
        on = env1[a + ms5:a + burst - ms5]
        off = env1[a + burst + ms5:b - ms5]
        if len(on) and len(off):
            pairs.append((float(np.median(off)), float(np.median(on))))
    if not pairs:
        return None
    off = float(np.median([o for o, _ in pairs]))
    on = float(np.median([x for _, x in pairs]))
    return round(off - on, 1), off > quiet + 3
