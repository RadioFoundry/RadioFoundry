# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""sdrmon.py's carrier floor and off-slot residual on synthetic 1 ms envelopes. No radio: these run anywhere, and
tools/ci.sh runs them. A receiver beside the radio heard its off-slot residual (about 41 dB under the bursts) far over
a floor the channel output's exact zeros had pulled down, so the threshold counted the residual as carrier: phantom
key-ups every few ms, 35 ms bursts and 5-40 ms ramps on a radio whose bursts were clean."""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "bench"))
import sdrmon  # noqa: E402

FS = sdrmon.FS_CH
THR = 10.0


def _call(burst_db, off_db, floor_db, quiet_s=1.0, call_s=4.0, zeros=False, seed=1):
    """A 1 ms envelope (dB, one value per sample at 48 kS/s): quiet, a call of 30 ms bursts every 60 ms with off_db
    between them, quiet again. zeros: the quiet stretches are exact zeros, as a channel output that rounds a quiet
    channel to zero gives them. Returns (env1, burst starts)."""
    rng = np.random.default_rng(seed)
    q, c = int(quiet_s * FS), int(call_s * FS)
    quiet = np.full(q, -120.0) if zeros else floor_db + rng.normal(0, 0.5, q)
    call = np.full(c, max(off_db, floor_db)) + rng.normal(0, 0.5, c)
    period, on = int(0.060 * FS), int(0.030 * FS)
    starts = []
    for a in range(0, c - period, period):
        call[a:a + on] = burst_db + rng.normal(0, 0.2, on)
        starts.append(q + a)
    return np.concatenate([quiet, call, quiet]), starts


def test_close_receiver_raises_the_base_over_the_residual():
    env, _ = _call(burst_db=-8, off_db=-49, floor_db=-90, zeros=True)
    noise, base = sdrmon.carrier_floor(env, THR)
    assert noise > -110, "the exact zeros set the floor"
    assert base + THR > -49 + 3, "the threshold sits under the residual: it would count as carrier"
    assert base + THR < -8 - 20, "the threshold must stay well under the bursts"


def test_far_receiver_keeps_the_noise_floor():
    env, _ = _call(burst_db=-40, off_db=-90, floor_db=-65)
    noise, base = sdrmon.carrier_floor(env, THR)
    assert abs(noise - -65) < 2
    assert base == noise


def test_residual_measured_beside_the_radio():
    env, starts = _call(burst_db=-8, off_db=-49, floor_db=-90, zeros=True)
    res, measured = sdrmon.off_slot_residual(env, starts, float(np.percentile(env, 10)))
    assert measured and abs(res - -41) < 1


def test_residual_at_the_floor_is_only_a_bound():
    env, starts = _call(burst_db=-40, off_db=-90, floor_db=-65)
    res, measured = sdrmon.off_slot_residual(env, starts, float(np.percentile(env, 10)))
    assert not measured and abs(res - -25) < 1


def test_a_real_leak_fails_the_residual():
    # 13 dB under the bursts (a transmitter supply that leaks the carrier): the residual check fails it, with the
    # quiet stretches as exact zeros (beside the radio) and as plain noise (further away)
    for zeros in (True, False):
        env, starts = _call(burst_db=-8, off_db=-21, floor_db=-90, zeros=zeros)
        res, measured = sdrmon.off_slot_residual(env, starts, float(np.percentile(env, 10)))
        assert measured and res > -sdrmon.CARRIER_BELOW_DB, (zeros, res, measured)


def test_no_bursts_no_residual():
    env, _ = _call(burst_db=-60, off_db=-60, floor_db=-60, call_s=0.05)
    assert sdrmon.off_slot_residual(env, [], -60.0) is None
