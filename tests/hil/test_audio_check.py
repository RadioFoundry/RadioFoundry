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


def _mic_fade(x, a, n, after_ms=65.0, over_ms=35.0, depth_db=25.0):
    """A headset's noise suppression on a steady tone from sample a, n long: full for after_ms, then down depth_db
    over over_ms, smoothly, until the tone stops"""
    g = np.ones(n)
    k0, k1 = int(after_ms * FS / 1000), int((after_ms + over_ms) * FS / 1000)
    g[k0:k1] = 10 ** (-depth_db / 20 * (0.5 - 0.5 * np.cos(np.pi * np.arange(k1 - k0) / (k1 - k0))))
    g[k1:] = 10 ** (-depth_db / 20)
    x[a:a + n] *= g


def _headset(x, a, n, cut_ms=25.0, depth_db=25.0, back_ms=60.0, back=1.5):
    """A headset that cuts a steady tone (from sample a, n long) after cut_ms and plays it back in the silence after
    it for back_ms, louder than the tone was (an adaptive canceller goes on subtracting a tone that has stopped)"""
    k = int(cut_ms * FS / 1000)
    y = x[a:a + n].copy()
    x[a + k:a + n] *= 10 ** (-depth_db / 20)
    m = int(back_ms * FS / 1000)
    x[a + n:a + n + m] += back * y[:m]


def onsets(x, segs, **kw):
    r = A.check_onsets(x, FS, segs, **kw)
    print(r.report())
    return r


def test_onsets_through_a_headset():
    segs = [(600, 150), (0, 80), (600, 150), (0, 80), (600, 150)]
    x, t0 = render(segs, echo=0.3)
    for k in range(3):
        _headset(x, int((t0 + k * 0.23) * FS), int(0.15 * FS), cut_ms=110)
    r = onsets(x, segs)
    assert not r.problems, r.problems
    assert all(abs(t.start_s - (t0 + k * 0.23)) < 0.012 for k, t in enumerate(r.tones)), r.report()
    assert check(x, segs).problems      # the full check can't pass this recording: why the onsets exist


def test_onsets_a_noise_bump_before_the_sequence():
    segs = [(1600, 50), (0, 40), (1600, 50)]
    x, t0 = render(segs, lead_s=0.5)
    b = int(0.1 * FS)
    x[b:b + 160] += 0.001 * np.sin(2 * np.pi * 1600 * np.arange(160) / FS)
    r = onsets(x, segs)
    assert not r.problems and abs(r.tones[0].start_s - t0) < 0.012, r.report()


def test_onsets_capture_opening():
    segs = [(300, 400)]
    x, t0 = render(segs, lead_s=0.4, level=0.001)
    x[:int(0.05 * FS)] = 0.0                               # the device opening: digital zero
    k = int(0.05 * FS)
    x[k:k + 40] += 0.0003 * np.sin(2 * np.pi * 300 * np.arange(40) / FS)     # and a click as it does
    r = onsets(x, segs)
    assert not r.problems and abs(r.tones[0].start_s - t0) < 0.012, r.report()


def test_onsets_a_lost_tone():
    segs = [(600, 150), (0, 80), (600, 150), (0, 80), (600, 150)]
    x, t0 = render(segs)
    a = int((t0 + 0.23) * FS)
    x[a:a + int(0.15 * FS)] = np.random.default_rng(2).normal(0, 10 ** (-85 / 20), int(0.15 * FS))
    r = onsets(x, segs)
    assert len(r.problems) == 1 and "600 Hz" in r.problems[0], r.report()


def test_onsets_a_lost_tone_with_the_headset_release():
    segs = [(600, 150), (0, 80), (600, 150)]
    x, t0 = render(segs)
    _headset(x, int(t0 * FS), int(0.15 * FS), cut_ms=110)
    a = int((t0 + 0.23) * FS)
    x[a:a + int(0.15 * FS)] = np.random.default_rng(2).normal(0, 10 ** (-85 / 20), int(0.15 * FS))
    assert onsets(x, segs).problems


def test_onsets_a_late_tone():
    segs = [(1200, 50), (0, 30), (900, 70)]
    late = [(1200, 50), (0, 90), (900, 70)]
    x, _ = render(late)
    r = onsets(x, segs)
    assert r.problems, r.report()


def test_onsets_wrong_pitch():
    x, _ = render([(1000, 100), (0, 100), (1000, 100)])
    r = onsets(x, [(600, 100), (0, 100), (600, 100)])
    print(r.report())
    assert r.problems or r.quiet, r.report()


def test_onsets_nothing_played():
    x, _ = render([(1000, 100)], level=0.0)
    r = onsets(x, [(1000, 100)])
    assert r.quiet and r.problems, r.report()


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


# --- audio_report.py: the calibration report ---

import audio_report as R  # noqa: E402


def tone_with(hz=1500, s=1.0, amp=0.1, h3_dbc=None, hum_amp=0.0, noise_db=-80, seed=2):
    rng = np.random.default_rng(seed)
    t = np.arange(int(s * FS)) / FS
    x = amp * np.sin(2 * np.pi * hz * t)
    if h3_dbc is not None:
        x += amp * 10 ** (h3_dbc / 20) * np.sin(2 * np.pi * 3 * hz * t)
    pad = np.zeros(int(0.5 * FS))
    y = np.concatenate([pad, x, pad])
    y += hum_amp * np.sin(2 * np.pi * 60 * np.arange(len(y)) / FS)   # mains hum: all the way through
    return y + rng.normal(0, 10 ** (noise_db / 20), len(y))


def test_report_tone_level_and_thd():
    r = R.report(tone_with(h3_dbc=-20), FS)
    assert len(r.sounds) == 1
    s = r.sounds[0]
    assert abs(s.hz - 1500) < 20
    assert abs(s.rms - (-23.0)) < 0.5          # a 0.1 sine: -20 dBFS peak, -23 rms
    assert abs(s.thd - 10.0) < 1.0             # one harmonic 20 dB down: 10 %, above the voice band too


def test_report_hum_notched():
    clean = R.report(tone_with(), FS)
    hummy = R.report(tone_with(hum_amp=0.05), FS)
    assert hummy.mains == [60] and clean.mains == []
    assert abs(hummy.sounds[0].rms - clean.sounds[0].rms) < 0.2
    assert hummy.floor < -60                   # the hum is out of the floor


def test_report_click_and_steady_line():
    x = tone_with()
    x[int(0.25 * FS)] += 0.5                   # a click in the lead-in
    t = np.arange(len(x)) / FS
    x += 0.001 * np.sin(2 * np.pi * 780 * t)   # a steady whine, -63 dBFS
    r = R.report(x, FS)
    assert any(abs(t0 - 0.25) < 0.01 for t0, _ in r.pops)
    assert any(abs(f - 780) < 8 for f, _, _ in r.spurs)


def test_report_gain_against_the_source():
    rng = np.random.default_rng(3)
    src = np.concatenate([np.zeros(FS // 2)] + [rng.normal(0, 0.1, FS // 4) * np.hanning(FS // 4) if k % 2 else
                                                np.zeros(FS // 8) for k in range(16)])
    rec = np.concatenate([np.zeros(FS), 10 ** (-12 / 20) * src, np.zeros(FS)]) + rng.normal(0, 1e-4, len(src) + 2 * FS)
    r = R.report(rec, FS, ref=(src, FS))
    assert abs(r.ref["start_s"] - 1.0) < 0.03
    assert abs(r.ref["loud_gain"] - (-12.0)) < 0.5
    assert abs(r.ref["word_gain"] - (-12.0)) < 0.5 and not r.ref["odd_words"]


def test_report_one_bad_word_listed_not_counted():
    rng = np.random.default_rng(3)
    src = np.concatenate([np.zeros(FS // 2)] + [rng.normal(0, 0.1, FS // 4) * np.hanning(FS // 4) if k % 2 else
                                                np.zeros(FS // 8) for k in range(16)])
    rec = np.concatenate([np.zeros(FS), 10 ** (-12 / 20) * src, np.zeros(FS)]) + rng.normal(0, 1e-4, len(src) + 2 * FS)
    a = FS + FS // 2 + 3 * (FS // 4 + FS // 8) // 2      # inside the second word: a garbled frame, 10 dB louder
    rec[a:a + FS // 10] *= 3.16
    r = R.report(rec, FS, ref=(src, FS))
    assert abs(r.ref["word_gain"] - (-12.0)) < 0.5
    assert len(r.ref["odd_words"]) == 1, r.ref["odd_words"]


def room(s=4.0, seed=5, lines=((60, 0.02), (180, 0.002)), noise_db=-80):
    """A room: mains hum and its third harmonic over a little noise"""
    rng = np.random.default_rng(seed)
    t = np.arange(int(s * FS)) / FS
    return sum(a * np.sin(2 * np.pi * hz * t + hz) for hz, a in lines) + rng.normal(0, 10 ** (noise_db / 20), len(t))


def test_report_room_learned():
    amb = R.learn_ambient(room(), FS)
    hz = [h for h, _ in amb["lines"]]
    assert any(abs(h - 60) <= 1 for h in hz) and any(abs(h - 180) <= 1 for h in hz), amb["lines"]
    assert len(hz) == 2, amb["lines"]
    assert amb["floor"] < -70


def test_report_room_notched_radio_buzz_kept():
    amb = R.learn_ambient(room(), FS)
    x = tone_with(s=1.5) + room(2.5, seed=6)
    t = np.arange(len(x)) / FS
    buzz = sum(0.003 * np.sin(2 * np.pi * k * 120 * t) for k in (3, 4, 5))   # the radio's 120 Hz family, -50 dBFS
    clean = R.report(x, FS, ambient=amb)
    r = R.report(x + buzz, FS, ambient=amb)
    assert not clean.room["radio"], clean.room
    assert any(abs(h - 60) <= 2 for h in r.room["notched"])
    assert {round(h / 120) for h, _, _ in r.room["radio"]} >= {3, 4, 5}, r.room
    assert abs(clean.sounds[0].rms - (-23.0)) < 0.3
    assert r.floor > clean.floor + 20                  # the buzz stays in the floor: the radio's, not the room's


def test_report_room_hum_louder_on_the_radio():
    amb = R.learn_ambient(room(), FS)
    r = R.report(room(3.0, seed=7, lines=((60, 0.1), (180, 0.002))), FS, ambient=amb)
    assert not any(abs(h - 60) <= 2 for h in r.room["notched"]), r.room
    assert any(abs(h - 60) <= 2 and d > 10 for h, _, d in r.room["radio"]), r.room
    assert any(abs(h - 180) <= 2 for h in r.room["notched"])


def test_report_room_buzz_on_a_carrier():
    """An FM carrier with a buzz, between silences: the buzz is a sound against the silence, still the radio's"""
    amb = R.learn_ambient(room(), FS)
    x = room(4.0, seed=8)
    t = np.arange(len(x)) / FS
    on = (t > 1.0) & (t < 3.0)
    x[on] += sum(0.003 * np.sin(2 * np.pi * k * 120 * t[on]) for k in (4, 5, 7))
    r = R.report(x, FS, ambient=amb)
    assert {round(h / 120) for h, _, _ in r.room["radio"]} >= {4, 5, 7}, r.room


def speechlike(seed=4, words=12):
    """Words of shaped noise, each with its own spectrum (two of the match bands loud), with gaps"""
    rng = np.random.default_rng(seed)
    out = [np.zeros(FS // 2)]
    f = np.fft.rfftfreq(FS // 4, 1 / FS)
    for _ in range(words):
        shape = np.full(len(f), 0.05)
        for lo, hi in rng.choice(np.array(R.MATCH_BANDS), 2, replace=False):
            shape[(f >= lo) & (f < hi)] = 1.0
        w = np.fft.irfft(np.fft.rfft(rng.normal(0, 1, FS // 4)) * shape, FS // 4)
        out += [0.1 * w / np.sqrt(np.mean(w ** 2)) * np.hanning(FS // 4), np.zeros(FS // 8)]
    return np.concatenate(out)


def test_report_frames_matched_bad_ones_out():
    rng = np.random.default_rng(9)
    src = speechlike()
    delay = FS + 37                                    # 1 s and 2.3 ms
    rec = np.concatenate([np.zeros(delay), 10 ** (-12 / 20) * src, np.zeros(FS)])
    rec += rng.normal(0, 1e-4, len(rec))
    w2 = delay + FS // 2 + 2 * (FS // 4 + FS // 8) + FS // 16      # inside the third word
    w5 = delay + FS // 2 + 5 * (FS // 4 + FS // 8) + FS // 16      # inside the sixth
    rec[w2:w2 + int(0.06 * FS)] = 0                                # a missing frame: silence
    rec[w5:w5 + int(0.06 * FS)] = rng.normal(0, 0.05, int(0.06 * FS))   # a garbled one: other spectrum, louder
    r = R.report(rec, FS, ref=(src, FS))
    m = r.ref["match"]
    assert abs(m["lag_s"] - delay / FS) < 0.002, m["lag_s"]
    assert abs(m["gain"] - (-12.0)) < 0.3, m
    found = [t for t, _, _, _ in m["bad"]]
    for at in (w2 / FS, w5 / FS):
        assert any(abs(t - at) < 0.04 for t in found), (at, m["bad"])
    assert m["match_pct"] > 85


def test_report_frames_coloring_not_bad():
    """A steady tilt (a speaker's response) is coloring, not bad frames"""
    src = speechlike(seed=5)
    X = np.fft.rfft(src)
    f = np.fft.rfftfreq(len(src), 1 / FS)
    tilt = np.where(f < 1200, 1.0, 0.5)                # the top two bands 6 dB down
    rec = np.concatenate([np.zeros(FS), 0.25 * np.fft.irfft(X * tilt, len(src)), np.zeros(FS // 2)])
    rec += np.random.default_rng(2).normal(0, 1e-4, len(rec))
    m = R.report(rec, FS, ref=(src, FS)).ref["match"]
    assert m["match_pct"] > 95 and not m["bad"], m
    assert m["coloring"][3] < m["coloring"][0] - 4
