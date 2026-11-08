# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Speaker checks from a microphone recording (the speaker tests, test_speaker.py; README.md, "Speaker tests").

A test plays a known sequence of tones and silences (a beep pattern, or a bench FM program) and records the radio's
speaker. check_tones() finds the sequence in the recording and measures each tone between the points where its
amplitude rises and falls fastest (the direct sound's edges: a room's echo builds up and dies away slower), so a lost
start or end shows as a short tone.
It also asks that the tone hold (no rise as an amplifier comes up, no dropout), that each silence be silent, and
that nothing between or around the tones is a pop: broadband energy outside the tones' harmonics far above the
recording's own floor. Pure Python and numpy, no radio: test_audio_check.py runs it on synthetic signals.

A microphone with its own processing (a headset's noise suppression) fades a steady tone by 20 dB or more after a
few tens of ms and lets it back up at once, more so the quieter it hears it; a room swells and fades one too. Neither
is the radio's: a fade is reported, not failed. What the radio does differs in kind: a lost block or a muted
amplifier takes the level to the floor within a millisecond or two, an amplifier coming up late puts it above where
the tone started, and a stream that stops early leaves only the room's echo, dying away, until the tone's time is up.
A fade instead leaves the tone sounding until its end, where its direct sound stops at once (DIRECT_DB)."""
import os
import re
import wave
from dataclasses import dataclass, field

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

WIN_MS = 15.0           # tone level analysis window (Blackman: tones 300 Hz apart and 25 dB apart stay apart)
HOP_MS = 1.0
POP_WIN_MS = 10.0       # pop analysis frames
ABRUPT_MS = 3.0         # a change within this long is abrupt: a lost block or an amplifier, not a room or a microphone
GONE_DB = 20.0          # a tone within this of the floor is gone: a dropout, not a microphone's fade
# A sound that stops falls this far at once (its direct sound); what follows is the room's echo dying away, at a rate
# that depends on the room and the microphone's place, not on the radio
DIRECT_DB = 10.0
HARMONICS = 6           # harmonics of each tone kept out of the pop measure (the speaker distorts)
SWEEP_HZ = -1           # beep_patterns' hz for the sweep segment (beep.h BEEP_HZ_SWEEP)


def beep_patterns(root=ROOT):
    """The beep patterns from core/radio/beep.c, so the tests copy none: {name: [(hz, ms), ...]} with the
    debug link's names (BEEP_TX_PERMIT -> tx_permit); hz 0 is a silence, SWEEP_HZ the speaker test's sweep."""
    src = open(os.path.join(root, "core", "radio", "beep.c")).read()
    tables = {}
    for name, body in re.findall(r"static const beep_seg_t (p_\w+)\[\]\s*=\s*\{(.*?)\};", src, re.S):
        tables[name] = [(SWEEP_HZ if h == "BEEP_HZ_SWEEP" else int(h), int(m))
                        for h, m in re.findall(r"\{\s*(\d+|BEEP_HZ_SWEEP)\s*,\s*(\d+)\s*\}", body)]
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


def tone_envelope(x, fs, hz, hop_ms=HOP_MS):
    """The amplitude of the hz component (dBFS) averaged over whole periods, at least 2 ms: fine enough in time to
    show a few ms lost, where tone_level's window would smooth them over. Index i is centered at i * hop_ms."""
    per = max(int(round(fs / hz)), 1)
    span = per * max(1, int(np.ceil(0.002 * fs / per)))
    z = x * np.exp(-2j * np.pi * hz * np.arange(len(x)) / fs)
    c = np.concatenate([[0.0], np.cumsum(z)])
    amp = 2 * np.abs((c[span:] - c[:-span]) / span)       # amp[k]: samples k .. k + span, centered at k + span / 2
    hop = max(int(fs * hop_ms / 1000), 1)
    k = np.arange(0, len(x), hop) - span // 2
    out = np.full(len(k), amp[0] if len(amp) else 0.0)
    ok = (k >= 0) & (k < len(amp))
    out[ok] = amp[k[ok]]
    return db(out)


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
    step_db: float = None       # the largest abrupt fall on the plateau (ABRUPT_MS)
    rise_db: float = None       # the most it rises over where it started (an amplifier coming up)
    fade_db: float = None       # a fall that left it sounding (a microphone's processing, the room), or None

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
                            f"{t.plateau_db:.1f} dBFS, ripple {t.ripple_db:.1f} dB, step {t.step_db:.1f} dB, "
                            f"rise {t.rise_db:.1f} dB" +
                            (f", faded {t.fade_db:.0f} dB (the microphone or the room)" if t.fade_db else ""))
        for want, got, depth in self.gaps:
            rows.append(f"  gap {want:.0f} ms: got {got:.1f} ms, {depth:.0f} dB down")
        for t, over, ms in self.pops:
            rows.append(f"  pop at {t:.3f} s: {ms:.0f} ms, +{over:.0f} dB")
        rows += [f"  PROBLEM: {p}" for p in self.problems]
        return "\n".join(r for r in rows if r)


def _past_fade(lv, d, e, near_a, e_hi, win_half, floor_db, gone_db=GONE_DB, hold_db=DIRECT_DB * 0.6, fade_max_db=35.0):
    """(end, fade dB or None). e is the steepest fall after the tone's middle. When it comes well before the end asked
    for, and the level after it holds (a tone still sounding: gone_db over the floor, at most fade_max_db under where
    it was, hold_db over what follows) until
    a fall of hold_db near the end asked for, that fall is the end: the earlier one was a fade (a microphone's
    processing). A stream that stopped at e leaves only its echo, dying away with no such fall."""
    if e >= near_a:         # the fall is where the tone should end: it did
        return e, None
    a = max(near_a, int(e + 2 * win_half))
    if e_hi <= a + 1:
        return e, None
    e2 = a + int(np.argmin(d[a:e_hi])) + 0.5
    k = int(win_half + 3)
    before, after = int(e2) - k, int(e2) + k + 1
    held = lv[int(e + win_half + 2):before]
    if before < 0 or after >= len(lv) or len(held) < 3:
        return e, None
    hold = float(np.median(held))
    was = float(lv[max(int(e) - k, 0)])
    if hold < floor_db + gone_db or hold < was - fade_max_db or lv[before] - lv[after] < hold_db or hold - lv[after] < hold_db:
        return e, None
    return e2, was - hold


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
                edge_db=6.0, gone_db=GONE_DB):
    """Find segs ([(hz, ms)], hz 0 a silence) in x and check it. search (start_s, end_s) limits where the first tone
    may start. Problems:
    - a tone shorter than asked by more than dur_tol_ms (a lost start or end), or longer by more than long_tol_ms
      (a speaker and a room ring on after a tone, so a recording's tone is often a little long);
    - over the length asked for (edge_ms plus half a window in from each edge): its level rising more than step_db
      over where it started (an amplifier coming up), or falling by more than step_db within ABRUPT_MS to within
      gone_db of the floor (a dropout: a lost block, a muted amplifier); or its level differing by ripple_db from
      another tone at its frequency. A fall that leaves the tone sounding is a fade (the module's notes): reported;
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
                e, t.fade_db = _past_fade(lv, d, e, int(s + span - (dur_tol_ms + WIN_MS / 2) / HOP_MS), e_hi, win_half,
                                          r.floor_db, gone_db)
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
        # an amplifier coming up late leaves the tone's start low; a room or a microphone only fades it from there
        early = float(np.median(lv[pa:pa + int(10 / HOP_MS)])) if pb > pa + int(10 / HOP_MS) else plateau
        t.rise_db = max(float(lv[pa:pb].max()) - early, 0.0) if pb > pa + 2 else 0.0
        # a dropout by the fine envelope: an abrupt fall to near the floor (no other tone is near on the plateau)
        fine = tone_envelope(x, fs, hz)
        f_floor = float(np.percentile(fine[a:b], 10))
        w = int(ABRUPT_MS / HOP_MS)
        qa, qb = pa, min(pb, len(fine))
        t.step_db, gone = 0.0, False
        if qb > qa + w:
            seg = fine[qa:qb]
            low = np.array([seg[i + 1:i + w + 1].min() for i in range(len(seg) - w)])
            fall = seg[:len(low)] - low
            t.step_db = float(fall.max())
            gone = bool(((fall > step_db) & (low < f_floor + gone_db)).any())
        if t.got_ms < ms - dur_tol_ms:
            r.problems.append(f"{hz} Hz: {t.got_ms:.1f} ms, want {ms:.0f} ms (cut by {ms - t.got_ms:.1f} ms)")
        elif t.got_ms > ms + long_tol_ms:
            r.problems.append(f"{hz} Hz: {t.got_ms:.1f} ms, want {ms:.0f} ms ({t.got_ms - ms:.1f} ms long)")
        if t.rise_db > step_db:
            r.problems.append(f"{hz} Hz {ms} ms: a step: it rises {t.rise_db:.1f} dB over where it started (at most "
                              f"{step_db:.0f}): an amplifier coming up")
        if gone:
            r.problems.append(f"{hz} Hz {ms} ms: a dropout: {t.step_db:.1f} dB down within {ABRUPT_MS:.0f} ms, to "
                              f"within {gone_db:.0f} dB of the floor")
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


def sweep_spec(root=ROOT):
    """The sweep (beep.h BEEP_SWEEP): (lowest Hz, highest Hz, its length in ms)"""
    src = open(os.path.join(root, "core", "radio", "beep.h")).read()
    lo, hi = (int(re.search(rf"#define BEEP_SWEEP_{k}_HZ\s+(\d+)", src).group(1)) for k in ("LO", "HI"))
    ms = [m for h, m in beep_patterns(root)["sweep"] if h == SWEEP_HZ]
    if len(ms) != 1:
        raise ValueError("core/radio/beep.c: the sweep pattern has no single sweep segment")
    return lo, hi, ms[0]


@dataclass
class Sweep:
    start_s: float = None           # where the sweep starts in the recording
    frames: int = 0                 # analysis frames inside it
    broken: int = 0                 # of those, frames with the sweep jumping (energy just above its frequency)
    off_db: float = None            # median energy 150-600 Hz above the sweep (harmonics excluded), relative to it
    response: list = field(default_factory=list)    # (Hz, dB relative to the loudest band): the speaker as heard
    snr_db: float = None
    problems: list = field(default_factory=list)
    quiet: bool = False

    def report(self):
        if self.start_s is None:
            return "  sweep not found\n" + "\n".join(f"  PROBLEM: {p}" for p in self.problems)
        rows = [f"sweep at {self.start_s:.3f} s, SNR {self.snr_db:.0f} dB; {self.broken}/{self.frames} frames broken, "
                f"median energy above the sweep {self.off_db:.1f} dB",
                "  response: " + " ".join(f"{hz:.0f}:{d:+.0f}" for hz, d in self.response)]
        rows += [f"  PROBLEM: {p}" for p in self.problems]
        return "\n".join(rows)


def check_sweep(x, fs, lo, hi, ms, off_db=-30.0, max_broken=0.05, min_snr_db=30.0):
    """The speaker test's sweep (BEEP_SWEEP), lo to hi Hz over ms, in a recording: where it is, the speaker's response
    along it, and whether it plays whole. A sweep played whole has nearly all its energy at its frequency and the
    speaker's harmonics of it. One whose audio jumps (samples lost, repeated or played out of order) spreads energy
    on both sides of it at every jump. The room can't fake that above the sweep: its echoes arrive at the pitch the
    sweep had a moment ago, below it. So a frame counts as broken when the energy 150-600 Hz above the sweep (its
    harmonics excluded) is within off_db of the sweep's. More than max_broken of the frames broken is a problem."""
    r = Sweep()
    n = 1024 if fs > 12000 else 512
    hop = n // 4
    win = np.hanning(n)
    fr = np.fft.rfftfreq(n, 1 / fs)
    rate = (hi - lo) / (ms / 1000)
    frames = np.array([np.abs(np.fft.rfft(x[k:k + n] * win)) ** 2 for k in range(0, len(x) - n, hop)])
    if not len(frames):
        r.problems.append("the recording is shorter than one analysis frame")
        return r
    t = (np.arange(len(frames)) * hop + n / 2) / fs
    band = (fr > lo * 0.8) & (fr < min(hi * 1.05, fs / 2))
    peak = fr[np.argmax(frames * band, axis=1)]
    power = frames[:, band].sum(axis=1)
    floor = max(np.percentile(power, 10), 1e-30)     # the quiet before and after the sweep
    loud = power > floor * 1e3
    inside = loud & (peak > lo + 0.1 * (hi - lo)) & (peak < hi - 0.1 * (hi - lo))
    if inside.sum() < 10:
        r.quiet = loud.sum() < 10
        r.problems.append("no sweep found: " + ("the microphone hears too little of the speaker" if r.quiet else
                                                "sound, but no tone rising through the band"))
        return r
    r.start_s = float(np.median(t[inside] - (peak[inside] - lo) / rate))
    r.snr_db = float(10 * np.log10(np.percentile(power[inside], 90) / floor))
    ratios = []
    resp = []
    for i in range(len(frames)):
        f0 = lo + rate * (t[i] - r.start_s)
        if f0 < lo + 50 or f0 > hi - 50 or t[i] - n / 2 / fs < r.start_s:
            continue
        width = max(120.0, 3 * fs / n)
        on = np.abs(fr - f0) < width
        e_on = frames[i][on].sum()
        resp.append((f0, e_on))
        if f0 + 600 > min(4000.0, fs / 2):
            continue        # nothing above it left in the band to look at
        above = (fr > f0 + 150) & (fr < f0 + 600)
        for h in range(2, HARMONICS + 1):
            above &= np.abs(fr - h * f0) >= width
        ratios.append((e_on, 10 * np.log10(max(frames[i][above].sum(), 1e-30) / max(e_on, 1e-30))))
    # Only where the speaker plays the sweep within 25 dB of its loudest: below that (a small speaker's low end) the
    # room's floor alone is close to the sweep
    e_max = max(v for f, v in resp)
    ratios = np.array([d for v, d in ratios if v > e_max * 10 ** -2.5])
    r.frames = len(ratios)
    r.broken = int((ratios > off_db).sum())
    r.off_db = float(np.median(ratios))
    # The response in third-octave bands from the first frames' level per band, relative to the loudest
    centers = [c for c in 200 * 2 ** (np.arange(0, 15) / 3) if lo * 1.12 < c < hi / 1.12]
    levels = []
    for c in centers:
        e = [v for f, v in resp if c / 1.12 <= f < c * 1.12]
        levels.append(10 * np.log10(np.mean(e)) if e else -300.0)
    top = max(levels) if levels else 0
    r.response = [(c, lv - top) for c, lv in zip(centers, levels)]
    # The radio plays an 8 kHz stream: above its band is only the codec's images, a few percent of the energy. Much
    # more is the capture's fault (such a recording also falls below the sweep's lowest pitch), not the radio's
    e_hi = frames[:, fr > 4300].sum()
    above = e_hi / max(e_hi + frames[:, (fr > 100) & (fr < 4000)].sum(), 1e-30)    # a share of the energy
    if fs > 9000 and above > 0.25:
        r.problems.append(f"{100 * above:.0f} % of the energy is above 4.3 kHz, which the radio's 8 kHz audio can't "
                          "make: the recording is broken (microphone capture), record again")
    elif r.snr_db < min_snr_db:
        r.quiet = True
        r.problems.append(f"the sweep is only {r.snr_db:.0f} dB over the floor (want {min_snr_db:.0f})")
    elif r.broken > max_broken * r.frames:
        r.problems.append(f"the sweep breaks up: {r.broken} of {r.frames} frames have energy just above its frequency within "
                          f"{-off_db:.0f} dB of it (audio jumping: samples lost, repeated or out of order)")
    return r


if __name__ == "__main__":      # by hand: audio_check.py FILE.wav BEEP|sweep|HZ:MS,...
    import sys
    pats = beep_patterns()
    spec = sys.argv[2]
    if spec == "sweep":
        x, fs = load_wav(sys.argv[1])
        res = check_sweep(x, fs, *sweep_spec())
        print(res.report())
        sys.exit(1 if res.problems else 0)
    segs = pats[spec] if spec in pats else [tuple(float(v) for v in s.split(":")) for s in spec.split(",")]
    res = check_wav(sys.argv[1], [(int(h), m) for h, m in segs])
    print(res.report())
    sys.exit(1 if res.problems else 0)
