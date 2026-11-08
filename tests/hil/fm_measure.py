# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""A bench board's FM receive job (the line protocol's rx op, mode fm) read as one report of what the radio sent, no
radio needed: test_fm_measure.py runs it on made-up records. SdrBench.fm_over_result() builds the report; the FM tests
assert on it.

The board demodulates: its block records give the keyed segments, its summary the carrier, deviation and sub-audio,
its digit records the DTMF. Only the timed audio figures come from the audio stream on this side (the board's
filtered audio, about 16 kS/s of demodulated 300-3000 Hz audio, never I/Q): the steady tone runs and the voice band's
peak and rms, each per segment, with the voice band's rms every 50 ms.

Times are seconds from the job's first sample (the blocks' `t`).
"""
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if os.path.join(ROOT, "tools") not in sys.path:
    sys.path.insert(0, os.path.join(ROOT, "tools"))

AUDIO_DELAY_S = 0.01     # the board's squelch gate looks this far ahead, so its audio stream lags its blocks by this
EDGE_S = 0.05            # left out of the voice figures at each end of a segment: the key-up and key-down transients
MIN_SEGMENT_S = 0.3      # a shorter run of carrier blocks isn't a segment
BRIDGE_S = 0.05          # a drop-out shorter than this doesn't end a segment
SAME_TONE_HZ = 15.0      # two runs this close in pitch can be one tone
LEVEL_STEP_S = 0.05


def number(v):
    """A summary figure as a number: some summaries give {"mean", "min", "max"}, others a plain number."""
    if isinstance(v, dict):
        v = v.get("mean")
    return None if v is None else float(v)


def blocks_of(records):
    return [r["block"] for r in records if isinstance(r, dict) and isinstance(r.get("block"), dict)]


def segments_of(blocks):
    """The runs of carrier blocks as [(start s, end s, blocks)], drop-outs under BRIDGE_S bridged."""
    if not blocks:
        return []
    step = min((b["t"] - a["t"] for a, b in zip(blocks, blocks[1:]) if b["t"] > a["t"]), default=0.1)
    runs, cur, last = [], None, None
    for i, b in enumerate(blocks):
        if not b.get("carrier"):
            continue
        if cur is None or b["t"] - blocks[last]["t"] > step + BRIDGE_S:
            cur = [i, i]
            runs.append(cur)
        else:
            cur[1] = i
        last = i
    out = []
    for i, j in runs:
        t0, t1 = blocks[i]["t"], blocks[j]["t"] + step
        if t1 - t0 >= MIN_SEGMENT_S:
            out.append((round(t0, 3), round(t1, 3), blocks[i:j + 1]))
    return out


def merge_tone_runs(runs):
    """tone_runs()'s runs, in time order, with the pieces of one tone joined: on a weak signal a click breaks a steady
    tone into runs that touch or overlap. A pause longer than BRIDGE_S, or another pitch, stays a new run."""
    out = []
    for f, a, b, d in runs:
        if out and abs(out[-1][0] - f) <= SAME_TONE_HZ and a - out[-1][2] <= BRIDGE_S:
            f0, a0, b0, d0 = out[-1]
            n0, n = b0 - a0, b - a
            out[-1] = ((f0 * n0 + f * n) / (n0 + n), a0, max(b0, b), (d0 * n0 + d * n) / (n0 + n))
        else:
            out.append((f, a, b, d))
    return out


def _voice(x, fs, seg):
    """One segment's voice band figures from its audio x (Hz of deviation, sample 0 at t0 of the stream)."""
    import numpy as np
    from sdr_fm_check import tone_runs
    t0, t1 = seg
    e0, e1 = int((t0 + AUDIO_DELAY_S) * fs), min(int((t1 + AUDIO_DELAY_S) * fs), len(x))
    out = {"start": t0, "len": round(t1 - t0, 3), "tones": []}
    if e1 - e0 < int(0.2 * fs):
        return out
    whole = x[e0:e1]
    # tones and levels over the whole segment, edges included (a roger beep sits in its last 200 ms)
    runs = merge_tone_runs(tone_runs(whole, fs, t0))
    out["tones"] = [(round(f, 1), round(a, 3), round(b, 3), round(d)) for f, a, b, d in runs]
    w = int(LEVEL_STEP_S * fs)
    out["levels"] = [float(np.std(whole[i:i + w])) for i in range(0, len(whole) - w + 1, w)]
    v = np.asarray(x[int((t0 + EDGE_S + AUDIO_DELAY_S) * fs):int((t1 - EDGE_S + AUDIO_DELAY_S) * fs)], float)
    if len(v):
        out["peak"], out["peak999"] = round(float(np.percentile(np.abs(v), 99.5))), \
            round(float(np.percentile(np.abs(v), 99.9)))
        out["rms"] = round(float(np.std(v)))
    return out


def measure(done, records, header=None, audio=None):
    """The report of one job: its done record, its records, its header and the audio stream's int16 LE bytes (None: no
    tones, voice figures or levels). Keys: segments, spans [(t0, t1)], levels [dBFS, one per segment], carrier_hz (the
    board's mean offset, its crystal taken out), freq_tol_hz, deviation, ctcss_hz / ctcss_dev_hz or dcs / dcs_codes /
    dcs_dev_hz / dcs_turnoff (a bool: the board heard the 134.4 Hz turn-off tone for an eighth of a second), sub (the
    board's as it is), track [(t, Hz): the carrier every 50 ms of the first segment], tone (the strongest audio tone:
    hz, dev_hz, sinad_db, thd_pct), peaks, dtmf [(digit, t0, t1, deviation, twist on the carrier, twist after a
    receiver's de-emphasis)], tones [(Hz, t0, t1, deviation)], voice
    (per segment: start, len, peak, peak999, rms, tones, levels)."""
    done = done or {}
    summary = done.get("summary") or {}
    header = header or {}
    blocks = blocks_of(records)
    segs = segments_of(blocks)
    r = {"segments": len(segs), "spans": [(a, b) for a, b, _ in segs], "levels": [], "seconds": done.get("seconds"),
         "carrier_hz": None, "freq_tol_hz": header.get("freq_tol_hz"), "ppm_source": header.get("ppm_source"),
         "deviation": summary.get("deviation") or {}, "sub": summary.get("sub") or {}, "dtmf": [], "tones": [],
         "voice": [], "tone": summary.get("tone"), "peaks": summary.get("peaks") or [],
         "carrier_s": summary.get("carrier_s", 0), "power_dbfs": number(summary.get("power_dbfs")),
         "noise_dbfs": number(summary.get("noise_dbfs"))}
    audio_part = summary.get("audio") or {}
    if isinstance(audio_part, dict):
        r["tone"] = r["tone"] or audio_part.get("tone")
        r["peaks"] = r["peaks"] or audio_part.get("peaks") or []
    for _, _, seg in segs:
        pw = sorted(b["power_dbfs"] for b in seg if b.get("power_dbfs") is not None)
        r["levels"].append(round(pw[len(pw) // 2], 1) if pw else 0.0)
    cfo = number(summary.get("cfo_hz"))
    if cfo is None and segs:
        vals = [b["cfo_hz"] for b in segs[0][2] if b.get("cfo_hz") is not None]
        cfo = sum(vals) / len(vals) if vals else None
    if cfo is not None:
        r["carrier_hz"] = round(cfo)

    sub = r["sub"]
    if sub.get("kind") == "ctcss":
        r["ctcss_hz"], r["ctcss_dev_hz"] = sub.get("hz"), round(sub.get("dev_hz") or 0)
    elif sub.get("kind") == "dcs":
        aliases = sub.get("aliases") or []     # a string of codes, or a list
        aliases = aliases.replace(",", " ").split() if isinstance(aliases, str) else list(aliases)
        r["dcs"] = sub.get("code")
        r["dcs_codes"] = [c.upper() for c in [sub.get("code") or ""] + aliases]
        r["dcs_dev_hz"] = round(sub.get("dev_hz") or 0)
        r["dcs_turnoff"] = bool(sub.get("turnoff"))

    digits = [rec["dtmf"] for rec in records if isinstance(rec, dict) and isinstance(rec.get("dtmf"), dict)]
    if digits:
        from sdr_fm_check import received_twist
        for d in digits:
            twist = float(d.get("twist_db", 0.0))
            r["dtmf"].append((d["digit"], round(float(d["t0"]), 3), round(float(d["t1"]), 3),
                              round(float(d.get("dev_hz", 0))), twist,
                              round(float(received_twist(twist, d["digit"])), 1)))

    # The carrier every 50 ms of the first segment: a slow change of it shows here
    r["track"] = []
    if segs:
        t0, bins = segs[0][0], {}
        for b in segs[0][2]:
            if b.get("cfo_hz") is not None:
                bins.setdefault(int((b["t"] - t0) / LEVEL_STEP_S), []).append(b["cfo_hz"])
        r["track"] = [(round(t0 + k * LEVEL_STEP_S, 2), round(sum(v) / len(v))) for k, v in sorted(bins.items())]

    if audio is not None and segs:
        import numpy as np
        rate, full = 16000.0, 5000.0
        for sd in header.get("streams") or []:
            if sd.get("name") == "audio":
                rate, full = float(sd.get("rate", rate)), float(sd.get("full", full))
        x = np.frombuffer(bytes(audio), dtype="<i2").astype(float) * full / 32767.0
        for t0, t1, _ in segs:
            v = _voice(x, rate, (t0, t1))
            r["voice"].append(v)
            r["tones"].extend(v["tones"])
    return r


def describe(r):
    """The report as text for a test's log."""
    lines = [f"{r['segments']} keyed segment{'' if r['segments'] == 1 else 's'} {r['spans']}, levels {r['levels']} dBFS, "
             f"carrier {r['carrier_hz']} Hz (tolerance {r.get('freq_tol_hz')}, {r.get('ppm_source')}), "
             f"deviation {r['deviation']}, sub {r['sub']}"]
    if r.get("tone"):
        lines.append(f"strongest audio tone {r['tone']}, peaks {r['peaks']}")
    for v in r["voice"]:
        if "peak" in v:
            lines.append(f"segment {v['start']:.2f} s, {v['len']:.2f} s: voice peak {v['peak']} Hz (99.5 %), "
                         f"{v['peak999']} Hz (99.9 %), rms {v['rms']} Hz")
    for t in r["tones"]:
        lines.append(f"tone {t[0]} Hz {t[1]:.2f}-{t[2]:.2f} s, deviation {t[3]} Hz")
    for d in r["dtmf"]:
        lines.append(f"dtmf {d[0]} {d[1]:.3f}-{d[2]:.3f} s, deviation {d[3]} Hz, twist {d[4]:+.1f} dB "
                     f"(received {d[5]:+.1f} dB)")
    return "\n".join(lines)
