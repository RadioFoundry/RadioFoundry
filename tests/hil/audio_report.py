# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""A level and quality report of any speaker recording, for audio level calibration:

    python3 tests/hil/audio_report.py REC.wav... [--ref SOURCE.wav [--ref-s S]] [--ambient ROOM.json]
    python3 tests/hil/audio_report.py --learn-ambient ROOM.json ROOM.wav

Per recording: the mains hum (50 and 60 Hz, which is found and notched out, up to its 6th harmonic), the floor and
each sound in the voice band (level, peak, crest factor, how steady it holds, clipped samples; a tone's pitch, THD
and THD+N with the wide band, so harmonics above the voice band count), clicks (bursts above 4 kHz far over their
own surroundings: a steady harmonic or hiss isn't one), steady single-frequency noise in the quiet parts, and the
speech level (the loud frames, as ITU-T P.56 does).

The room has its own steady noise (mains hum, a fan, a computer's whine), and so may the radio (supply ripple on FM).
--learn-ambient takes a recording of the room with the radio silent and saves its steady lines, its noise per third
octave and its floor. --ambient then notches only those lines, and only where the recording holds them at the room's
level: a line 6 dB or more over the room's, or one the room doesn't have, is the radio's. It is listed and stays in
every level.

With --ref, the source the recording played (the same voice sent as FM and as DMR, tests/data/voice): the recording
is aligned to it on the two envelopes and its level taken only where the source speaks, so a receiver's noise
between words (FM) or silence there (a vocoder) doesn't count. Then frame by frame (10 ms, aligned to 1 ms on four
band envelopes): each band's steady offset is the path's coloring (vocoder, speaker, mic); a frame whose bands sit
more than 6 dB off the source once that is out is bad (a vocoder frame with bad FEC, a missing or concealed one, a
click, a dropout), is listed, and stays out of the GAIN, the number to compare between modes. The match percentage
and residual say how tightly the recording follows the source. The per-word median and the gain on the loud frames
(one power mean over all of them) are the coarser views; the gain on the quiet ones says how much noise fills them in, and the spread how far a limiter or
clipping squeezes the peaks. Pure Python and numpy, no radio: test_audio_check.py runs it on synthetic signals."""
import argparse
import json
import sys
from dataclasses import dataclass, field

import numpy as np

from audio_check import db, load_wav

MAINS = (50, 60)
MAINS_HARMONICS = 6         # notched; higher ones are reported as steady noise
HUM_FOUND_DBFS = -75.0      # a mains line louder than this, and HUM_OVER_DB over the noise near it, is notched
HUM_OVER_DB = 10.0
LINE_DB = 10.0              # a steady line: this far over the noise within 25 Hz of it
RADIO_DB = 6.0              # a line this far over the room's is the radio's
NOTCH_HZ = 2.0
MATCH_BANDS = ((300, 600), (600, 1200), (1200, 2400), (2400, 3400))
MATCH_MS = 10.0             # frames compared with the source
MATCH_SEARCH_MS = 40        # the fine lag search, either side of the coarse one, in 1 ms steps
MATCH_SCORED_DB = 15.0      # frames within this of the loud ones are scored
MATCH_BAD_DB = 6.0          # a frame this far from the source (rms over its bands, the steady coloring taken out) is bad
WORD_ODD_DB = 3.0           # a word whose gain is this far from the median is listed: a bad frame, a click, a dropout
FRAME_MS = 20.0
POP_FRAME_MS = 2.0
POP_HZ = 4000               # clicks are measured above this: voice and the radio's 8 kHz audio hardly reach it
TONE_SHARE = 0.6            # a sound with this share of its energy in one line is a tone
SPEECH_DB = 20.0            # speech frames: within this of the loud ones (95th percentile)
CLIP = 0.995


def notches(mains):
    """The frequencies a list of mains frequencies notches: each one's first MAINS_HARMONICS"""
    return [k * m for m in mains for k in range(1, MAINS_HARMONICS + 1)]


def band(x, fs, lo, hi, notch=()):
    """x limited to lo..hi Hz, with each frequency in notch taken out (NOTCH_HZ either side)"""
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / fs)
    X[(f < lo) | (f > hi)] = 0
    for hz in notch:
        X[np.abs(f - hz) < NOTCH_HZ] = 0
    return np.fft.irfft(X, len(x))


def frames_db(y, fs, ms):
    """The level of each ms-long frame, dBFS rms (a full-scale sine is -3)"""
    n = int(fs * ms / 1000)
    m = len(y) // n
    return db(np.sqrt(np.mean(y[:m * n].reshape(m, n) ** 2, axis=1)))


def hum(x, fs):
    """({50: [f, 2f, 3f, 5f] dBFS, 60: [...]}, the mains frequencies found): the strongest line within 1.5 Hz of
    each, over the first 8 s; a mains frequency is found when one of its lines is louder than HUM_FOUND_DBFS and
    HUM_OVER_DB over the noise 3-10 Hz from it (a sound's edges spread energy over the low bins, a line stands out)"""
    n = min(len(x), fs * 8)
    if n < fs // 2:     # bins over 2 Hz: a line can't be told from the noise 3-10 Hz from it
        return {m: [float("nan")] * 4 for m in MAINS}, []
    w = np.hanning(n)
    F = np.abs(np.fft.rfft(x[:n] * w)) * 2 / w.sum()
    f = np.fft.rfftfreq(n, 1 / fs)
    lv = {m: [float(db(F[np.abs(f - k * m) < 1.5].max())) for k in (1, 2, 3, 5)] for m in MAINS}

    def stands_out(hz):
        d = np.abs(f - hz)
        line = db(F[d < 1.5].max())
        return line > HUM_FOUND_DBFS and line > db(np.median(F[(d > 3) & (d < 10)])) + HUM_OVER_DB
    return lv, [m for m in MAINS if any(stands_out(k * m) for k in (1, 2, 3, 5))]


def spectrum(x, fs):
    """(f, dBFS): the median over 1 s stretches (half overlapped) of each 1 Hz bin's level: a steady line holds its
    level, speech and clicks that come and go hardly move it"""
    L = min(len(x), fs)
    w = np.hanning(L)
    segs = [x[i:i + L] for i in range(0, len(x) - L + 1, L // 2)]
    P = np.median([np.abs(np.fft.rfft(s * w)) ** 2 for s in segs], axis=0)
    return np.fft.rfftfreq(L, 1 / fs), db(np.sqrt(P) * 2 / w.sum())


def lines_of(f, F, lo=20.0, hi=None):
    """[(Hz, dBFS, dB over the noise)]: the bins between lo and hi that top their neighbors and stand LINE_DB over the
    median within 25 Hz"""
    hi = hi or f[-1]
    df = f[1] - f[0]
    k25 = max(2, int(25 / df))
    out = []
    for k in np.nonzero((f >= lo) & (f <= hi))[0]:
        if F[k] < F[max(0, k - 1)] or F[k] < F[min(len(F) - 1, k + 1)]:
            continue
        near = np.median(F[max(0, k - k25):k + k25 + 1])
        if F[k] > near + LINE_DB:
            out.append((float(f[k]), float(F[k]), float(F[k] - near)))
    return out


def third_octaves(f, F, lo=31.5, hi=None):
    """[(center Hz, dBFS)]: the median bin level in each third octave, the room's noise for a line it doesn't have"""
    hi = hi or f[-1]
    out = []
    c = lo
    while c * 2 ** (1 / 6) <= hi:
        m = (f >= c * 2 ** (-1 / 6)) & (f < c * 2 ** (1 / 6))
        if m.any():
            out.append((round(c, 1), float(np.median(F[m]))))
        c *= 2 ** (1 / 3)
    return out


def learn_ambient(x, fs, lo=300.0, hi=3400.0):
    """The room's profile from a recording with the radio silent: its steady lines (any frequency), noise per third
    octave and voice-band floor"""
    f, F = spectrum(x, fs)
    env = frames_db(band(x, fs, lo, hi), fs, FRAME_MS)
    return {"rate": fs, "seconds": len(x) / fs, "lo": lo, "hi": hi, "floor": float(np.percentile(env, 10)),
            "lines": [[round(h, 1), round(lv, 1)] for h, lv, _ in lines_of(f, F)],
            "noise": [[c, round(lv, 1)] for c, lv in third_octaves(f, F)]}


def room_level(amb, hz):
    """The room's level at hz: its line there, else its noise in that third octave"""
    near = [lv for h, lv in amb["lines"] if abs(h - hz) <= NOTCH_HZ]
    if near:
        return max(near)
    return min(amb["noise"], key=lambda n: abs(np.log2(n[0] / hz)))[1]


def against_room(x, fs, amb, lo, hi, over, split, min_ms):
    """(the room's lines to notch, the radio's lines [(Hz, dBFS, dB over the room)]), from every frame but the tones
    the recording plays (a tone is a steady line too; speech isn't, and the median spectrum passes over it): a room
    line held within RADIO_DB of its learned level is notched; any line RADIO_DB over the room's level there is the
    radio's. A carrier's hiss or buzz counts as a sound against the silence around it, so it is judged here too."""
    n = int(fs * FRAME_MS / 1000)
    y = band(x, fs, lo, hi)
    env = frames_db(y, fs, FRAME_MS)
    keep = np.ones(len(env), bool)
    for snd in sounds_of(env, y, y, x, fs, float(np.percentile(env, 10)), over, split, min_ms):
        if snd.hz is not None:
            i = int(round(snd.start_s * 1000 / FRAME_MS))
            keep[max(0, i - 2):i + int(round(snd.len_s * 1000 / FRAME_MS)) + 2] = False
    if keep.sum() * FRAME_MS < 500:
        return [h for h, _ in amb["lines"]], []        # nothing but tones: every room line notched, none flagged
    f, F = spectrum(np.concatenate([x[i * n:(i + 1) * n] for i in np.nonzero(keep)[0]]), fs)
    at = lambda hz: float(F[np.abs(f - hz) <= NOTCH_HZ].max())
    notch = [h for h, lv in amb["lines"] if at(h) < lv + RADIO_DB]
    radio = [(h, lv, lv - room_level(amb, h)) for h, lv, _ in lines_of(f, F, 20.0, hi)
             if lv >= room_level(amb, h) + RADIO_DB]
    return notch, sorted(radio, key=lambda r: -r[1])


def tone(s, fs):
    """(pitch, its share of the energy above 250 Hz, THD %, THD+N %, [h2..h5 dBc])"""
    n = len(s)
    P = np.abs(np.fft.rfft(s * np.blackman(n))) ** 2
    f = np.fft.rfftfreq(n, 1 / fs)
    f0 = f[P.argmax()]
    bw = 6 * fs / n
    fund = P[np.abs(f - f0) < bw].sum()
    total = P[f > 250].sum()
    harm = [P[np.abs(f - k * f0) < bw].sum() for k in range(2, 8) if k * f0 < fs / 2]
    return (float(f0), fund / total, 100 * np.sqrt(sum(harm) / fund),
            100 * np.sqrt(max(total - fund, 1e-30) / fund), [float(10 * np.log10(h / fund + 1e-30)) for h in harm[:4]])


@dataclass
class Sound:
    start_s: float
    len_s: float
    rms: float              # dBFS in the band
    peak: float
    hold: tuple             # the frames' highest and lowest level against their median, dB
    clipped: int
    hz: float = None        # a tone: its pitch and distortion
    thd: float = None
    thdn: float = None
    harmonics: list = field(default_factory=list)


@dataclass
class Report:
    path: str
    seconds: float
    hum: dict
    mains: list             # the mains frequencies notched
    floor: float            # dBFS: the band level the quietest frames hold (10th percentile)
    peak: float             # the raw recording's peak
    clipped: int
    sounds: list
    pops: list              # (time s, dB over the surroundings)
    spurs: list             # (Hz, dBFS, dB over the nearby noise): steady lines in the quiet parts
    speech: dict = None     # active (dBFS), seconds, loud (95th percentile frame)
    room: dict = None       # with --ambient: floor (the room's), notched (Hz), radio [(Hz, dBFS, dB over the room)]
    ref: dict = None        # with --ref: start_s, corr, level, ref_level, gain, loud_gain, quiet_gain, spread


def sounds_of(env, y, wide, raw, fs, floor, over, split, min_ms):
    """Each run of frames over the floor by `over` and within `split` dB of the loudest frame near it (300 ms either
    side): a quiet gap between two sounds splits them even when noise fills it"""
    n = int(fs * FRAME_MS / 1000)
    k = max(1, int(300 / FRAME_MS))
    near = np.array([env[max(0, i - k):i + k + 1].max() for i in range(len(env))])
    on = (env > floor + over) & (env > near - split)
    out, i = [], 0
    while i < len(on):
        if not on[i]:
            i += 1
            continue
        j = i
        while j < len(on) and on[j]:
            j += 1
        if (j - i) * FRAME_MS >= min_ms:
            e = 1 if j - i > 4 else 0          # the edges' frames hold part of the rise and fall
            a, b = (i + e) * n, (j - e) * n
            s = y[a:b]
            rms = float(db(np.sqrt(np.mean(s ** 2))))
            sub = env[i + e:j - e]
            med = np.median(sub)
            snd = Sound(i * FRAME_MS / 1000, (j - i) * FRAME_MS / 1000, rms, float(db(np.abs(s).max())),
                        (float(sub.max() - med), float(sub.min() - med)), int((np.abs(raw[a:b]) > CLIP).sum()))
            f0, share, thd, thdn, hs = tone(wide[a:b], fs)
            if share > TONE_SHARE:
                snd.hz, snd.thd, snd.thdn, snd.harmonics = f0, float(thd), float(thdn), hs
            out.append(snd)
        i = j
    return out


def pops_of(x, fs, notch, over_db):
    """Clicks: 2 ms frames above POP_HZ that stand over_db above the median of their 100 ms (and 6 dB over the
    whole recording's), grouped when closer than 10 ms"""
    hp = band(x, fs, POP_HZ, fs / 2 - 100, notch)
    pe = frames_db(hp, fs, POP_FRAME_MS)
    k = int(50 / POP_FRAME_MS)
    loc = np.array([np.median(pe[max(0, i - k):i + k + 1]) for i in range(len(pe))])
    hit = np.nonzero((pe > loc + over_db) & (pe > np.median(pe) + 6))[0]
    if len(hit) == 0:
        return []
    groups = np.split(hit, np.nonzero(np.diff(hit) > 5)[0] + 1)
    return [(float(g[0] * POP_FRAME_MS / 1000), float((pe[g] - loc[g]).max())) for g in groups]


def spurs_of(y, fs, quiet, lo, hi):
    """Lines 12 dB over the noise near them (+-25 bins) in at least 80 % of up to 40 quarter-second stretches of the
    quiet frames: hum harmonics past the notch, a fan, a whine"""
    n = int(fs * FRAME_MS / 1000)
    L = int(fs * 0.25)
    w = np.hanning(L)
    q = quiet[::max(1, len(quiet) // 40)]
    stretches = [y[i * n:i * n + L] for i in q if i * n + L <= len(y)]
    if len(stretches) < 4:
        return []
    F = db(np.array([np.abs(np.fft.rfft(s * w)) * 2 / w.sum() for s in stretches]))
    f = np.fft.rfftfreq(L, 1 / fs)
    out = []
    for k in np.nonzero((f >= lo) & (f <= hi))[0]:
        near = np.median(F[:, max(0, k - 25):k + 25], axis=1)
        top = (F[:, k] > near + 12) & (F[:, k] >= F[:, max(0, k - 1)]) & (F[:, k] >= F[:, min(len(f) - 1, k + 1)])
        if top.mean() >= 0.8:
            out.append((float(f[k]), float(np.median(F[:, k])), float(np.median(F[:, k] - near))))
    return sorted(out, key=lambda s: -s[1])


def power_db(v):
    return float(10 * np.log10(np.mean(10 ** (np.asarray(v) / 10))))


def band_frames(x, fs, ms, hop_ms):
    """[frames, len(MATCH_BANDS) + 1] mean-square levels in dB: each band, then 300-3400 Hz in all; ms-long Hann
    frames every hop_ms"""
    n = int(fs * ms / 1000)
    hop = max(1.0, fs * hop_ms / 1000)     # fractional at 44.1 kHz: frame k starts at k * hop_ms exactly, no drift
    if len(x) < n:
        return np.zeros((0, len(MATCH_BANDS) + 1))
    w = np.hanning(n)
    idx = np.round(np.arange(0, (len(x) - n) / hop + 1) * hop).astype(int)
    idx = idx[idx <= len(x) - n]
    X = np.abs(np.fft.rfft(np.stack([x[i:i + n] for i in idx]) * w, axis=1)) ** 2 * 2 / (n * (w ** 2).sum())
    f = np.fft.rfftfreq(n, 1 / fs)
    cols = [X[:, (f >= lo) & (f < hi)].sum(axis=1) for lo, hi in MATCH_BANDS]
    cols.append(X[:, (f >= MATCH_BANDS[0][0]) & (f < MATCH_BANDS[-1][1])].sum(axis=1))
    return 10 * np.log10(np.stack(cols, axis=1) + 1e-12)


def matched(x, fs, ref, rfs, coarse_s):
    """The recording against the source frame by frame: the lag refined to 1 ms on the band envelopes, each band's
    steady offset (the vocoder's, speaker's and mic's coloring), then every 10 ms frame where the source speaks
    scored by how far its bands sit from the source once that coloring is out. Frames over MATCH_BAD_DB (a vocoder frame
    with bad FEC, a missing or concealed frame, a click, a dropout) are listed and left out of the gain."""
    R = band_frames(ref, rfs, MATCH_MS, MATCH_MS)
    X = band_frames(x, fs, MATCH_MS, 1.0)
    if len(R) == 0 or len(X) == 0:
        return None
    tot = R[:, -1]
    talk = tot > np.percentile(tot, 95) - 25
    step = int(MATCH_MS)
    best = (None, -2.0)
    c0 = int(round(coarse_s * 1000))
    for lag in range(c0 - MATCH_SEARCH_MS, c0 + MATCH_SEARCH_MS + 1):
        k = lag + step * np.arange(len(R))
        if lag < 0 or k[-1] >= len(X):
            continue
        c = np.corrcoef(X[k[talk], :-1].ravel(), R[talk, :-1].ravel())[0, 1]    # every band: sharper than one
        if c > best[1]:
            best = (lag, c)
    lag, corr = best
    if lag is None:
        return None
    Y = X[lag + step * np.arange(len(R))]
    D = Y - R
    loud = talk & (tot > np.percentile(tot[talk], 50))
    color = np.median(D[loud, :-1], axis=0) - np.median(D[loud, -1])   # each band against the whole band's gain
    gain0 = float(np.median(D[loud, -1]))
    use = R[:, :-1] > R[:, -1:] - 20                                     # bands holding the frame's sound
    E = D[:, :-1] - color - gain0
    resid = np.sqrt(np.where(use, E ** 2, 0).sum(axis=1) / np.maximum(use.sum(axis=1), 1))
    scored = tot > np.percentile(tot, 95) - MATCH_SCORED_DB     # the quieter frames' tails are the room's echo
    bad = scored & (resid > MATCH_BAD_DB)
    good = loud & ~bad
    events = []
    hit = np.nonzero(bad)[0]
    if len(hit):
        for g in np.split(hit, np.nonzero(np.diff(hit) > 3)[0] + 1):
            events.append(((lag + step * g[0]) / 1000, (g[-1] - g[0] + 1) * MATCH_MS, float(resid[g].max()),
                           float(np.median(D[g, -1]) - gain0)))
    return {"lag_s": lag / 1000, "corr": float(corr), "frames": int(scored.sum()),
            "match_pct": float(100 * (1 - bad.sum() / max(1, scored.sum()))),
            "resid_db": float(np.median(resid[scored])), "coloring": [float(v) for v in color],
            "gain": (power_db(Y[good, -1]) - power_db(R[good, -1])) if good.any() else None,
            "median_gain": float(np.median(D[good, -1])) if good.any() else None, "bad": events}


def against(env, ref_env):
    """The recording's frames against the source's: the lag that correlates the two envelopes best, then the level
    over the frames where the source speaks, its gain, and that gain on the loud (top quarter) and quiet (bottom 40 %)
    of them"""
    if len(env) < len(ref_env):
        return None
    best = (0, -2.0)
    for lag in range(len(env) - len(ref_env) + 1):
        c = np.corrcoef(env[lag:lag + len(ref_env)], ref_env)[0, 1]
        if c > best[1]:
            best = (lag, c)
    lag, c = best
    seg = env[lag:lag + len(ref_env)]
    talk = ref_env > np.percentile(ref_env, 95) - 25
    loud = talk & (ref_env > np.percentile(ref_env[talk], 75))
    quiet = talk & (ref_env < np.percentile(ref_env[talk], 40))
    gain = lambda m: power_db(seg[m]) - power_db(ref_env[m])
    # Per word (each run of the source's loud frames): a garbled or concealed vocoder frame, a click or a dropout moves
    # one word's gain, not the median
    words = []
    i = 0
    while i < len(loud):
        if loud[i]:
            j = i
            while j < len(loud) and loud[j]:
                j += 1
            if j - i >= 3:
                m = np.zeros(len(loud), bool)
                m[i:j] = True
                words.append(((lag + i) * FRAME_MS / 1000, gain(m)))
            i = j
        else:
            i += 1
    med = float(np.median([g for _, g in words])) if words else None
    odd = [(t, g - med) for t, g in words if abs(g - med) > WORD_ODD_DB] if words else []
    return {"start_s": lag * FRAME_MS / 1000, "corr": float(c), "level": power_db(seg[talk]),
            "ref_level": power_db(ref_env[talk]), "gain": gain(talk), "loud_gain": gain(loud),
            "quiet_gain": gain(quiet), "spread": float(np.std(seg[talk] - ref_env[talk])),
            "word_gain": med, "words": len(words), "odd_words": odd}


def report(x, fs, path="", lo=300.0, hi=3400.0, over=12.0, split=15.0, min_ms=200.0, pop_db=15.0, ref=None,
           ambient=None):
    """The Report of recording x (floats, full scale 1) at fs; ref = (samples, rate) of the source it played;
    ambient = the room's profile (learn_ambient): only its lines are notched, the radio's are listed"""
    h, mains = hum(x, fs)
    if ambient is not None:
        notch, radio = against_room(x, fs, ambient, lo, hi, over, split, min_ms)
        mains = []
    else:
        notch, radio = notches(mains), None
    y = band(x, fs, lo, hi, notch)
    wide = band(x, fs, 250, fs / 2 - 100, notch)
    env = frames_db(y, fs, FRAME_MS)
    floor = float(np.percentile(env, 10))
    r = Report(path, len(x) / fs, h, mains, floor, float(db(np.abs(x).max())), int((np.abs(x) > CLIP).sum()),
               sounds_of(env, y, wide, x, fs, floor, over, split, min_ms), pops_of(x, fs, notch, pop_db),
               spurs_of(y, fs, np.nonzero(env < floor + over)[0], lo, hi))
    if ambient is not None:
        r.room = {"floor": ambient["floor"], "notched": notch, "radio": radio}
    loud = np.percentile(env, 95)
    talk = env[(env > loud - SPEECH_DB) & (env > floor + 6)]
    if len(talk):
        r.speech = {"active": power_db(talk), "seconds": len(talk) * FRAME_MS / 1000, "loud": float(loud)}
    if ref is not None:
        rx, rfs = ref
        r.ref = against(env, frames_db(band(rx, rfs, lo, hi), rfs, FRAME_MS))
        if r.ref:
            r.ref["match"] = matched(band(x, fs, lo, hi, notch), fs, band(rx, rfs, lo, hi), rfs, r.ref["start_s"])
    return r


def text(r):
    out = [f"== {r.path}  {r.seconds:.1f} s",
           "  hum dBFS (f/2f/3f/5f): " + "  ".join(f"{m} Hz " + "/".join(f"{v:.0f}" for v in r.hum[m]) for m in MAINS)
           + ("" if r.room else f"  notched: {r.mains or 'none'}"),
           f"  floor {r.floor:.1f} dBFS in the band, raw peak {r.peak:.1f}, clipped samples {r.clipped}"]
    if r.room:
        out.append(f"  room: floor {r.room['floor']:.1f} dBFS (this floor {r.floor - r.room['floor']:+.1f} dB), notched "
                   + (", ".join(f"{h:.0f}" for h in r.room["notched"]) or "none") + " Hz")
        out.append("  radio's lines (over the room): " + (", ".join(f"{h:.0f} Hz {lv:.0f} dBFS (+{d:.0f})"
                                                              for h, lv, d in r.room["radio"][:12]) or "none"))
    for s in r.sounds:
        line = (f"  {s.start_s:6.2f} s {s.len_s:5.2f} s  rms {s.rms:6.1f}  peak {s.peak:6.1f}  crest {s.peak - s.rms:4.1f}"
                f"  over floor {s.rms - r.floor:4.1f}  hold {s.hold[0]:+.1f}/{s.hold[1]:+.1f}")
        if s.hz is not None:
            line += (f"  tone {s.hz:.0f} Hz THD {s.thd:.1f} % THD+N {s.thdn:.1f} %  h2-h5 dBc "
                     + "/".join(f"{v:.0f}" for v in s.harmonics))
        if s.clipped:
            line += f"  CLIPPED {s.clipped}"
        out.append(line)
    if r.speech:
        out.append(f"  speech: active {r.speech['active']:.1f} dBFS over {r.speech['seconds']:.1f} s,"
                   f" loud frames {r.speech['loud']:.1f}")
    if r.ref:
        g = r.ref
        out.append(f"  against the source: starts {g['start_s']:.2f} s (corr {g['corr']:.2f}), speech {g['level']:.1f} dBFS"
                   f" (source {g['ref_level']:.1f}): gain {g['gain']:+.1f} dB, loud {g['loud_gain']:+.1f},"
                   f" quiet {g['quiet_gain']:+.1f}, spread {g['spread']:.1f}")
        m = g.get("match")
        if m:
            out.append(f"  frame match: lag {m['lag_s']:.3f} s, corr {m['corr']:.3f}, {m['match_pct']:.1f} % of"
                       f" {m['frames']} speech frames within {MATCH_BAD_DB:.0f} dB, median residual {m['resid_db']:.1f} dB,"
                       f" coloring " + "/".join(f"{v:+.1f}" for v in m["coloring"]) + " dB per band")
            if m["gain"] is not None:
                out.append(f"  GAIN (matched loud frames): {m['gain']:+.1f} dB (median {m['median_gain']:+.1f})")
            out.append("  bad frames: " + (", ".join(f"{t:.3f} s {ms:.0f} ms ({d:.0f} dB off, level {lv:+.0f})"
                                                     for t, ms, d, lv in m["bad"][:10]) or "none")
                       + (f" (+{len(m['bad']) - 10} more)" if len(m["bad"]) > 10 else ""))
        if g["word_gain"] is not None:
            out.append(f"  per word: median gain {g['word_gain']:+.1f} dB over {g['words']} words; off by more than"
                       f" {WORD_ODD_DB:.0f} dB: " + (", ".join(f"{t:.2f} s ({d:+.1f})" for t, d in g["odd_words"])
                                                     or "none"))
    out.append(f"  clicks: {len(r.pops)}" + ("  at " + ", ".join(f"{t:.3f} s (+{d:.0f})" for t, d in r.pops[:12])
                                            if r.pops else ""))
    out.append("  steady lines in the quiet: " + (", ".join(f"{f:.0f} Hz {lv:.0f} dBFS (+{o:.0f})"
                                                           for f, lv, o in r.spurs[:8]) or "none"))
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("wav", nargs="+")
    ap.add_argument("--learn-ambient", metavar="OUT.json",
                    help="learn the room from the recording(s), radio silent, and save the profile here")
    ap.add_argument("--ambient", metavar="ROOM.json", help="a learned room profile: notch only its lines")
    ap.add_argument("--ref", help="the source the recording played (the same voice): aligned level and gain")
    ap.add_argument("--ref-s", type=float, help="only the first S seconds of --ref (what was sent)")
    ap.add_argument("--lo", type=float, default=300.0)
    ap.add_argument("--hi", type=float, default=3400.0)
    ap.add_argument("--over", type=float, default=12.0, help="a sound: this many dB over the floor")
    ap.add_argument("--split", type=float, default=15.0, help="a gap this many dB down splits two sounds (speech: 30-40)")
    ap.add_argument("--min-ms", type=float, default=200.0, help="shorter sounds aren't listed")
    ap.add_argument("--pop-db", type=float, default=15.0)
    a = ap.parse_args(argv)
    if a.learn_ambient:
        xs = [load_wav(p) for p in a.wav]
        if len({fs for _, fs in xs}) != 1:
            ap.error("the room recordings differ in rate")
        amb = learn_ambient(np.concatenate([x for x, _ in xs]), xs[0][1], a.lo, a.hi)
        with open(a.learn_ambient, "w") as fp:
            json.dump(amb, fp, indent=1)
        print(f"room: floor {amb['floor']:.1f} dBFS, {len(amb['lines'])} lines: "
              + ", ".join(f"{h:.0f} Hz {lv:.0f}" for h, lv in amb["lines"][:16]))
        return 0
    amb = None
    if a.ambient:
        with open(a.ambient) as fp:
            amb = json.load(fp)
    ref = None
    if a.ref:
        rx, rfs = load_wav(a.ref)
        ref = (rx[:int(a.ref_s * rfs)] if a.ref_s else rx, rfs)
    for p in a.wav:
        x, fs = load_wav(p)
        if amb and amb["rate"] != fs:
            ap.error(f"{p}: {fs} Hz, the room profile {amb['rate']} Hz")
        print(text(report(x, fs, p, a.lo, a.hi, a.over, a.split, a.min_ms, a.pop_db, ref, amb)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
