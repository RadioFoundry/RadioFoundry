# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The speaker checks' analysis (audio_check.py) on synthetic recordings, no radio: tools/ci.sh runs it when numpy
is installed. Each fault the speaker tests look for is made on purpose and must be found; the clean signal, with
a room's echo and the fades the beep player gives, must pass."""
from rfoundry_hil import skip

try:
    import numpy as np
except ImportError:
    skip("bench", "numpy is not installed", module=True)

import audio_check as A  # noqa: E402

FS = 16000


def render(segs, lead_s=0.3, tail_s=0.4, level=0.03, noise_db=-85, ramp_ms=2.0, echo=0.0, seed=1):
    """segs as a beep plays them (hz 0 a silence), each tone faded in and out over ramp_ms, over a noise floor;
    echo adds a copy 6 ms later at that amplitude, as a room does. Returns the signal and the first tone's start."""
    rng = np.random.default_rng(seed)
    parts = [np.zeros(int(lead_s * FS))]
    for hz, ms in segs:
        n = int(ms * FS / 1000)
        if hz == 0:
            parts.append(np.zeros(n))
            continue
        t = np.arange(n) / FS
        edge = np.minimum(np.arange(n), np.arange(n)[::-1]) / (ramp_ms * FS / 1000)
        parts.append(level * np.minimum(edge, 1.0) * np.sin(2 * np.pi * hz * t))
    parts.append(np.zeros(int(tail_s * FS)))
    x = np.concatenate(parts)
    if echo:
        d = int(0.006 * FS)
        x[d:] += echo * x[:-d].copy()
    return x + rng.normal(0, 10 ** (noise_db / 20), len(x)), lead_s


def check(x, segs, **kw):
    r = A.check_tones(x, FS, segs, **kw)
    print(r.report())
    return r


def test_patterns_from_source():
    p = A.beep_patterns()
    assert {"key", "error", "tx_permit", "tx_deny", "call_end"} <= set(p)
    assert all(ms > 0 for segs in p.values() for _, ms in segs)
    assert p["tx_permit"][1][0] == 0       # a silence between the two parts


def test_every_beep_clean():
    for name, segs in A.beep_patterns().items():
        if any(hz == A.SWEEP_HZ for hz, _ in segs):
            continue        # the speaker test's sweep: test_sweep_*
        x, t0 = render(segs, echo=0.3)
        r = check(x, segs)
        assert not r.problems, name
        assert abs(r.tones[0].start_s - t0) < 0.004, name
        for t, (_, ms) in zip(r.tones, [s for s in segs if s[0]]):
            assert abs(t.got_ms - ms) < 6, (name, t)


def test_start_cut():
    segs = [(400, 150)]
    x, t0 = render(segs)
    x[int(t0 * FS):int((t0 + 0.04) * FS)] = 0      # an amplifier that comes up 40 ms late
    r = check(x, segs)
    assert any("cut by" in p for p in r.problems), r.problems


def test_end_cut():
    segs = [(1000, 100), (0, 100), (1000, 100)]
    x, t0 = render(segs)
    a = int((t0 + 0.27) * FS)
    x[a:a + int(0.03 * FS)] = 0                    # the stream stopped 30 ms early
    r = check(x, segs)
    assert any("cut by" in p for p in r.problems), r.problems


def test_level_step():
    segs = [(800, 200)]
    x, t0 = render(segs)
    a = int(t0 * FS)
    x[a:a + int(0.06 * FS)] *= 0.25                 # the first 60 ms 12 dB low, as an amplifier settles
    r = check(x, segs)
    # the step is seen as the start (a cut) or on the plateau, as it is long or short
    assert any("step" in p or "cut by" in p for p in r.problems), r.problems


def test_dropout():
    segs = [(600, 300)]
    x, t0 = render(segs)
    a = int((t0 + 0.15) * FS)
    x[a:a + int(0.004 * FS)] = 0                    # 4 ms of the stream lost
    r = check(x, segs)
    assert any("dropout" in p for p in r.problems), r.problems


def test_room_swell_passes():
    segs = [(600, 150)]
    x, t0 = render(segs)
    a, n = int(t0 * FS), int(0.15 * FS)
    x[a:a + n] *= 1 + 0.9 * np.sin(np.pi * np.arange(n) / n)    # +5.6 dB in the middle, slowly
    assert not check(x, segs).problems


def test_click_in_silence():
    segs = [(1200, 50), (0, 30), (900, 70)]
    x, t0 = render(segs)
    x[int((t0 + 0.2) * FS)] += 0.3                  # a pop after the beep (an amplifier muting)
    r = check(x, segs)
    assert r.pops and any("pop" in p for p in r.problems), r.problems


def test_click_before():
    segs = [(1000, 25)]
    x, t0 = render(segs)
    x[int((t0 - 0.08) * FS)] -= 0.2                 # a pop as the amplifier comes on, before the tone
    r = check(x, segs)
    assert any("pop" in p for p in r.problems), r.problems


def test_noise_burst_after():
    segs = [(1000, 300)]
    x, t0 = render(segs, tail_s=0.6)
    a = int((t0 + 0.4) * FS)
    rng = np.random.default_rng(2)
    x[a:a + int(0.17 * FS)] += rng.normal(0, 0.03, int(0.17 * FS))  # an FM squelch tail
    r = check(x, segs, guard_ms=(150, 500))
    assert any("noise burst" in p for p in r.problems), r.problems


def test_gap_filled():
    segs = [(1600, 50), (0, 40), (1600, 50)]
    x, t0 = render(segs)
    a = int((t0 + 0.05) * FS)
    t = np.arange(int(0.04 * FS)) / FS
    x[a:a + len(t)] += 0.02 * np.sin(2 * np.pi * 1600 * t)  # the silence isn't one
    r = check(x, segs)
    assert r.problems, r.report()


def test_too_quiet():
    segs = [(1000, 100)]
    x, _ = render(segs, level=10 ** (-105 / 20))
    r = check(x, segs)
    assert r.quiet and r.problems


def test_program_segs():
    prog = [{"kind": "silence", "s": 0.5}, {"kind": "tone", "hz": 1000, "s": 0.3, "level": 0.6}]
    assert A.program_segs(prog) == [(0, 500.0), (1000, 300.0)]


def test_noisy_lead():
    """An FM receiver's hiss as the squelch opens, before and between the tones: the tones are still found where
    they are, not at the first noise"""
    segs = [(1000, 300), (0, 200), (600, 150), (0, 200), (1500, 80)]
    x, t0 = render(segs, lead_s=0.8, level=0.03)
    rng = np.random.default_rng(3)
    a = int(0.2 * FS)
    x[a:] += rng.normal(0, 10 ** (-58 / 20), len(x) - a)       # the carrier's noise from 0.2 s on
    r = check(x, segs, guard_ms=(550, 300))
    assert abs(r.tones[0].start_s - t0) < 0.004, r.report()
    assert not [p for p in r.problems if "cut" in p or "long" in p], r.problems


def render_sweep(lo, hi, ms, swap_from_s=None, level=0.05, noise_db=-85, seed=2):
    """The sweep as the radio makes it, 8 kHz, then at FS for the microphone: silence, the sweep, silence. swap_from_s:
    from that time into the sweep on, each pair of 32-sample chunks plays in the wrong order, as the prompt FIFO did
    when a chunk went into the half being played (docs/spec/modem-hrc7000.md 8.4)"""
    rate = 8000
    n = int(ms * rate / 1000)
    t = np.arange(n) / rate
    s = level * np.sin(2 * np.pi * (lo * t + 0.5 * (hi - lo) / (ms / 1000) * t ** 2))
    if swap_from_s is not None:
        a = int(swap_from_s * rate) // 64 * 64
        for k in range(a, n - 64, 64):
            s[k:k + 64] = np.concatenate([s[k + 32:k + 64], s[k:k + 32]])
    x8 = np.concatenate([np.zeros(int(0.4 * rate)), s, np.zeros(int(0.6 * rate))])
    x = np.interp(np.arange(len(x8) * FS // rate) * rate / FS, np.arange(len(x8)), x8)
    rng = np.random.default_rng(seed)
    return x + rng.normal(0, 10 ** (noise_db / 20), len(x))


def test_sweep_clean():
    lo, hi, ms = A.sweep_spec()
    r = A.check_sweep(render_sweep(lo, hi, ms), FS, lo, hi, ms)
    assert not r.problems and r.broken == 0 and abs(r.start_s - 0.4) < 0.01, r.report()
    # Flat but for the linear interpolation to FS here, about 4 dB down at 3.2 kHz
    assert max(d for _, d in r.response) == 0 and min(d for _, d in r.response) > -6, r.report()


def test_sweep_breaks_up():
    """Chunks out of order for the second half of the sweep: half the frames broken, and a problem"""
    lo, hi, ms = A.sweep_spec()
    r = A.check_sweep(render_sweep(lo, hi, ms, swap_from_s=ms / 2000), FS, lo, hi, ms)
    assert r.problems and 0.35 < r.broken / r.frames < 0.65, r.report()


def test_sweep_too_quiet():
    lo, hi, ms = A.sweep_spec()
    r = A.check_sweep(render_sweep(lo, hi, ms, level=10 ** (-110 / 20)), FS, lo, hi, ms)
    assert r.quiet and r.problems, r.report()


def test_sweep_bad_capture():
    """A recording with most of its energy above the radio's 8 kHz band: the capture's fault, said so"""
    lo, hi, ms = A.sweep_spec()
    x = render_sweep(lo, hi, ms)
    t = np.arange(len(x)) / FS
    x += 0.08 * np.sin(2 * np.pi * (6000 - 300 * t) * t) * ((t > 0.4) & (t < 0.4 + ms / 1000))
    r = A.check_sweep(x, FS, lo, hi, ms)
    assert any("capture" in p for p in r.problems), r.report()
