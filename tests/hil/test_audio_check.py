# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The speaker checks' analysis (audio_check.py) on synthetic recordings, no radio: tools/ci.sh runs it when numpy
is installed. Each fault the speaker tests look for is made on purpose and must be found; the clean signal, with
a room's echo, the fades the beep player gives and a microphone's noise suppression, must pass."""
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


def _mic_fade(x, a, n, after_ms=65.0, over_ms=35.0, depth_db=25.0):
    """A headset's noise suppression on a steady tone from sample a, n long: full for after_ms, then down depth_db
    over over_ms, smoothly, until the tone stops"""
    g = np.ones(n)
    k0, k1 = int(after_ms * FS / 1000), int((after_ms + over_ms) * FS / 1000)
    g[k0:k1] = 10 ** (-depth_db / 20 * (0.5 - 0.5 * np.cos(np.pi * np.arange(k1 - k0) / (k1 - k0))))
    g[k1:] = 10 ** (-depth_db / 20)
    x[a:a + n] *= g


def test_microphone_fade_passes():
    segs = [(600, 150), (0, 80), (600, 150), (0, 80), (600, 150)]
    x, t0 = render(segs, echo=0.3)
    for k in range(3):
        _mic_fade(x, int((t0 + k * 0.23) * FS), int(0.15 * FS))
    r = check(x, segs)
    assert not r.problems, r.problems
    assert all(abs(t.got_ms - 150) < 6 and t.fade_db > 15 for t in r.tones), r.report()


def test_microphone_fade_then_cut():
    segs = [(600, 200)]
    x, t0 = render(segs)
    a = int(t0 * FS)
    _mic_fade(x, a, int(0.2 * FS))
    x[a + int(0.16 * FS):a + int(0.2 * FS)] = 0       # faded, and the stream stopped 40 ms early
    r = check(x, segs)
    # 40 ms of silence at either end fit the sequence equally well: the fault shows at the start or the end
    assert r.problems, r.report()


def test_microphone_fade_then_dropout():
    segs = [(600, 300)]
    x, t0 = render(segs)
    a = int(t0 * FS)
    _mic_fade(x, a, int(0.3 * FS))
    x[a + int(0.2 * FS):a + int(0.204 * FS)] = 0      # 4 ms lost after the fade
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


def _tail_with_echo(burst_s, level):
    """A 1 kHz tone, then a noise burst burst_s long at level, each followed by a room's echo: it falls 12 dB at once,
    then fades 0.15 dB per ms (as a microphone at a radio's speaker heard it). Returns the signal, the burst's start"""
    segs = [(1000, 300)]
    x, t0 = render(segs, tail_s=0.9)
    rng = np.random.default_rng(3)
    a, n = int((t0 + 0.55) * FS), int(burst_s * FS)
    burst = rng.normal(0, level, n)
    k = np.arange(int(0.3 * FS))
    echo = rng.normal(0, level * 10 ** (-12 / 20), len(k)) * 10 ** (-0.15 * (k / FS * 1000) / 20)
    x[a:a + n] += burst
    x[a + n:a + n + len(k)] += echo
    return x, segs, t0 + 0.55


def test_burst_length_leaves_out_the_echo():
    """The same 50 ms burst at two levels: the pop runs on with the echo (longer when louder), the burst's own
    length stays about 50 ms"""
    out = []
    for level in (0.01, 0.1):
        x, segs, start = _tail_with_echo(0.05, level)
        r = check(x, segs, guard_ms=(150, 900))
        t0, _, ms = [p for p in r.pops if p[0] > start - 0.02][0]
        out.append((ms, A.burst_ms(x, FS, [1000], t0, ms)))
    print(out)
    assert out[1][0] > out[0][0] + 40 and out[1][0] > 150            # the echo counted: louder is longer
    assert all(40 <= own <= 65 for _, own in out), out                # the burst itself
    x, segs, start = _tail_with_echo(0.17, 0.05)                       # a long tail stays long
    r = check(x, segs, guard_ms=(150, 900))
    t0, _, ms = [p for p in r.pops if p[0] > start - 0.02][0]
    assert 160 <= A.burst_ms(x, FS, [1000], t0, ms) <= 185


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
