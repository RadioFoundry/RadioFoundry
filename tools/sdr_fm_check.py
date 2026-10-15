#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""FM transmit checker for an RTL-SDR capture (rtl_sdr raw unsigned-centered int8 I/Q, "CS8").

    tools/sdr_fm_check.py CAPTURE.cs8 --offset -25000 [--center 461887500 --ppm P] [--bw 12.5]
                          [--expect-ctcss 100.0 | --expect-dcs 023N] [--wav voice.wav] [--tones]

The channel sits --offset Hz from the tuner center (an offset keeps it away from the RTL's DC spike). For every
keyed segment (carrier above the capture's noise floor) it reports:
  - level (dBFS, 10 ms power) and length
  - carrier offset from the channel; with --center and --ppm also corrected for the SDR's own frequency error
    (corrected = measured - f_channel x ppm x 1e-6). Both default to the capture's CAPTURE.json (sdrmon.py capture
    records the tuner center and the SDR bench's calibrated ppm there)
  - voice band 300-3000 Hz deviation: peak (99.5th and 99.9th percentile of |dev|) and RMS
  - the 4-7.5 kHz band RMS of the discriminator output, a reference for the noise / residual FM floor
  - sub-audio: a CTCSS tone (60-260 Hz) with its frequency and peak deviation, or a DCS code (134.4 bit/s NRZ,
    23-bit Golay word) with its polarity and deviation; checked against --expect-ctcss / --expect-dcs
  - with --tones: the audio tones in the voice band (a roger beep, a 1750 Hz burst): each run of 40 ms windows whose
    strongest 300-3000 Hz line holds most of the band's power, with its frequency, start and end (s from the start
    of the capture) and peak deviation, as "tone 1750.0 Hz 0.52-2.47 s (1.95 s), deviation 2400 Hz peak"
The exit status is 1 when an expectation fails (or no keyed segment is found), else 0.

Processing is streamed in chunks (0.25 s of I/Q at a time) and only a 16 kHz discriminator output is kept, so
it runs on a small ARM board too. Needs numpy only.

Signal chain: int8 I/Q -> DC removal per chunk (the RTL's spike at the tuner center) -> mix the channel to 0 Hz ->
low-pass and decimate 8 (192 kHz) -> channel low-pass and decimate 4 (48 kHz) -> phase-difference FM discriminator
(Hz) -> low-pass and decimate 3 (16 kHz).

DCS polarity convention: a data 1 is taken as a positive frequency deviation for "N" (normal). A DCS code also
decodes as other codes (rotations and inverted equivalents, for example D023N = D047I); the tool lists every code
the bit stream matches, and an expectation passes when the expected code and polarity is among them.
"""
import argparse
import json
import os
import sys
import wave

import numpy as np

CTCSS = [67.0, 69.3, 71.9, 74.4, 77.0, 79.7, 82.5, 85.4, 88.5, 91.5, 94.8, 97.4, 100.0, 103.5, 107.2, 110.9,
         114.8, 118.8, 123.0, 127.3, 131.8, 136.5, 141.3, 146.2, 151.4, 156.7, 159.8, 162.2, 165.5, 167.9, 171.3,
         173.8, 177.3, 179.9, 183.5, 186.2, 189.9, 192.8, 196.6, 199.5, 203.5, 206.5, 210.7, 218.1, 225.7, 229.1,
         233.6, 241.8, 250.3, 254.1]
DCS = [0o023, 0o025, 0o026, 0o031, 0o032, 0o036, 0o043, 0o047, 0o051, 0o053, 0o054, 0o065, 0o071, 0o072, 0o073,
       0o074, 0o114, 0o115, 0o116, 0o122, 0o125, 0o131, 0o132, 0o134, 0o143, 0o145, 0o152, 0o155, 0o156, 0o162,
       0o165, 0o172, 0o174, 0o205, 0o212, 0o223, 0o225, 0o226, 0o243, 0o244, 0o245, 0o246, 0o251, 0o252, 0o255,
       0o261, 0o263, 0o265, 0o266, 0o271, 0o274, 0o306, 0o311, 0o315, 0o325, 0o331, 0o332, 0o343, 0o346, 0o351,
       0o356, 0o364, 0o365, 0o371, 0o411, 0o412, 0o413, 0o423, 0o431, 0o432, 0o445, 0o446, 0o452, 0o454, 0o455,
       0o462, 0o464, 0o465, 0o466, 0o503, 0o506, 0o516, 0o523, 0o526, 0o532, 0o546, 0o565, 0o606, 0o612, 0o624,
       0o627, 0o631, 0o632, 0o654, 0o662, 0o664, 0o703, 0o712, 0o723, 0o731, 0o732, 0o734, 0o743, 0o754]
DCS_RATE = 134.4

FS_IN = 1536000.0
D1, D2, D3 = 8, 4, 3            # 1.536 MHz -> 192 kHz -> 48 kHz -> 16 kHz
FS_AUDIO = FS_IN / (D1 * D2 * D3)
POWER_WIN_S = 0.01
EDGE_S = 0.05                   # skipped at each end of a keyed segment (key-up / key-down transients)
CTCSS_MIN_DB = 25.0             # a CTCSS line stands this far above the 60-260 Hz median
CTCSS_MIN_DEV_HZ = 60.0         # and deviates at least this much (real tones: 250-750 Hz)
WAV_FULL_SCALE_HZ = 5000.0      # WAV: full scale = 5 kHz deviation, so loudness shows deviation


# --- filters ---

def lowpass(cutoff, fs, ntaps):
    """Windowed-sinc low-pass FIR (Hamming), unity gain at DC."""
    n = np.arange(ntaps) - (ntaps - 1) / 2.0
    h = np.sinc(2.0 * cutoff / fs * n) * np.hamming(ntaps)
    return (h / h.sum()).astype(np.float32)


def bandpass(lo, hi, fs, ntaps):
    """Band-pass FIR as the difference of two low-passes."""
    return (lowpass(hi, fs, ntaps) - lowpass(lo, fs, ntaps)).astype(np.float32)


class Decimator:
    """Streaming FIR low-pass + decimation; keeps the filter history and the decimation phase across chunks."""

    def __init__(self, taps, factor, dtype):
        self.h = taps
        self.d = factor
        self.hist = np.zeros(len(taps) - 1, dtype=dtype)
        self.phase = 0

    def __call__(self, x):
        buf = np.concatenate((self.hist, x))
        self.hist = buf[len(buf) - (len(self.h) - 1):]
        idx = np.arange(self.phase, len(x), self.d)
        self.phase = int((self.phase - len(x)) % self.d)
        if len(idx) == 0:
            return np.zeros(0, dtype=x.dtype)
        # Only the kept outputs: each is a dot product of the taps with a window of the buffer (in blocks, so the
        # window copies stay small)
        view = np.lib.stride_tricks.sliding_window_view(buf, len(self.h))
        hr = self.h[::-1]
        out = np.empty(len(idx), dtype=x.dtype)
        rows = max(1, (1 << 18) // len(self.h))
        for i in range(0, len(idx), rows):
            out[i:i + rows] = view[idx[i:i + rows]] @ hr
        return out


def fir(x, h, block=1 << 15):
    """Zero-phase-delay FIR (centered) by FFT overlap-save in blocks; output has len(x)."""
    m = len(h)
    pad = (m - 1) // 2
    xp = np.concatenate((np.zeros(m - 1 - pad, dtype=np.float32), x.astype(np.float32), np.zeros(pad, np.float32)))
    nfft = 1
    while nfft < block + m - 1:
        nfft <<= 1
    step = nfft - (m - 1)
    hf = np.fft.rfft(h, nfft)
    out = np.empty(len(x), dtype=np.float32)
    pos = 0
    while pos < len(x):
        seg = xp[pos:pos + nfft]
        if len(seg) < nfft:
            seg = np.concatenate((seg, np.zeros(nfft - len(seg), np.float32)))
        y = np.fft.irfft(np.fft.rfft(seg) * hf, nfft)[m - 1:]
        n = min(step, len(x) - pos)
        out[pos:pos + n] = y[:n]
        pos += n
    return out


# --- capture to discriminator output ---

def demodulate(path, offset, bw_khz, chunk):
    """Stream the capture: returns (16 kHz discriminator output in Hz, 10 ms power in dBFS)."""
    cutoff = 10000.0 if bw_khz < 20 else 16000.0     # channel filter: Carson bandwidth plus room for the noise band
    fs1, fs2 = FS_IN / D1, FS_IN / (D1 * D2)
    st1 = Decimator(lowpass(40000.0, FS_IN, 31), D1, np.complex64)
    st2 = Decimator(lowpass(cutoff, fs1, 321), D2, np.complex64)
    st3 = Decimator(lowpass(7000.0, fs2, 95), D3, np.float32)
    win = int(round(fs2 * POWER_WIN_S))
    audio, power = [], []
    last = np.complex64(0)
    pw_rest = np.zeros(0, np.float32)
    k0 = 0
    with open(path, 'rb') as f:
        while True:
            raw = np.fromfile(f, dtype=np.int8, count=2 * chunk)
            n = len(raw) // 2
            if n == 0:
                break
            iq = (raw[0:2 * n:2].astype(np.float32) + 1j * raw[1:2 * n:2].astype(np.float32)) / 128.0
            iq -= iq.mean()                                         # the DC spike at the tuner center
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
            inst = (np.angle(zz[1:] * np.conj(zz[:-1])) * (fs2 / (2.0 * np.pi))).astype(np.float32)
            audio.append(st3(inst))
    if not audio:
        return np.zeros(0, np.float32), np.zeros(0)
    # The three filters delay the output; shift it back so times line up with the power track
    delay = (30 / 2.0) / FS_IN + (320 / 2.0) / fs1 + (94 / 2.0) / fs2
    a = np.concatenate(audio)
    a = a[int(round(delay * FS_AUDIO)):]
    return a, np.concatenate(power)


def find_segments(power_db, threshold_db, min_s):
    floor = float(np.percentile(power_db, 10))
    on = power_db > floor + threshold_db
    # bridge drop-outs shorter than 50 ms
    gap = int(0.05 / POWER_WIN_S)
    segs, i, n = [], 0, len(on)
    while i < n:
        if not on[i]:
            i += 1
            continue
        j = i
        while j < n:
            if on[j]:
                j += 1
            elif np.any(on[j:j + gap]):
                j += 1
            else:
                break
        if (j - i) * POWER_WIN_S >= min_s:
            segs.append((i, j))
        i = j
    return floor, segs


# --- DCS ---

def dcs_word(code):
    """23-bit DCS word: Golay(23,12) parity in bits 22:12, marker 100 in 11:9, the 9-bit code in 8:0."""
    data = 0x800 | (code & 0x1FF)
    rem = data << 11
    for bit in range(22, 10, -1):
        if rem & (1 << bit):
            rem ^= 0xC75 << (bit - 11)
    return (rem << 12) | data


DCS_WORDS = {dcs_word(c): c for c in range(0x200)}


def dcs_name(code, inv):
    return 'D%03o%s' % (code, 'I' if inv else 'N')


def dcs_bits(x, fs):
    """Slice a sub-audio signal into DCS bits: per 1.5 s window, a bit clock fitted to the zero crossings.
    Returns (list of bit-center values per window, list of slicing thresholds)."""
    tb = fs / DCS_RATE
    bits, thrs = [], []
    span = int(1.5 * fs)
    for w0 in range(0, len(x) - int(10 * tb), span):
        w = x[w0:w0 + span]
        # two-level threshold (the NRZ word has a DC part): 1-D two-means
        thr = float(np.mean(w))
        for _ in range(8):
            hi, lo = w[w > thr], w[w <= thr]
            if len(hi) == 0 or len(lo) == 0:
                break
            thr = 0.5 * (float(hi.mean()) + float(lo.mean()))
        s = w - thr
        zc = np.nonzero(np.signbit(s[1:]) != np.signbit(s[:-1]))[0]
        if len(zc) < 8:
            continue
        t = zc + s[zc] / (s[zc] - s[zc + 1])          # fractional crossing times
        n = np.round((t - t[0]) / tb)
        # least squares t = a + b n, refined once without outliers
        for _ in range(2):
            b_, a_ = np.polyfit(n, t, 1)
            keep = np.abs(t - (a_ + b_ * n)) < 0.25 * tb
            if keep.sum() < 8:
                break
            t, n = t[keep], n[keep]
        if not (0.97 * tb < b_ < 1.03 * tb):
            continue
        first = a_ + 0.5 * b_
        first -= np.floor((first - 0) / b_) * b_
        centers = np.arange(first, len(w) - 1, b_)
        v = np.interp(centers, np.arange(len(w)), s)
        bits.append(v)
        thrs.append(thr)
    return bits, thrs


def dcs_decode(x, fs):
    """Count every (code, inverted) the bit stream matches, both bit orders.
    Returns (counts, deviation Hz, the NRZ midpoint relative to x's zero in Hz)."""
    counts = {}
    amps = []
    vals, thrs = dcs_bits(x, fs)
    for v in vals:
        amps.append(np.median(np.abs(v)))
        b = (v > 0).astype(np.int64)
        for i in range(len(b) - 22):
            win = b[i:i + 23]
            for inv in (False, True):
                ww = 1 - win if inv else win
                lsb_first = int(np.dot(ww, 1 << np.arange(23)))
                msb_first = int(np.dot(ww[::-1], 1 << np.arange(23)))
                for order, word in (('lsb', lsb_first), ('msb', msb_first)):
                    code = DCS_WORDS.get(word)
                    if code is not None:
                        key = (code, inv, order)
                        counts[key] = counts.get(key, 0) + 1
    dev = float(np.median(amps)) if amps else 0.0
    return counts, dev, float(np.median(thrs)) if thrs else 0.0


# --- CTCSS ---

def ctcss_find(x, fs):
    """Strongest line 60-260 Hz: (frequency, peak deviation Hz, ratio to the band's median in dB)."""
    n = len(x)
    nfft = 1
    while nfft < max(n, int(10 * fs)):
        nfft <<= 1
    spec = np.abs(np.fft.rfft(x * np.hanning(n), nfft))
    f = np.fft.rfftfreq(nfft, 1.0 / fs)
    band = (f >= 60) & (f <= 260)
    i = int(np.argmax(np.where(band, spec, 0)))
    # parabolic interpolation on the log magnitude
    a, b, c = np.log(spec[i - 1:i + 2] + 1e-12)
    p = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
    freq = f[i] + p * (f[1] - f[0])
    ratio = 20 * np.log10(spec[i] / (np.median(spec[band]) + 1e-12))
    # deviation: the band +-4 Hz around the line, RMS x sqrt(2)
    X = np.fft.rfft(x)
    fx = np.fft.rfftfreq(n, 1.0 / fs)
    X[np.abs(fx - freq) > 4.0] = 0
    tone = np.fft.irfft(X, n)
    return freq, float(np.sqrt(2.0) * np.std(tone)), float(ratio)


# --- audio tones (roger beep, 1750 Hz burst) ---

TONE_WIN_S, TONE_HOP_S = 0.04, 0.01
TONE_PURITY = 0.6               # the line (+-40 Hz) holds this share of the 300-3000 Hz power
TONE_MIN_DEV_HZ = 150.0
TONE_SAME_HZ = 25.0


def tone_runs(x, fs, t_first):
    """Runs of one steady audio tone in a voice band signal x (Hz of deviation, sample 0 at t_first s): a list of
    (frequency Hz, start s, end s, peak deviation Hz)."""
    win, hop = int(TONE_WIN_S * fs), int(TONE_HOP_S * fs)
    nfft = 4096
    f = np.fft.rfftfreq(nfft, 1.0 / fs)
    band = (f >= 300) & (f <= 3000)
    w = np.hanning(win)
    hits = []
    for i in range(0, len(x) - win, hop):
        seg = x[i:i + win]
        spec = np.abs(np.fft.rfft(seg * w, nfft)) ** 2
        j = int(np.argmax(np.where(band, spec, 0)))
        near = band & (np.abs(f - f[j]) <= 40.0)
        purity = spec[near].sum() / (spec[band].sum() + 1e-12)
        dev = float(np.sqrt(2.0) * np.std(seg))
        if purity >= TONE_PURITY and dev >= TONE_MIN_DEV_HZ and 0 < j < len(spec) - 1:
            a, b, c = np.log(spec[j - 1:j + 2] + 1e-12)
            p = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
            hits.append((t_first + (i + win / 2) / fs, f[j] + p * (f[1] - f[0]), dev))
        else:
            hits.append(None)
    runs, cur = [], None
    for h in hits + [None]:
        if h is not None and cur is not None and abs(h[1] - np.median(cur[1])) <= TONE_SAME_HZ:
            cur[0].append(h[0])
            cur[1].append(h[1])
            cur[2].append(h[2])
            continue
        if cur is not None and len(cur[0]) >= 3:
            runs.append((float(np.median(cur[1])), cur[0][0] - TONE_WIN_S / 2, cur[0][-1] + TONE_WIN_S / 2,
                         float(np.median(cur[2]))))
        cur = ([h[0]], [h[1]], [h[2]]) if h is not None else None
    return runs


# --- report ---

def parse_dcs(s):
    s = s.strip().upper().lstrip('D')
    inv = s.endswith('I')
    s = s.rstrip('NI')
    return int(s, 8), inv


def analyze(args):
    audio, power = demodulate(args.capture, args.offset, args.bw, args.chunk)
    if len(power) == 0:
        print('empty capture')
        return 1
    floor, segs = find_segments(power, args.threshold, args.min_seconds)
    chan_hz = args.center + args.offset if args.center else None
    print('%s: %.2f s, noise floor %.1f dBFS (10 ms), %d keyed segment%s' %
          (args.capture, len(power) * POWER_WIN_S, floor, len(segs), '' if len(segs) == 1 else 's'))
    if chan_hz:
        print('channel %.4f MHz (tuner %.4f MHz %+.1f kHz)' % (chan_hz / 1e6, args.center / 1e6, args.offset / 1e3))
    failed = not segs
    voice_bp = bandpass(300.0, 3000.0, FS_AUDIO, 641)
    noise_bp = bandpass(4000.0, 7500.0, FS_AUDIO, 161)
    sub_lp = lowpass(400.0, FS_AUDIO, 255)
    sub_fs = FS_AUDIO / 8
    sub_lp2 = lowpass(300.0, sub_fs, 101)
    wav = np.zeros(len(audio), np.float32) if args.wav else None

    for k, (a, b) in enumerate(segs, 1):
        t0, t1 = a * POWER_WIN_S + EDGE_S, b * POWER_WIN_S - EDGE_S
        s0, s1 = int(t0 * FS_AUDIO), min(int(t1 * FS_AUDIO), len(audio))
        if s1 - s0 < int(0.2 * FS_AUDIO):
            continue
        x = audio[s0:s1]
        carrier = float(np.median(x))
        x = x - carrier
        print('\nsegment %d: %.2f-%.2f s (%.2f s), level %.1f dBFS' %
              (k, a * POWER_WIN_S, b * POWER_WIN_S, (b - a) * POWER_WIN_S, float(np.median(power[a:b]))))
        sub = fir(x, sub_lp)[::8]
        sub = fir(sub, sub_lp2)
        counts, dcs_dev, dcs_mid = dcs_decode(sub, sub_fs)
        words = sum(counts.values())
        nbits = len(sub) / sub_fs * DCS_RATE
        best = max(counts.values()) if counts else 0
        is_dcs = best >= max(3, 0.5 * nbits / 23)
        if is_dcs:
            carrier += dcs_mid       # the DCS word has a DC part: the carrier is the midpoint of the two NRZ levels
        line = '  carrier %+.0f Hz%s' % (carrier, ' (DCS midpoint)' if is_dcs else '')
        if chan_hz and args.ppm:
            corr = chan_hz * args.ppm * 1e-6
            line += ' (SDR %+.2f ppm = %+.0f Hz: corrected %+.0f Hz)' % (args.ppm, corr, carrier - corr)
        print(line)
        voice = fir(x, voice_bp)
        noise = fir(x, noise_bp)
        va = np.abs(voice)
        print('  voice 300-3000 Hz: peak %.0f Hz (99.5%%), %.0f Hz (99.9%%), rms %.0f Hz' %
              (np.percentile(va, 99.5), np.percentile(va, 99.9), np.std(voice)))
        print('  noise 4-7.5 kHz: rms %.0f Hz' % np.std(noise))
        if wav is not None:
            wav[s0:s1] = voice
        if args.track:
            # the carrier every 50 ms over the whole segment: a register that moves the carrier shows as a step
            e0, e1 = int(a * POWER_WIN_S * FS_AUDIO), min(int(b * POWER_WIN_S * FS_AUDIO), len(audio))
            w = int(0.05 * FS_AUDIO)
            vals = [float(np.median(audio[i:i + w])) for i in range(e0, e1 - w + 1, w)]
            print('  track %.2f s, 0.05 s steps: %s' % (e0 / FS_AUDIO, ' '.join('%.0f' % v for v in vals)))
        if args.tone_snr:
            # the tone's level over the whole demodulated band (to 7.5 kHz): the response above the voice band
            blk = int(0.1 * FS_AUDIO)
            n_ = np.arange(blk) / FS_AUDIO
            A = np.stack([np.cos(2 * np.pi * args.tone_snr * n_), np.sin(2 * np.pi * args.tone_snr * n_)], axis=1)
            fits = [A @ np.linalg.lstsq(A, x[i:i + blk].astype(np.float64), rcond=None)[0]
                    for i in range(0, len(x) - blk + 1, blk)]
            if fits:
                print('  tone %.0f Hz: wideband tone rms %.0f Hz' % (args.tone_snr, np.sqrt(np.mean(np.concatenate(fits) ** 2))))
        if args.tone_snr and args.tone_snr <= 3000:
            # a test tone's SNR in the voice band: fit the tone (and its harmonics up to 3 kHz) in 0.1 s blocks,
            # the rest of the 300-3000 Hz signal is noise and distortion
            vb = fir(x, voice_bp)
            blk = int(0.1 * FS_AUDIO)
            tp = npow = 0.0
            for i in range(0, len(vb) - blk + 1, blk):
                seg = vb[i:i + blk].astype(np.float64)
                n_ = np.arange(blk) / FS_AUDIO
                cols = []
                for k in range(1, int(3000 // args.tone_snr) + 1):
                    cols += [np.cos(2 * np.pi * k * args.tone_snr * n_), np.sin(2 * np.pi * k * args.tone_snr * n_)]
                A = np.stack(cols, axis=1)
                c, *_ = np.linalg.lstsq(A, seg, rcond=None)
                fund = A[:, :2] @ c[:2]
                tp += float(np.sum(fund ** 2))
                npow += float(np.sum((seg - fund) ** 2))
            print('  tone %.0f Hz: SINAD %.1f dB (voice band), tone rms %.0f Hz' %
                  (args.tone_snr, 10 * np.log10(tp / max(npow, 1e-9)), np.sqrt(tp / max(len(vb) // blk * blk, 1))))
        if args.levels:
            # the voice band level every 50 ms over the whole segment: an on-off source (a choppy tone) shows here
            # even when it is not a steady tone
            e0, e1 = int(a * POWER_WIN_S * FS_AUDIO), min(int(b * POWER_WIN_S * FS_AUDIO), len(audio))
            xv = fir(audio[e0:e1] - carrier, voice_bp)
            w = int(0.05 * FS_AUDIO)
            vals = [float(np.std(xv[i:i + w])) for i in range(0, len(xv) - w + 1, w)]
            print('  levels %.2f s, 0.05 s steps: %s' % (e0 / FS_AUDIO, ' '.join('%.0f' % v for v in vals)))
        if args.tones:
            # the whole segment, edges included (a roger beep sits in its last 200 ms)
            e0, e1 = int(a * POWER_WIN_S * FS_AUDIO), min(int(b * POWER_WIN_S * FS_AUDIO), len(audio))
            xt = fir(audio[e0:e1] - carrier, voice_bp)
            runs = tone_runs(xt, FS_AUDIO, e0 / FS_AUDIO)
            for hz, r0, r1, dev in runs:
                print('  tone %.1f Hz %.2f-%.2f s (%.2f s), deviation %.0f Hz peak' % (hz, r0, r1, r1 - r0, dev))
            if not runs:
                print('  tones: none')

        freq, tone_dev, ratio = ctcss_find(sub, sub_fs)
        tone_found = ratio >= CTCSS_MIN_DB and tone_dev >= CTCSS_MIN_DEV_HZ
        if is_dcs:
            # every code matched about as often as the best one: the same transmission read as its equivalents
            keys = sorted([k_ for k_, c in counts.items() if c >= 0.5 * best],
                          key=lambda k_: (k_[2] != 'lsb', k_[0] not in DCS, k_[1], -counts[k_]))
            names = ['%s%s' % (dcs_name(c, inv), '' if order == 'lsb' else '(msb)') for c, inv, order in keys]
            print('  sub-audio: DCS %s, deviation +-%.0f Hz, %d word matches in %.0f bits%s' %
                  (names[0], dcs_dev, words, nbits,
                   ('; also decodes as ' + ' '.join(names[1:])) if len(names) > 1 else ''))
        elif tone_found:
            near = min(CTCSS, key=lambda c: abs(c - freq))
            print('  sub-audio: CTCSS %.2f Hz (nearest standard %.1f, %+.2f Hz), deviation %.0f Hz peak, %.0f dB'
                  ' above the 60-260 Hz median' % (freq, near, freq - near, tone_dev, ratio))
        else:
            print('  sub-audio: none (strongest 60-260 Hz line %.1f Hz, %.0f Hz peak, %.0f dB above the median)' %
                  (freq, tone_dev, ratio))

        if args.expect_ctcss is not None:
            ok = (not is_dcs) and tone_found and abs(freq - args.expect_ctcss) <= 0.5
            print('  expect CTCSS %.1f Hz: %s' % (args.expect_ctcss, 'PASS' if ok else 'FAIL'))
            failed |= not ok
        if args.expect_dcs is not None:
            code, inv = parse_dcs(args.expect_dcs)
            got = is_dcs and any(k_[0] == code and k_[1] == inv and counts[k_] >= 0.5 * best for k_ in counts)
            flipped = is_dcs and any(k_[0] == code and k_[1] != inv and counts[k_] >= 0.5 * best for k_ in counts)
            print('  expect DCS %s: %s' % (dcs_name(code, inv), 'PASS' if got else
                                            'FAIL (polarity inverted)' if flipped else 'FAIL'))
            failed |= not got

    if wav is not None:
        pcm = np.clip(wav[::2] / WAV_FULL_SCALE_HZ, -1.0, 1.0)            # 8 kHz
        with wave.open(args.wav, 'wb') as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(int(FS_AUDIO / 2))
            w.writeframes((pcm * 32767).astype('<i2').tobytes())
        print('\nwrote %s (voice band of the keyed segments, 8 kHz, full scale = %.0f Hz deviation)' %
              (args.wav, WAV_FULL_SCALE_HZ))
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('capture', help='rtl_sdr capture: interleaved int8 I/Q (CS8), 1.536 Msps')
    ap.add_argument('--offset', type=float, required=True, help='channel frequency minus tuner center, Hz')
    ap.add_argument('--center', type=float, default=None, help='tuner center frequency, Hz (for --ppm; default: CAPTURE.json)')
    ap.add_argument('--ppm', type=float, default=None, help="the SDR's frequency error, ppm (needs --center; default: CAPTURE.json)")
    ap.add_argument('--bw', type=float, default=12.5, help='channel bandwidth, kHz: 12.5 or 25 (channel filter)')
    ap.add_argument('--threshold', type=float, default=15.0, help='keyed when this many dB above the floor')
    ap.add_argument('--min-seconds', type=float, default=0.3, help='shortest keyed segment reported')
    ap.add_argument('--expect-ctcss', type=float, help='expected CTCSS tone, Hz (e.g. 100.0)')
    ap.add_argument('--expect-dcs', help='expected DCS code and polarity (e.g. 023N, D754I)')
    ap.add_argument('--wav', help='write the voice band (300-3000 Hz) of the keyed segments to this WAV')
    ap.add_argument('--tones', action='store_true', help='report the steady audio tones (roger beep, 1750 Hz burst)')
    ap.add_argument('--tone-snr', type=float, help='report the SINAD of a test tone at this frequency, Hz')
    ap.add_argument('--levels', action='store_true', help='print the voice band rms deviation every 50 ms')
    ap.add_argument('--track', action='store_true', help='print the carrier frequency every 50 ms of each segment')
    ap.add_argument('--chunk', type=int, default=384000, help='I/Q samples per processing chunk')
    args = ap.parse_args()
    meta = {}
    if os.path.exists(args.capture + '.json'):
        with open(args.capture + '.json') as f:
            meta = json.load(f)
    if args.center is None:
        args.center = float(meta.get('center', 0.0))
    if args.ppm is None:
        args.ppm = float(meta.get('ppm', 0.0))
    if args.chunk % (D1 * D2 * D3):
        args.chunk -= args.chunk % (D1 * D2 * D3)
    return analyze(args)


if __name__ == '__main__':
    sys.exit(main())
