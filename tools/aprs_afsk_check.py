#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""APRS (Bell 202 AFSK / AX.25) decoder and transmitter checker for an RTL-SDR capture or a WAV file.

    tools/aprs_afsk_check.py CAPTURE.cs8 --offset -200000 [--bw 25] [--json]
    tools/aprs_afsk_check.py --audio FILE.wav [--json]
    tools/aprs_afsk_check.py --self-test [--wav tests/host/out/aprs_test_44k1.wav]

The capture is rtl_sdr I/Q as sdr_fm_check.py reads it (interleaved int8, 1.536 Msps, the channel --offset Hz from
the tuner center). Every keyed segment is FM demodulated to 48 kHz audio in Hz of deviation, and every AX.25 frame in
it is decoded and checked (FCS). For each frame it reports:
  - the frame: source, destination, digipeaters, information field (TNC2 text), FCS ok
  - TXDELAY: the flags in front of the frame (count and ms), and the time from the carrier coming up to the first
    frame bit
  - deviation: the peak (99.9th percentile of |audio| over the transmission) and the mark (1200 Hz) and space
    (2200 Hz) tone amplitudes, each the median envelope at the centers of bits inside a run of one tone; twist is
    20 log10(mark / space)
  - phase continuity: at every tone change the tone phase is extrapolated from the bit before and the bit after to
    the boundary. A continuous phase generator switching on the symbol clock gives the same difference at every
    boundary once the one switching delay (fitted, reported in us) is taken out; a generator that restarts or jumps
    its phase scatters it. Reported: the RMS and largest residual in degrees and the share above 45 degrees
The exit status is 1 when no frame decodes with a valid FCS, else 0.

Demodulator (written for this repository from the Bell 202 and AX.25 2.2 definitions; no other decoder was read):
audio band 400-4000 Hz -> each tone mixed to 0 Hz and averaged over one bit -> decision mark envelope minus g x
space envelope (several g, for twist) -> a bit clock PLL nudged at each decision change -> NRZI (no change = 1) ->
HDLC flags, bit unstuffing, LSB first octets, CRC-16 (X.25) -> AX.25 addresses. --self-test checks it on frames
built here, on synthesized I/Q (continuous and jumping phase, twist, noise) and, with --wav, on the WAV that the
firmware's own modulator writes (tests/host/aprsTest --wav DIR).

Needs numpy only (it runs on the SDR bench). Uses sdr_fm_check.py's filters (keep the two files together).
"""
import argparse
import json
import os
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sdr_fm_check import Decimator, bandpass, find_segments, fir, lowpass  # noqa: E402

FS_IN = 1536000.0
D1, D2 = 8, 4                   # 1.536 MHz -> 192 kHz -> 48 kHz
FS_AUDIO = FS_IN / (D1 * D2)
POWER_WIN_S = 0.01
BAUD = 1200.0
MARK, SPACE = 1200.0, 2200.0
FLAG = "01111110"
GAINS = (1.0, 0.7, 1.4, 0.5, 2.0, 0.35, 2.8)    # space envelope weights tried (twist); every valid frame is kept


# --- capture to audio ---

def demodulate_iq(path, offset, bw_khz, chunk=384000):
    """Stream the capture: (48 kHz discriminator output in Hz, 10 ms power in dBFS)."""
    cutoff = 7000.0 if bw_khz < 20 else 12500.0
    fs1 = FS_IN / D1
    st1 = Decimator(lowpass(40000.0, FS_IN, 31), D1, np.complex64)
    st2 = Decimator(lowpass(cutoff, fs1, 161), D2, np.complex64)
    win = int(round(FS_AUDIO * POWER_WIN_S))
    audio, power = [], []
    last = np.complex64(0)
    pw_rest = np.zeros(0, np.float32)
    k0 = 0
    chunk -= chunk % (D1 * D2)
    with open(path, "rb") as f:
        while True:
            raw = np.fromfile(f, dtype=np.int8, count=2 * chunk)
            n = len(raw) // 2
            if n == 0:
                break
            iq = (raw[0:2 * n:2].astype(np.float32) + 1j * raw[1:2 * n:2].astype(np.float32)) / 128.0
            iq -= iq.mean()
            ph = (-offset / FS_IN) * (np.arange(n, dtype=np.float64) + k0)
            ph -= np.floor(ph)
            iq *= np.exp((2j * np.pi) * ph.astype(np.float32))
            k0 += n
            z = st2(st1(iq))
            if len(z) == 0:
                continue
            p = np.concatenate((pw_rest, (np.abs(z) ** 2).astype(np.float32)))
            nb = len(p) // win
            power.append(10.0 * np.log10(p[:nb * win].reshape(nb, win).mean(axis=1) + 1e-12))
            pw_rest = p[nb * win:]
            zz = np.concatenate(([last], z))
            last = z[-1]
            audio.append((np.angle(zz[1:] * np.conj(zz[:-1])) * (FS_AUDIO / (2.0 * np.pi))).astype(np.float32))
    if not audio:
        return np.zeros(0, np.float32), np.zeros(0)
    delay = (30 / 2.0) / FS_IN + (160 / 2.0) / fs1          # the two filters' delay: line audio up with the power
    a = np.concatenate(audio)
    return a[int(round(delay * FS_AUDIO)):], np.concatenate(power)


def read_wav(path):
    with wave.open(path, "rb") as w:
        fs, n, ch, sw = w.getframerate(), w.getnframes(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(n)
    if sw != 2:
        raise SystemExit(f"{path}: only 16-bit PCM WAV files")
    x = np.frombuffer(raw, dtype="<i2").astype(np.float32).reshape(-1, ch)[:, 0]
    return x, float(fs)


# --- AX.25 / HDLC ---

def crc_x25(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF


def parse_ax25(frame):
    """(dst, src, digis, control, pid, info) of a frame without its FCS; None if the address field is malformed."""
    addrs, i = [], 0
    while True:
        if i + 7 > len(frame):
            return None
        a = frame[i:i + 7]
        call = "".join(chr(b >> 1) for b in a[:6]).rstrip()
        ssid = (a[6] >> 1) & 0x0F
        addrs.append((call, ssid, bool(a[6] & 0x80)))
        i += 7
        if a[6] & 1:
            break
        if len(addrs) > 10:
            return None
    if len(addrs) < 2 or i + 2 > len(frame):
        return None

    def name(c):
        return c[0] + (f"-{c[1]}" if c[1] else "")
    digis = [name(c) + ("*" if c[2] else "") for c in addrs[2:]]
    return name(addrs[0]), name(addrs[1]), digis, frame[i], frame[i + 1], bytes(frame[i + 2:])


def bits_to_bytes(bits):
    out = bytearray()
    for k in range(0, len(bits) - 7, 8):
        v = 0
        for j in range(8):
            if bits[k + j] == "1":
                v |= 1 << j
        out.append(v)
    return bytes(out)


def unstuff(s):
    """The bits between two flags with stuffing removed; None for an abort (seven 1s) or a stray flag."""
    out, ones = [], 0
    for c in s:
        if c == "1":
            ones += 1
            if ones > 6:
                return None
            out.append(c)
        else:
            if ones == 5:
                ones = 0
                continue        # the stuffed 0
            if ones == 6:
                return None
            ones = 0
            out.append(c)
    return "".join(out)


def hdlc_frames(bits):
    """Frames in a bit string: (first bit index, end bit index, octets incl. FCS, preceding back to back flags)."""
    flags, i = [], bits.find(FLAG)
    while i >= 0:
        flags.append(i)
        i = bits.find(FLAG, i + 1)
    out = []
    for a, b in zip(flags, flags[1:]):
        body = bits[a + 8:b]
        if len(body) < 18 * 8:
            continue
        u = unstuff(body)
        if u is None or len(u) % 8 or len(u) < 18 * 8:
            continue
        octets = bits_to_bytes(u)
        n, k = 1, flags.index(a)
        while k - n >= 0 and flags[k - n] == a - 8 * n:
            n += 1
        out.append((a + 8, b, octets, n))
    return out


# --- demodulator ---

def tone_envelopes(x, fs):
    """|mark| and |space| envelopes: each tone mixed to 0 Hz and averaged over one bit (centered)."""
    t = np.arange(len(x)) / fs
    nbit = int(round(fs / BAUD))
    h = np.ones(nbit, np.float32) / nbit
    env = []
    for f in (MARK, SPACE):
        lo = np.exp(-2j * np.pi * f * t)
        re = fir((x * lo.real).astype(np.float32), h)
        im = fir((x * lo.imag).astype(np.float32), h)
        env.append(2.0 * np.hypot(re, im))
    return env[0], env[1]


def clock_bits(d, fs, gain=0.25):
    """Decision signal -> (tone per bit: True = mark, sample index of each bit center) with a PLL that expects the
    decision to change half way between two samplings."""
    hard = d > 0
    step = BAUD / fs
    phase, prev = 0.0, bool(hard[0])
    tones, idx = [], []
    for i in range(1, len(hard)):
        h = bool(hard[i])
        if h != prev:
            phase += (0.5 - phase) * gain
            prev = h
        phase += step
        if phase >= 1.0:
            phase -= 1.0
            tones.append(h)
            idx.append(i)
    return tones, np.array(idx)


def nrzi(tones):
    return "".join("1" if tones[k] == tones[k - 1] else "0" for k in range(1, len(tones)))


def decode_audio(x, fs):
    """Every valid frame in the audio: dicts with the frame, its bit range, tones and bit centers."""
    m, s = tone_envelopes(x, fs)
    found = {}
    for g in GAINS:
        tones, idx = clock_bits(m - g * s, fs)
        if len(tones) < 200:
            continue
        bits = nrzi(tones)
        for a, b, octets, nflags in hdlc_frames(bits):
            body, fcs = octets[:-2], octets[-2] | (octets[-1] << 8)
            if crc_x25(body) != fcs:
                continue
            p = parse_ax25(body)
            if p is None:
                continue
            t0 = idx[a + 1] / fs            # bit a of the NRZI string is between tones a and a + 1
            key = (round(t0, 2), body)
            if any(abs(k[0] - t0) < 0.05 and k[1] == body for k in found):
                continue
            found[key] = {"t_start": t0, "t_end": idx[min(b + 1, len(idx) - 1)] / fs, "body": body, "parsed": p,
                          "flags_before": nflags, "gain": g, "tones": tones, "idx": idx, "bit_a": a, "bit_b": b}
    return sorted(found.values(), key=lambda r: r["t_start"])


# --- measurements ---

def analytic(x):
    n = len(x)
    X = np.fft.fft(x)
    h = np.zeros(n)
    h[0] = 1
    if n % 2 == 0:
        h[n // 2] = 1
        h[1:n // 2] = 2
    else:
        h[1:(n + 1) // 2] = 2
    return np.fft.ifft(X * h)


def wrap(a):
    return (a + np.pi) % (2 * np.pi) - np.pi


def measure(x, fs, fr):
    """Deviation, tone levels, phase continuity of one decoded frame's transmission (preamble to tail)."""
    tones, idx = fr["tones"], fr["idx"]
    k0 = max(1, fr["bit_a"] - 8 * fr["flags_before"])
    k1 = min(len(tones) - 1, fr["bit_b"] + 9)
    s0, s1 = int(idx[k0]), int(idx[k1])
    seg = x[s0:s1]
    za = analytic(x)
    env, ph = np.abs(za), np.angle(za)
    t = np.arange(len(x)) / fs
    marks, spaces = [], []
    for k in range(k0 + 1, k1 - 1):
        if tones[k - 1] == tones[k] == tones[k + 1]:
            (marks if tones[k] else spaces).append(env[idx[k]])
    out = {"peak_dev_hz": float(np.percentile(np.abs(seg), 99.9)), "max_dev_hz": float(np.max(np.abs(seg))),
           "rms_dev_hz": float(np.std(seg))}
    if marks and spaces:
        am, asp = float(np.median(marks)), float(np.median(spaces))
        out.update(mark_dev_hz=am, space_dev_hz=asp, twist_db=20 * np.log10(am / asp))
    # phase at each tone change: offsets of the tone phase (relative to the nominal tone) over the central half of
    # the bits either side, extrapolated to the boundary half way between the two bit centers
    T = fs / BAUD
    w = int(T / 4)
    deltas, dfs = [], []
    for k in range(k0 + 1, k1):
        if tones[k] == tones[k - 1]:
            continue
        fa, fb = (MARK if tones[k - 1] else SPACE), (MARK if tones[k] else SPACE)
        ca, cb = int(idx[k - 1]), int(idx[k])
        if cb - ca < T / 2 or cb - ca > 1.5 * T:
            continue
        b = 0.5 * (ca + cb) / fs
        ra = np.arange(ca - w, ca + w + 1)
        rb = np.arange(cb - w, cb + w + 1)
        oa = np.angle(np.mean(np.exp(1j * (ph[ra] - 2 * np.pi * fa * t[ra]))))
        ob = np.angle(np.mean(np.exp(1j * (ph[rb] - 2 * np.pi * fb * t[rb]))))
        deltas.append(wrap((ob + 2 * np.pi * fb * b) - (oa + 2 * np.pi * fa * b)))
        dfs.append(fa - fb)
    if len(deltas) >= 8:
        deltas, dfs = np.array(deltas), np.array(dfs)
        grid = np.linspace(-0.5 / BAUD, 0.5 / BAUD, 401)
        cost = [np.mean(wrap(deltas - 2 * np.pi * dfs * d) ** 2) for d in grid]
        d = grid[int(np.argmin(cost))]
        res = np.degrees(np.abs(wrap(deltas - 2 * np.pi * dfs * d)))
        out.update(phase_transitions=int(len(res)), phase_switch_us=float(d * 1e6),
                   phase_rms_deg=float(np.sqrt(np.mean(res ** 2))), phase_max_deg=float(np.max(res)),
                   phase_over45=float(np.mean(res > 45)))
    return out


def tnc2(p):
    dst, src, digis, _c, _pid, info = p
    return f"{src}>{dst}{''.join(',' + d for d in digis)}:{info.decode('latin-1')}"


def report_frames(x, fs, frames, seg_t0, key_t0=None):
    out = []
    for fr in frames:
        dst, src, digis, ctl, pid, info = fr["parsed"]
        r = {"tnc2": tnc2(fr["parsed"]), "src": src, "dst": dst, "digis": digis, "control": ctl, "pid": pid,
             "info": info.decode("latin-1"), "fcs_ok": True, "t_start_s": round(seg_t0 + fr["t_start"], 4),
             "t_end_s": round(seg_t0 + fr["t_end"], 4), "txdelay_flags": fr["flags_before"],
             "txdelay_ms": round(fr["flags_before"] * 8 / BAUD * 1000, 1), "decision_gain": fr["gain"]}
        if key_t0 is not None:
            r["keyup_to_data_ms"] = round((seg_t0 + fr["t_start"] - key_t0) * 1000, 1)
        r.update({k: (round(v, 3) if isinstance(v, float) else v) for k, v in measure(x, fs, fr).items()})
        out.append(r)
    return out


def analyze_iq(path, offset, bw_khz, threshold=12.0):
    audio, power = demodulate_iq(path, offset, bw_khz)
    res = {"capture": path, "seconds": round(len(power) * POWER_WIN_S, 2), "segments": [], "frames": []}
    if len(power) == 0:
        return res
    floor, segs = find_segments(power, threshold, 0.1)
    res["floor_dbfs"] = round(float(floor), 1)
    for a, b in segs:
        t0, t1 = a * POWER_WIN_S, b * POWER_WIN_S
        s0, s1 = int(t0 * FS_AUDIO), min(int(t1 * FS_AUDIO), len(audio))
        x = audio[s0:s1]
        carrier = float(np.median(x))
        x = fir(x - carrier, bandpass(400.0, 4000.0, FS_AUDIO, 241))
        res["segments"].append({"t_start_s": round(t0, 3), "t_end_s": round(t1, 3), "carrier_hz": round(carrier),
                                "level_dbfs": round(float(np.median(power[a:b])), 1)})
        res["frames"] += report_frames(x, FS_AUDIO, decode_audio(x, FS_AUDIO), t0, key_t0=t0)
    return res


def analyze_audio(path):
    x, fs = read_wav(path)
    x = fir(x - float(np.mean(x)), bandpass(400.0, 4000.0, fs, int(fs / 200) | 1))
    return {"audio": path, "fs": fs, "seconds": round(len(x) / fs, 2), "frames":
            report_frames(x, fs, decode_audio(x, fs), 0.0)}


def print_text(res):
    if "capture" in res:
        print(f"{res['capture']}: {res['seconds']} s, floor {res.get('floor_dbfs')} dBFS, "
              f"{len(res['segments'])} keyed segment(s)")
        for s in res["segments"]:
            print(f"  segment {s['t_start_s']:.2f}-{s['t_end_s']:.2f} s, carrier {s['carrier_hz']:+d} Hz, "
                  f"level {s['level_dbfs']} dBFS")
    print(f"{len(res['frames'])} frame(s) with a valid FCS")
    for f in res["frames"]:
        print(f"  {f['t_start_s']:.3f} s  {f['tnc2']}")
        line = (f"    TXDELAY {f['txdelay_flags']} flags ({f['txdelay_ms']} ms)"
                + (f", carrier up to first frame bit {f['keyup_to_data_ms']} ms" if "keyup_to_data_ms" in f else ""))
        print(line)
        if "mark_dev_hz" in f:
            print(f"    deviation peak {f['peak_dev_hz']:.0f} Hz (99.9 %), mark {f['mark_dev_hz']:.0f} Hz, space "
                  f"{f['space_dev_hz']:.0f} Hz, twist {f['twist_db']:+.1f} dB")
        if "phase_rms_deg" in f:
            print(f"    phase at {f['phase_transitions']} tone changes: residual rms {f['phase_rms_deg']:.1f} deg, max "
                  f"{f['phase_max_deg']:.1f} deg, {100 * f['phase_over45']:.0f} % over 45 deg (switch "
                  f"{f['phase_switch_us']:+.0f} us from the bit clock)")


# --- self-test: frames, AFSK and I/Q built here ---

def ax25_frame(src, dst, digis, info):
    def addr(c, last, cbit):
        call, _, ssid = c.partition("-")
        b = bytes((ord(ch) << 1) for ch in call.ljust(6)[:6])
        return b + bytes([0x60 | (int(ssid or 0) << 1) | (0x80 if cbit else 0) | (1 if last else 0)])
    parts = [addr(dst, False, True), addr(src, not digis, False)]
    parts += [addr(d, i == len(digis) - 1, False) for i, d in enumerate(digis)]
    body = b"".join(parts) + b"\x03\xf0" + info.encode("latin-1")
    fcs = crc_x25(body)
    return body + bytes([fcs & 0xFF, fcs >> 8])


def hdlc_bits(frames, nflags, tail=3):
    bits = FLAG * nflags
    for f in frames:
        ones, s = 0, []
        for b in f:
            for j in range(8):
                c = "1" if b >> j & 1 else "0"
                s.append(c)
                ones = ones + 1 if c == "1" else 0
                if ones == 5:
                    s.append("0")
                    ones = 0
        bits += "".join(s) + FLAG
    return bits + FLAG * (tail - 1)


def afsk_audio(bits, fs, dev_mark, dev_space, phase_jumps=False, seed=1):
    """NRZI tones (a 0 changes the tone), continuous phase unless phase_jumps; in Hz of deviation."""
    rng = np.random.default_rng(seed)
    n = int(len(bits) * fs / BAUD) + 1
    tone = np.empty(n, bool)
    cur, k_prev = True, -1
    jumps = np.zeros(n)
    for i in range(n):
        k = min(int(i * BAUD / fs), len(bits) - 1)
        if k != k_prev:
            if bits[k] == "0":
                cur = not cur
                if phase_jumps:
                    jumps[i] = rng.uniform(-np.pi, np.pi)
            k_prev = k
        tone[i] = cur
    f = np.where(tone, MARK, SPACE)
    ph = np.cumsum(2 * np.pi * f / fs) + np.cumsum(jumps)
    return np.where(tone, dev_mark, dev_space) * np.sin(ph)


def synth_iq(path, audio_hz, fs_audio, offset, snr_db=25.0, lead_s=0.3, seed=2):
    """FM modulate audio (Hz of deviation) onto a carrier offset Hz from the tuner center; int8 I/Q at 1.536 Msps
    with silence (noise only) before and after the keyed part."""
    rng = np.random.default_rng(seed)
    up = np.interp(np.arange(int(len(audio_hz) * FS_IN / fs_audio)) / FS_IN, np.arange(len(audio_hz)) / fs_audio,
                   audio_hz)
    lead = int(lead_s * FS_IN)
    f = np.concatenate((np.zeros(lead), up, np.zeros(lead))) + offset
    ph = np.cumsum(2 * np.pi * f / FS_IN)
    amp = np.concatenate((np.zeros(lead), np.ones(len(up)), np.zeros(lead)))
    sig = amp * np.exp(1j * ph) * 0.5
    noise = (rng.standard_normal(len(sig)) + 1j * rng.standard_normal(len(sig))) * 0.5 / np.sqrt(2) * \
        10 ** (-snr_db / 20) * np.sqrt(FS_IN / 25000.0)
    z = sig + noise
    iq = np.empty(2 * len(z), np.int8)
    iq[0::2] = np.clip(np.round(z.real * 127), -127, 127)
    iq[1::2] = np.clip(np.round(z.imag * 127), -127, 127)
    iq.tofile(path)


def self_test(wav=None):
    import tempfile
    fails = []

    def check(cond, what):
        print(("ok    " if cond else "FAIL  ") + what)
        if not cond:
            fails.append(what)

    lines = [("N0TST-15", "APZODR", [], "!/5L!!<*e7>7P[ODR self test"),
             ("N0TST-15", "APZODR", ["WIDE1-1"], ">status text"),
             ("N0TST-15", "APZODR", [], ":K1ABC-9  :hello{42")]
    frames = [ax25_frame(*ln) for ln in lines]
    want = [f"{s}>{d}{''.join(',' + x for x in dg)}:{i}" for s, d, dg, i in lines]
    for f, ln in zip(frames, lines):
        p = parse_ax25(f[:-2])
        check(p is not None and tnc2(p) == want[lines.index(ln)], f"frame round trip {want[lines.index(ln)]}")
    tmp = tempfile.mkdtemp(prefix="aprs_afsk_")
    offset = -200000.0
    cases = [("continuous, flat", 3000, 3000, False, 0.0),
             ("continuous, space -4 dB", 3000, 3000 * 10 ** (-4 / 20), False, 0.0),
             ("phase jumps", 3000, 3000, True, 0.0)]
    for name, dm, ds, jumps, _ in cases:
        bits = hdlc_bits(frames[:1], 45)
        a = afsk_audio(bits, 48000.0, dm, ds, phase_jumps=jumps)
        path = os.path.join(tmp, "synth.cs8")
        synth_iq(path, a, 48000.0, offset)
        res = analyze_iq(path, offset, 25.0)
        got = [f["tnc2"] for f in res["frames"]]
        if jumps:
            check(want[0] in got or not got, f"{name}: decodes or not, no wrong frame ({got})")
            if got:
                f = res["frames"][0]
                check(f.get("phase_rms_deg", 0) > 40, f"{name}: phase residual rms {f.get('phase_rms_deg')} deg "
                      "> 40 (discontinuities seen)")
            continue
        check(got == [want[0]], f"{name}: I/Q decodes {got}")
        if not got:
            continue
        f = res["frames"][0]
        tw = 20 * np.log10(dm / ds)
        check(abs(f["mark_dev_hz"] - dm) < 0.08 * dm, f"{name}: mark deviation {f['mark_dev_hz']:.0f} Hz ~ {dm}")
        check(abs(f["twist_db"] - tw) < 0.7, f"{name}: twist {f['twist_db']:+.2f} dB ~ {tw:+.1f}")
        check(2500 < f["peak_dev_hz"] < 3600, f"{name}: peak deviation {f['peak_dev_hz']:.0f} Hz")
        check(f.get("phase_rms_deg", 99) < 15, f"{name}: phase residual rms {f.get('phase_rms_deg')} deg < 15")
        check(f["txdelay_flags"] in (44, 45), f"{name}: TXDELAY {f['txdelay_flags']} flags (45 sent)")
        check(250 < f["keyup_to_data_ms"] < 400, f"{name}: carrier to data {f['keyup_to_data_ms']} ms")
    # three frames back to back, audio input at 44.1 kHz
    bits = hdlc_bits(frames, 30)
    a = afsk_audio(bits, 44100.0, 1.0, 1.0)
    p = os.path.join(tmp, "synth.wav")
    with wave.open(p, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes((a * 12000).astype("<i2").tobytes())
    got = [f["tnc2"] for f in analyze_audio(p)["frames"]]
    check(got == want, f"three frames in a WAV: {got}")
    if wav:
        # the firmware's modulator (aprsTest --wav): as audio, and FM modulated into I/Q
        r = analyze_audio(wav)
        got = [f["tnc2"] for f in r["frames"]]
        check(len(got) == 3 and all(g.startswith("N0CALL-7>APZODR") for g in got), f"firmware WAV: {got}")
        x, fs = read_wav(wav)
        path = os.path.join(tmp, "fw.cs8")
        synth_iq(path, x / np.max(np.abs(x)) * 3000.0, fs, offset)
        res = analyze_iq(path, offset, 25.0)
        got2 = [f["tnc2"] for f in res["frames"]]
        check(got2 == got, f"firmware WAV as I/Q: {len(got2)} frames")
        for f in res["frames"]:
            check(f.get("phase_rms_deg", 99) < 15 and abs(f.get("twist_db", 99)) < 1.0,
                  f"firmware tones: phase rms {f.get('phase_rms_deg')} deg, twist {f.get('twist_db')} dB")
    print(f"aprs_afsk_check self-test: {'FAILED' if fails else 'PASSED'}")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("capture", nargs="?", help="rtl_sdr capture: interleaved int8 I/Q (CS8), 1.536 Msps")
    ap.add_argument("--offset", type=float, default=-200000.0, help="channel frequency minus tuner center, Hz")
    ap.add_argument("--bw", type=float, default=25.0, help="channel bandwidth, kHz: 12.5 or 25 (channel filter)")
    ap.add_argument("--threshold", type=float, default=12.0, help="keyed when this many dB above the floor")
    ap.add_argument("--audio", help="decode a 16-bit WAV instead of a capture")
    ap.add_argument("--json", action="store_true", help="print the result as JSON")
    ap.add_argument("--self-test", action="store_true", help="check the decoder on frames built here")
    ap.add_argument("--wav", help="--self-test: also the firmware's WAV (tests/host/aprsTest --wav DIR)")
    a = ap.parse_args()
    if a.self_test:
        return self_test(a.wav)
    if a.audio:
        res = analyze_audio(a.audio)
    elif a.capture:
        res = analyze_iq(a.capture, a.offset, a.bw, a.threshold)
    else:
        ap.error("a capture, --audio or --self-test")
    if a.json:
        print(json.dumps(res))
    else:
        print_text(res)
    return 0 if res["frames"] else 1


if __name__ == "__main__":
    sys.exit(main())
