# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Speaker checks from a microphone recording (the speaker tests, test_speaker.py; README.md, "Speaker tests").

A test plays a known sequence of tones and silences (a beep pattern, or a bench FM program) and records the radio's
speaker. check_tones() finds the sequence in the recording and measures each tone between the points where its
amplitude rises and falls fastest (the direct sound's edges: a room's echo builds up and dies away slower), so a lost
start or end shows as a short tone.
It also asks that the plateau be flat (no step at the amplifier's start, no fade), that each silence be silent, and
that nothing between or around the tones is a pop: broadband energy outside the tones' harmonics far above the
recording's own floor. Pure Python and numpy, no radio: test_audio_check.py runs it on synthetic signals."""
import os
import re
import wave
from dataclasses import dataclass, field

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

WIN_MS = 15.0           # tone level analysis window (Blackman: tones 300 Hz apart and 25 dB apart stay apart)
HOP_MS = 1.0
STEP_WIN_MS = 5.0       # the window steps on a plateau are measured with (at least three periods of the tone)
POP_WIN_MS = 10.0       # pop analysis frames
HARMONICS = 6           # harmonics of each tone kept out of the pop measure (the speaker distorts)


def beep_patterns(root=ROOT):
    """The beep patterns from core/radio/beep.c, so the tests copy none: {name: [(hz, ms), ...]} with the
    debug link's names (BEEP_TX_PERMIT -> tx_permit); hz 0 is a silence."""
    src = open(os.path.join(root, "core", "radio", "beep.c")).read()
    tables = {}
    for name, body in re.findall(r"static const beep_seg_t (p_\w+)\[\]\s*=\s*\{(.*?)\};", src, re.S):
        tables[name] = [(int(h), int(m)) for h, m in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}", body)]
    out = {}
    for beep, table in re.findall(r"\[BEEP_(\w+)\]\s*=\s*PAT\((p_\w+)\)", src):
        out[beep.lower()] = tables[table]
    if not out:
        raise ValueError("no beep patterns found in core/radio/beep.c")
    return out


def load_wav(path):
    """A mono 16-bit WAV as floats (full scale 1.0) and its rate"""
    with wave.open(path) as w:
        if w.getsampwidth() != 2:
            raise ValueError(f"{path}: {8 * w.getsampwidth()}-bit samples, want 16")
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64) / 32768.0
        if w.getnchannels() > 1:
            x = x.reshape(-1, w.getnchannels())[:, 0]
        return x, w.getframerate()


def db(v):
    return 20.0 * np.log10(np.maximum(v, 1e-9))


def tone_level(x, fs, hz, win_ms=WIN_MS, hop_ms=HOP_MS):
    """The amplitude of a sine at hz (dBFS, a full-scale sine is 0) in a sliding Blackman window; one value per hop,
    each for the window centered on it"""
    win = max(int(fs * win_ms / 1000), 8)
    hop = max(int(fs * hop_ms / 1000), 1)
    t = np.arange(win) / fs
    w = np.blackman(win)
    ref = w * np.exp(-2j * np.pi * hz * t)
    n = max((len(x) - win) // hop + 1, 0)
    if n == 0:
        return np.zeros(0)
    frames = np.lib.stride_tricks.as_strided(x, (n, win), (x.strides[0] * hop, x.strides[0]))
    amp = np.abs(frames @ ref) * 2 / w.sum()
    # pad so index i is the window centered at i * hop
    pad = int(round(win / 2 / hop))
    return db(np.concatenate([np.full(pad, amp[0]), amp]))


def residual_level(x, fs, freqs, win_ms=POP_WIN_MS):
    """Per frame (win_ms long, every win_ms / 2: a click on one frame's edge is in the middle of the next): the
    energy (dBFS rms) left once the bins near each tone's harmonics and below 150 Hz are taken out. A pop or a noise
    burst is broadband and shows here; the tones don't."""
    win = int(fs * win_ms / 1000)
    hop = win // 2
    n = (len(x) - win) // hop + 1 if len(x) >= win else 0
    if n == 0:
        return np.zeros(0)
    frames = np.lib.stride_tricks.as_strided(x, (n, win), (x.strides[0] * hop, x.strides[0]))
    frames = frames - frames.mean(axis=1, keepdims=True)
    w = np.hanning(win)
    spec = np.abs(np.fft.rfft(frames * w, axis=1)) ** 2
    f = np.fft.rfftfreq(win, 1 / fs)
    keep = f >= 150
    step = fs / win
    for hz in freqs:
        for k in range(1, HARMONICS + 1):
            keep &= np.abs(f - k * hz) > 1.6 * step
    p = spec[:, keep].sum(axis=1) * 2 / (w ** 2).sum() / win
    return 10 * np.log10(np.maximum(p, 1e-18))


@dataclass
class Tone:
    hz: int
    want_ms: float
    start_s: float = None       # the edges, seconds into the recording
    end_s: float = None
    plateau_db: float = None
    ripple_db: float = None     # max - min on the plateau
    step_db: float = None       # the largest change on the plateau within a few ms

    @property
    def got_ms(self):
        return None if self.start_s is None else (self.end_s - self.start_s) * 1000


@dataclass
class Result:
    tones: list = field(default_factory=list)
    gaps: list = field(default_factory=list)       # (want_ms, got_ms, depth_db below the louder neighbor)
    pops: list = field(default_factory=list)       # (time_s, dB over the floor, ms)
    floor_res_db: float = None
    floor_db: float = None
    snr_db: float = None
    problems: list = field(default_factory=list)
    quiet: bool = False     # the sequence never rose far enough over the floor to be measured

    def report(self):
        rows = [f"floor {self.floor_db:.1f} dBFS, SNR {self.snr_db:.0f} dB" if self.floor_db is not None else ""]
        for t in self.tones:
            if t.start_s is None:
                rows.append(f"  {t.hz} Hz {t.want_ms:.0f} ms: not found")
            else:
                rows.append(f"  {t.hz} Hz {t.want_ms:.0f} ms: got {t.got_ms:.1f} ms at {t.start_s:.3f} s, "
                            f"{t.plateau_db:.1f} dBFS, ripple {t.ripple_db:.1f} dB, step {t.step_db:.1f} dB")
        for want, got, depth in self.gaps:
            rows.append(f"  gap {want:.0f} ms: got {got:.1f} ms, {depth:.0f} dB down")
        for t, over, ms in self.pops:
            rows.append(f"  pop at {t:.3f} s: {ms:.0f} ms, +{over:.0f} dB")
        rows += [f"  PROBLEM: {p}" for p in self.problems]
        return "\n".join(r for r in rows if r)


def _crossing_up(lv, a, b, thr, hold=1):
    """The first rise to thr in [a, b) that stays there for hold values (a noise blip isn't a tone)"""
    above = lv[a:b] >= thr
    idx = [k for k in np.nonzero(above)[0] if above[k:k + hold].all()]
    if len(idx) == 0:
        return None
    i = a + idx[0]
    if i == 0:
        return float(i)
    y0, y1 = lv[i - 1], lv[i]
    return i - 1 + (thr - y0) / (y1 - y0) if y1 != y0 else float(i)


def check_tones(x, fs, segs, search=None, dur_tol_ms=10.0, ripple_db=12.0, step_db=8.0, gap_db=8.0, pop_db=20.0,
                min_snr_db=30.0, edge_ms=4.0, guard_ms=(150.0, 300.0), long_tol_ms=40.0, rel_db=10.0, ring_ms=60.0,
                edge_db=6.0):
    """Find segs ([(hz, ms)], hz 0 a silence) in x and check it. search (start_s, end_s) limits where the first tone
    may start. Problems:
    - a tone shorter than asked by more than dur_tol_ms (a lost start or end), or longer by more than long_tol_ms
      (a speaker and a room ring on after a tone, so a recording's tone is often a little long);
    - its level moving by more than ripple_db over the length asked for (a room swells and fades a tone by several
      dB as its echoes add up), by more than step_db within a few ms (a step as an amplifier comes up,
      a dropout; measured in a short window), or differing by ripple_db from another tone at its frequency (edge_ms plus half a window in from each
      edge, so the fades don't count): a step as the amplifier comes up, a fade;
    - a silence longer than asked by dur_tol_ms, shorter by long_tol_ms (the ring), or less than gap_db below its
      louder neighbor;
    - a pop or noise burst from guard_ms[0] before the first tone to guard_ms[1] after the last: residual_level more
      than pop_db over the window's floor, or, on a tone and ring_ms after it (its reverberation), within rel_db of
      the tone's own level (at its edges, more than edge_db louder than the tone)."""
    r = Result()
    tones = [(h, m) for h, m in segs if h]
    if not tones:
        raise ValueError("no tone in the sequence")
    hop = HOP_MS / 1000
    levels = {h: tone_level(x, fs, h) for h in {h for h, _ in tones}}
    n = min(len(v) for v in levels.values())
    a = 0 if search is None else max(int(search[0] / hop), 0)
    b = n if search is None else min(int(search[1] / hop), n)
    first = levels[tones[0][0]][:n]
    if b - a < 10:
        raise ValueError("the search window is outside the recording")
    r.floor_db = float(np.percentile(first[a:b], 10))
    peak = float(first[a:b].max())
    r.snr_db = peak - r.floor_db
    if r.snr_db < min_snr_db:
        r.quiet = True
        r.problems.append(f"the first tone ({tones[0][0]} Hz) is only {r.snr_db:.0f} dB over the floor "
                          f"(at least {min_snr_db:.0f} needed): too quiet, or not played")
        r.tones = [Tone(h, m) for h, m in tones]
        return r
    # coarse start: where the whole sequence fits best (the tones' mean levels at their places, summed), so a noise
    # burst or a squelch opening before the first tone isn't taken for it; then each segment measured near there
    span_i, offs, t = int(sum(m for _, m in segs) / 1000 / hop), [], 0.0
    for h, m in segs:
        if h:
            offs.append((h, int(t / 1000 / hop), max(int(m / 1000 / hop), 1)))
        t += m
    last = min(b, n - span_i)
    if last <= a:
        last = a + 1
    score = np.zeros(last - a)
    for h, o, ln in offs:
        c = np.concatenate([[0.0], np.cumsum(levels[h][:n])])
        idx = np.arange(a, last) + o
        ok = idx + ln <= n
        score[ok] += (c[idx[ok] + ln] - c[idx[ok]])      # the sum over the tone: longer tones weigh more
        score[~ok] -= 1e9
    i0 = a + int(np.argmax(score))
    # the fit favors a room's swell over a short tone's true start: take the first sustained rise halfway from the
    # background (just before the fit) to the tone's level, a little before the fit
    t_len = max(int(tones[0][1] / 1000 / hop), 1)
    near = min(t_len, int(0.03 / hop))
    lo = max(a, i0 - near)
    back = float(np.median(first[max(a, lo - int(0.1 / hop)):lo + 1]))
    top = float(first[i0:i0 + t_len].max())
    c = _crossing_up(first, lo, i0 + near, (back + top) / 2, int(min(tones[0][1] / 2, 10) / HOP_MS))
    if c is not None:
        i0 = int(c)
    expect = i0
    prev = None
    win_half = WIN_MS / 2 / HOP_MS
    for hz, ms in segs:
        span = ms / 1000 / hop
        if hz == 0:
            expect += span
            prev = ("gap", ms, expect - span)
            continue
        lv = levels[hz]
        lo = int(max(expect - 30 / HOP_MS, 0))
        t = Tone(hz, ms)
        # the edges where the amplitude rises and falls fastest: the direct sound's, as a room's echo builds up and
        # dies away slower, and for a symmetric window the steepest point is the edge itself (half the amplitude)
        d = np.diff(10 ** (lv / 20))
        last = r.tones[-1] if r.tones else None
        if last is not None and last.end_s is not None:
            if last.hz == hz:       # the same tone again: its rise comes after the last one's fall
                lo = max(lo, int(last.end_s / hop + win_half))
            else:                   # another: it may start right where the last one ends
                lo = max(lo, int(last.end_s / hop - win_half))
        s_hi = int(min(expect + min(span * 0.4, 20 / HOP_MS), n - 2))
        s = e = None
        if s_hi > lo + 1:
            s = lo + int(np.argmax(d[lo:s_hi])) + 0.5
            mid_a, mid_b = int(s + span * 0.3), int(s + span * 0.7) + 1
            plateau = float(np.median(lv[mid_a:mid_b])) if mid_b > mid_a else float(lv[int(s + span / 2)])
            e_lo = int(s + span * 0.5)
            e_hi = int(min(s + span + (long_tol_ms + WIN_MS / 2) / HOP_MS, n - 2))
            if e_hi > e_lo + 1:
                e = e_lo + int(np.argmin(d[e_lo:e_hi])) + 0.5
        if s is None or e is None:
            r.problems.append(f"{hz} Hz {ms} ms: no clean rise and fall around {expect * hop:.3f} s")
            r.tones.append(t)
            expect += span
            prev = ("tone", t)
            continue
        t.start_s, t.end_s, t.plateau_db = s * hop, e * hop, plateau
        inner = int(edge_ms / HOP_MS + win_half)
        pa, pb = int(s) + inner, int(min(e, s + span)) - inner     # the length asked for: a ring after isn't the plateau
        t.ripple_db = float(lv[pa:pb].max() - lv[pa:pb].min()) if pb > pa + 2 else 0.0
        # steps by a short window (no other tone is near on the plateau): a room swells a tone slowly, an amplifier
        # coming up or a lost block changes it at once
        sw = max(STEP_WIN_MS, 3000.0 / hz)
        short = tone_level(x, fs, hz, win_ms=sw)
        w = int(sw / HOP_MS)
        qa, qb = pa, min(pb, len(short))
        t.step_db = float(np.abs(short[qa + w:qb] - short[qa:qb - w]).max()) if qb > qa + w else 0.0
        if t.got_ms < ms - dur_tol_ms:
            r.problems.append(f"{hz} Hz: {t.got_ms:.1f} ms, want {ms:.0f} ms (cut by {ms - t.got_ms:.1f} ms)")
        elif t.got_ms > ms + long_tol_ms:
            r.problems.append(f"{hz} Hz: {t.got_ms:.1f} ms, want {ms:.0f} ms ({t.got_ms - ms:.1f} ms long)")
        if t.ripple_db > ripple_db:
            r.problems.append(f"{hz} Hz {ms} ms: the level moves by {t.ripple_db:.1f} dB on the plateau "
                              f"(at most {ripple_db:.0f})")
        if t.step_db > step_db:
            r.problems.append(f"{hz} Hz {ms} ms: a step or dropout of {t.step_db:.1f} dB within {sw:.0f} ms "
                              f"(at most {step_db:.0f})")
        if prev and prev[0] == "gap":
            _, gms, _ = prev
            before = r.tones[-1] if r.tones else None
            if before and before.end_s is not None:
                got = (t.start_s - before.end_s) * 1000
                ga, gb = int(before.end_s / hop + win_half), int(t.start_s / hop - win_half)
                deepest = -999.0
                if gb > ga:
                    louder = max(before.plateau_db, t.plateau_db)
                    quietest = min(float(levels[before.hz][ga:gb].min()), float(lv[ga:gb].min()))
                    deepest = louder - quietest
                r.gaps.append((gms, got, deepest))
                if got > gms + dur_tol_ms or got < gms - long_tol_ms:
                    r.problems.append(f"silence {gms:.0f} ms: got {got:.1f} ms")
                if deepest < gap_db:
                    r.problems.append(f"silence {gms} ms: only {deepest:.0f} dB down (at least {gap_db:.0f})")
        r.tones.append(t)
        expect = e      # follow the measured end: small offsets don't add up
        prev = ("tone", t)
    found = [t for t in r.tones if t.start_s is not None]
    for hz in sorted({t.hz for t in found}):
        same = [t.plateau_db for t in found if t.hz == hz]
        if len(same) > 1 and max(same) - min(same) > ripple_db:
            r.problems.append(f"{hz} Hz: the tones' levels differ by {max(same) - min(same):.1f} dB "
                              f"({', '.join(f'{v:.1f}' for v in same)} dBFS)")
    if found:
        res = residual_level(x, fs, [h for h, _ in tones])
        fr = POP_WIN_MS / 2000     # the frames' step
        ra = int(max(found[0].start_s - guard_ms[0] / 1000, 0) / fr)     # frame k starts at k * fr
        rb = min(int((found[-1].end_s + guard_ms[1] / 1000) / fr), len(res))
        # near a tone (its edges included: a 2 ms fade spreads a little energy past the notches) the measure may
        # come within rel_db of the tone; elsewhere within pop_db of the window's own floor
        limit = np.full(rb - ra, np.nan)
        for t in found:
            ka, kb = int(t.start_s / fr) - 2, int((t.end_s + ring_ms / 1000) / fr) + 1
            for k in range(max(ka, ra), min(kb, rb)):
                limit[k - ra] = t.plateau_db - 3 - rel_db
        # a tone's edges spread energy past the notches (more than its 2 ms fades alone would, through a small
        # speaker that is louder at a few kHz than at a low tone): a pop there is edge_db louder than the tone
        for t in found:
            for edge in (t.start_s, t.end_s):
                for k in range(int(edge / fr) - 2, int(edge / fr) + 2):   # the frames holding the edge
                    if ra <= k < rb:
                        limit[k - ra] = t.plateau_db - 3 + edge_db
        quiet = np.isnan(limit)
        r.floor_res_db = float(np.median(res[ra:rb][quiet])) if quiet.any() else float(np.min(res))
        limit = np.fmax(limit, r.floor_res_db + pop_db)     # a quiet tone's limit is never under the floor's
        over = res[ra:rb] - limit
        k = 0
        while k < rb - ra:
            if over[k] > 0:
                j = k
                while j < rb - ra and over[j] > 0:
                    j += 1
                worst = float(res[ra + k:ra + j].max() - r.floor_res_db)
                t0, ms = (ra + k) * fr, (j - k) * fr * 1000
                if r.pops and t0 - (r.pops[-1][0] + r.pops[-1][2] / 1000) < 0.03:     # one burst, not several
                    p0, o0, _ = r.pops[-1]
                    r.pops[-1] = (p0, max(o0, worst), (t0 - p0) * 1000 + ms)
                else:
                    r.pops.append((t0, worst, ms))
                k = j
            k += 1
        for t0, o, ms in r.pops:
            r.problems.append(f"a pop or noise burst at {t0:.3f} s, {ms:.0f} ms, {o:.0f} dB over the floor")
    return r


# A sound that stops falls this far at once (its direct sound); what follows is the room's echo dying away, at a rate
# that depends on the room and the microphone's place, not on the radio
DIRECT_DB = 10.0


def burst_ms(x, fs, freqs, t0_s, ms, drop_db=DIRECT_DB):
    """A noise burst's own length (one of check_tones' pops: start t0_s, ms long): from its start to the last frame
    within drop_db of its peak. The pop's length runs on until the echo falls to the floor's limit, so a louder burst
    (the volume, the microphone closer) measures longer; this doesn't. A room that drops less than drop_db at once
    only makes it longer, never shorter"""
    res = residual_level(x, fs, freqs)
    fr = POP_WIN_MS / 2000
    a = max(int(round(t0_s / fr)), 0)
    seg = res[a:min(a + int(round(ms / 1000 / fr)), len(res))]
    if len(seg) == 0:
        return ms
    keep = np.nonzero(seg >= seg.max() - drop_db)[0]
    return float((keep[-1] + 1) * fr * 1000)


def check_wav(path, segs, **kw):
    x, fs = load_wav(path)
    return check_tones(x, fs, segs, **kw)


def program_segs(program):
    """A bench FM program ([{"kind": "tone"|"silence", "hz", "s"}]) as check_tones' segments"""
    out = []
    for p in program:
        if p["kind"] == "tone":
            out.append((int(p["hz"]), p["s"] * 1000))
        elif p["kind"] == "silence":
            out.append((0, p["s"] * 1000))
        else:
            raise ValueError(f"a {p['kind']} segment can't be checked by tone")
    return out


if __name__ == "__main__":      # by hand: audio_check.py FILE.wav BEEP|HZ:MS,...
    import sys
    pats = beep_patterns()
    spec = sys.argv[2]
    segs = pats[spec] if spec in pats else [tuple(float(v) for v in s.split(":")) for s in spec.split(",")]
    res = check_wav(sys.argv[1], [(int(h), m) for h, m in segs])
    print(res.report())
    sys.exit(1 if res.problems else 0)
