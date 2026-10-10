#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""
Adapted from our earlier bench tool sdrmon.py.

sdrmon: on-air DMR burst monitor (one channel, offline analysis of a raw IQ capture from the SDR bench's RTL-SDR).
Runs on the SDR bench with the odr_sdrbench package (its Channelizer, FmDemod, slicer and FEC).

    sdrmon.py capture --seconds N --out FILE [--freq 461.6875] [--offset-khz -200] [--gain 15] [--ppm 0.7]
                      [--service odr-sdrbench@<bench host> | --service '']
        Stops the bench's gateway service (it holds the dongle) unless --service '', records raw IQ (cs8, 1.536 MS/s, bias tee
        on) tuned --offset-khz away from --freq (keeps the channel off the RTL DC spike at the tuner center), then
        restarts the service, always (finally + SIGTERM/SIGHUP/SIGINT handlers). Prints "READY <wall clock of sample
        0>" once samples flow: wait 2 s more after READY before keying, or the first 0.5-1 s is missing.
        Ctrl-C / SIGTERM ends the capture early and still restarts the service.
        Writes FILE plus FILE.json (center, rate, gain, ppm, wall clock of sample 0, overflows, ADC clipping: the
        share of I/Q samples at the int8 rails). Clipping above 1 % is reported as a warning: lower --gain, move the
        antenna, or turn the radio's power down (see docs/testing/dmr-bench.md).
        The ppm is applied in software at analysis time (librtlsdr only takes whole ppm).

    sdrmon.py analyze FILE [--cc 2] [--from MS] [--to MS] [--edges-only] [--dbm-offset DB] [--json OUT]
        Channelizes and FM-demodulates (48 kS/s, 10 samples per symbol), searches every sample for all four sync
        families (BS / MS / TS1 / TS2, voice and data), follows voice bursts B-F by their EMB, and prints per burst:
        time (ms from sample 0), slot phase, family, sync bit errors, color code / data type, power (dBFS and a
        rough dBm), power spread inside the burst (a second transmitter shows up as a step), the frequency offset,
        and the LC where there is one (voice LC header / terminator: FLCO, source, destination; voice E: the
        embedded LC assembled from B-E); then the carrier key-up / key-down edges and ramps from the 0.1 ms power
        envelope, and a summary. --json OUT also writes all of it as JSON (dmr_bench.py checks pass criteria on
        it). The capture is read in 1 M-sample chunks; a 15 s capture needs about 150 MB of RAM to analyze.
        --cach also decodes the CACH before every BS-sourced burst (dmr_cach.py: TACT Hamming(7,4) with AT, TC,
        LCSS, the 17 payload bits, short LCs assembled from LCSS 1 3 3 2) and prints it per burst with a summary:
        the split duplex TX half's check (docs/spec/split-duplex.md 10.1).
"""
import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import time

import numpy as np

SERVICE = os.environ.get("ODR_SDR_BENCH_SERVICE") or f"odr-sdrbench@{socket.gethostname()}"
FS_CH = 48000.0
SPS = 10
BURST = 132 * SPS                  # 27.5 ms
PRE = 54 * SPS                     # burst start -> sync start
SLOT_MS = 30.0
DT_NAMES = {0: "PI_HDR", 1: "VLC_HDR", 2: "TERM_LC", 3: "CSBK", 4: "MBC_HDR", 5: "MBC_CONT", 6: "DATA_HDR",
            7: "R1/2", 8: "R3/4", 9: "IDLE", 10: "R1", 11: "USBD", 12: "dt12", 13: "dt13", 14: "dt14", 15: "dt15"}


# ── capture ──────────────────────────────────────────────────────────────────

class _Stop(Exception):
    pass


def _raise_stop(signum, frame):
    raise _Stop(signal.Signals(signum).name)


def cmd_capture(args) -> int:
    import SoapySDR
    from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CS8
    freq = args.freq * 1e6
    center = freq - args.offset_khz * 1e3
    fs = args.rate
    for s in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(s, _raise_stop)
    stopped = False
    dev = stream = None
    meta = {"freq": freq, "center": center, "fs": fs, "fmt": "cs8", "gain": args.gain, "ppm": args.ppm,
            "biastee": not args.no_biastee, "service": args.service}
    got = overflows = clipped = 0
    t0 = None
    try:
        if args.service:
            subprocess.run(["sudo", "systemctl", "stop", args.service], check=True)
            stopped = True
        deadline = time.monotonic() + 10
        while True:
            try:
                dev = SoapySDR.Device("driver=rtlsdr")
                break
            except RuntimeError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.1)
        dev.setSampleRate(SOAPY_SDR_RX, 0, fs)
        if not args.no_biastee:
            dev.writeSetting("biastee", "true")
        dev.setFrequency(SOAPY_SDR_RX, 0, center)
        dev.setGainMode(SOAPY_SDR_RX, 0, False)
        dev.setGain(SOAPY_SDR_RX, 0, args.gain)
        # Small USB transfers (8192 samples = 5.3 ms) so the wall clock of sample 0 is tight.
        stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8, [0], {"bufflen": "16384", "buffers": "32"})
        dev.activateStream(stream)
        want = int(args.seconds * fs)
        n = 8192
        buf = np.empty(2 * n, np.int8)
        with open(args.out, "wb") as f:
            while got < want:
                sr = dev.readStream(stream, [buf], n, timeoutUs=1_000_000)
                now = time.time()
                if sr.ret > 0:
                    f.write(buf[:2 * sr.ret].tobytes())
                    clipped += int(np.count_nonzero((buf[:2 * sr.ret] >= 127) | (buf[:2 * sr.ret] <= -127)))
                    got += sr.ret
                    # Each read returns as its last sample arrives (plus USB latency): the earliest
                    # estimate over the first 2 s is the best one.
                    est = now - got / fs
                    if t0 is None or (got < 2 * fs and est < t0):
                        first = t0 is None
                        t0 = est
                        if first:
                            print(f"READY {time.strftime('%H:%M:%S', time.localtime(t0))}"
                                  f".{int(t0 * 1000) % 1000:03d} t0={t0:.3f}", flush=True)
                elif sr.ret == SoapySDR.SOAPY_SDR_OVERFLOW:
                    overflows += 1
                elif sr.ret == SoapySDR.SOAPY_SDR_TIMEOUT:
                    raise RuntimeError("SDR read timeout")
                else:
                    raise RuntimeError(f"SDR read failed: {sr.ret}")
    except _Stop as e:
        print(f"stopped by {e}", file=sys.stderr)
    finally:
        try:
            if stream is not None:
                dev.deactivateStream(stream)
                dev.closeStream(stream)
        except Exception:
            pass
        dev = None
        if stopped:
            for s in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
                signal.signal(s, signal.SIG_IGN)
            r = subprocess.run(["sudo", "systemctl", "start", args.service])
            print(f"{args.service} restarted (rc {r.returncode})", file=sys.stderr)
        if t0 is not None:
            clip_pct = 100.0 * clipped / max(1, 2 * got)
            meta.update(t0=t0, samples=got, seconds=got / fs, overflows=overflows, clip_pct=round(clip_pct, 3))
            with open(args.out + ".json", "w") as f:
                json.dump(meta, f, indent=1)
            print(f"wrote {got / fs:.2f} s of {freq / 1e6:.5f} MHz (tuned {center / 1e6:.5f}) to {args.out}"
                  f" ({overflows} overflows, ADC clipping {clip_pct:.2f} %)", flush=True)
            if clip_pct > 1.0:
                print(f"WARNING: {clip_pct:.1f} % of I/Q samples at the ADC rails: the capture is saturated. Lower "
                      f"--gain (try 5-10), move the SDR antenna away, or set the radio to low power", flush=True)
    return 0


# ── analysis helpers ─────────────────────────────────────────────────────────

def _load(path: str, meta: dict, freq_override=None):
    """Channel baseband at 48 kS/s, delay-compensated: (f Hz through the matched filter, |x|^2)."""
    from odr_sdrbench.channelizer import Channelizer
    from odr_sdrbench.dsp import FmDemod
    from odr_sdrbench.source import from_bytes
    fs = meta["fs"]
    freq = freq_override or meta["freq"]
    D = int(round(fs / FS_CH))
    chz = Channelizer(fs, [freq - meta["center"]], decimation=D, native=False, threads=1)
    # A tuner that's ppm fast shifts every channel up by f·ppm·1e-6 (as the bench gateway's receiver).
    chz.correction_hz[:] = [freq * meta["ppm"] * 1e-6]
    fm = FmDemod(FS_CH, SPS)
    bps = {"cu8": 2, "cs8": 2, "cs16": 4, "cf32": 8}[meta["fmt"]]
    fs_out, ps_out = [], []
    clipped = total = 0
    with open(path, "rb") as fh:
        while True:
            data = fh.read((1 << 20) * bps)
            if len(data) < bps:
                break
            if meta["fmt"] in ("cs8", "cu8"):
                v = np.frombuffer(data, np.int8 if meta["fmt"] == "cs8" else np.uint8).astype(np.int16)
                if meta["fmt"] == "cu8":
                    v -= 128
                clipped += int(np.count_nonzero((v >= 127) | (v <= -127)))
                total += len(v)
            y = chz.process(from_bytes(data[:len(data) - len(data) % bps], meta["fmt"]))[0]
            f, p = fm.process(y)
            fs_out.append(f)
            ps_out.append(p)
    f = np.concatenate(fs_out)
    p = np.concatenate(ps_out)
    delay = (chz.N - chz.hop) // (2 * D) + fm.delay       # channel FIR + matched filter group delays
    meta["clip_pct_analyzed"] = round(100.0 * clipped / total, 3) if total else None
    return f[delay:], p[delay:]


def _movavg(x: np.ndarray, n: int) -> np.ndarray:
    c = np.concatenate([[0.0], np.cumsum(x, dtype=np.float64)])
    out = np.empty(len(x))
    out[:len(x) - n + 1] = (c[n:] - c[:-n]) / n
    out[len(x) - n + 1:] = out[len(x) - n]
    # Center the window.
    return np.roll(out, n // 2)


def _sync_scores(f: np.ndarray, unit: np.ndarray) -> np.ndarray:
    """(4, len(f)) cosine similarity of the 24 symbols starting at every sample with each family."""
    n = len(f)
    S = np.zeros((len(unit), n), np.float32)
    for ph in range(SPS):
        x = f[ph::SPS].astype(np.float64)
        if len(x) < 24:
            continue
        m = len(x) - 23
        c1 = np.concatenate([[0.0], np.cumsum(x)])
        c2 = np.concatenate([[0.0], np.cumsum(x * x)])
        var = (c2[24:24 + m] - c2[:m]) - (c1[24:24 + m] - c1[:m]) ** 2 / 24
        norm = np.where(var > 24 * 100.0 ** 2, np.sqrt(np.maximum(var, 0)), np.inf)
        for k, t in enumerate(unit):
            S[k, ph::SPS][:m] = np.correlate(x, t, mode="valid") / norm
    return S


def _slice(vals, center, outer):
    from odr_sdrbench.demod import _slice as s
    return s(vals, center, outer)


class Burst:
    def __init__(self, **kw):
        self.__dict__.update(kw)


# ── analysis ─────────────────────────────────────────────────────────────────

def cmd_analyze(args) -> int:
    from odr_sdrbench.dmr import fec, pdu
    from odr_sdrbench import layer2 as L2
    meta = json.load(open(args.file + ".json")) if os.path.exists(args.file + ".json") else \
        {"freq": args.freq * 1e6, "center": args.freq * 1e6 - args.offset_khz * 1e3, "fs": args.rate,
         "fmt": "cu8" if args.file.endswith("cu8") else "cs8", "ppm": args.ppm, "t0": None}
    f, p = _load(args.file, meta, args.freq * 1e6 if args.freq_given else None)
    n = len(f)
    ms = lambda i: i / FS_CH * 1000.0  # noqa: E731
    t0 = meta.get("t0")

    # Power envelope: 0.1 ms (5 samples) and 1 ms averages, in dB.
    env = 10 * np.log10(_movavg(p, 5) + 1e-12)
    env1 = 10 * np.log10(_movavg(p, 48) + 1e-12)
    noise = float(np.percentile(env1, 10))
    # Thermal noise in the ~15 kHz channel (NF ~3 dB, referred to the antenna through the LNA): rough dBm.
    dbm_off = args.dbm_offset if args.dbm_offset is not None else (-174 + 10 * np.log10(15e3) + 3) - noise
    on_th, off_th = noise + args.threshold_db, noise + args.threshold_db - 3

    # Carrier segments with sub-sample edges (linear interpolation of the 0.1 ms envelope).
    segs = []
    state, start = False, None
    above_on = env > on_th
    below_off = env < off_th
    i = 0
    idx_on = np.nonzero(above_on)[0]
    while True:
        if not state:
            j = idx_on[np.searchsorted(idx_on, i)] if np.searchsorted(idx_on, i) < len(idx_on) else None
            if j is None:
                break
            state, start = True, j
            i = j
        else:
            k = np.nonzero(below_off[i:])[0]
            if len(k) == 0:
                segs.append((start, n - 1, True))
                break
            j = i + int(k[0])
            segs.append((start, j, False))
            state, i = False, j
    segs = [(a, b, o) for a, b, o in segs if b - a >= 24]       # >= 0.5 ms

    def cross(i, th):
        """Fractional sample where env crosses th between i-1 and i."""
        if i <= 0:
            return float(i)
        a, b = env[i - 1], env[i]
        return i - 1 + (th - a) / (b - a) if b != a else float(i)

    def ramp_up(a):
        plat = np.median(env[a + 48:a + 144]) if a + 144 < n else env[a]
        lo = noise + 6
        i0 = a
        while i0 > 0 and env[i0] > lo:
            i0 -= 1
        i1 = a
        while i1 < min(n - 1, a + 480) and env[i1] < plat - 3:
            i1 += 1
        return (i1 - i0) / FS_CH * 1000, plat

    def ramp_down(b):
        plat = np.median(env[max(0, b - 144):b - 48]) if b > 144 else env[b]
        lo = noise + 6
        i1 = b
        while i1 < n - 1 and env[i1] > lo:
            i1 += 1
        i0 = b
        while i0 > max(0, b - 480) and env[i0] < plat - 3:
            i0 -= 1
        return (i1 - i0) / FS_CH * 1000, plat

    # Sync search over every sample.
    temps = np.array([L2.pattern_symbols(pat) for _, pat, _ in L2.FAMILIES], np.float64)
    t = temps - temps.mean(axis=1, keepdims=True)
    unit = t / np.linalg.norm(t, axis=1, keepdims=True)
    S = _sync_scores(f, unit)
    best_fam = np.argmax(np.abs(S), axis=0)
    best = S[best_fam, np.arange(n)]
    gate = env1 > noise + 6
    cand = np.nonzero((np.abs(best) >= args.min_corr) & gate)[0]
    cand = cand[(cand >= PRE + 24 * SPS) & (cand < n - (BURST - PRE) - 1)]
    order = cand[np.argsort(-np.abs(best[cand]))]
    taken = np.zeros(0, np.int64)
    peaks = []
    for c in order:
        if len(taken) and np.min(np.abs(taken - c)) < 12 * SPS:
            continue
        taken = np.append(taken, c)
        peaks.append(int(c))
    peaks.sort()

    bursts, weak = [], 0
    for c in peaks:
        fam = int(best_fam[c])
        name, pattern, slot_hint = L2.FAMILIES[fam]
        sc = best[c]
        voice = sc > 0
        # Sub-sample peak (parabola through |score|).
        y0, y1, y2 = abs(S[fam, c - 1]), abs(S[fam, c]), abs(S[fam, c + 1])
        den = y0 - 2 * y1 + y2
        frac = 0.5 * (y0 - y2) / den if den < 0 else 0.0
        s = f[c + np.arange(24) * SPS]
        tp = temps[fam] * (1 if voice else -1)
        hi_v, lo_v = s[tp > 0].mean(), s[tp < 0].mean()
        center, outer = (hi_v + lo_v) / 2, (hi_v - lo_v) / 2
        if not (300 < outer < 5000):
            continue
        st = c - PRE
        bits = _slice(f[st + np.arange(132) * SPS], center, outer)
        raw = fec.bits_to_bytes(bits)
        errs = L2.sync_errors(raw, pattern if voice else L2.data_sync(pattern))
        if errs > args.max_sync_errors:
            continue
        # A sync "found" in a slot with no carrier is noise: real bursts sit well above the carrier threshold
        if not args.keep_weak and 10 * np.log10(np.mean(p[max(st, 0):st + BURST]) + 1e-12) < off_th:
            weak += 1
            continue
        b = Burst(start=st + frac, fam=name, voice=voice, errs=errs, cc=None, dt=None, st_err=None,
                  center=center, outer=outer, corr=abs(sc), kind="", slot=slot_hint, raw=raw)
        if voice:
            b.kind = "VOICE_A"
        else:
            cc, dt, se = L2.decode_slot_type(raw)
            b.cc, b.dt, b.st_err = cc, dt, se
            b.kind = DT_NAMES.get(dt, "?") if dt is not None else "slottype?"
        if name == "bs":
            cb = _slice(f[st - 12 * SPS + np.arange(12) * SPS], center, outer)
            b.slot = L2.decode_cach_slot(cb)
        bursts.append(b)

    # Voice B-F (and late entry): walk 60 ms steps from every sync burst and check the EMB.
    def covered(pos):
        return any(abs(x.start - pos) < 10 * SPS for x in bursts)

    def emb_at(pos, ref):
        bestv = None
        for d in range(-6, 7):
            q = int(round(pos)) + d
            if q < 0 or q + BURST >= n:
                continue
            if np.mean(env1[q:q + BURST]) < noise + 6:
                continue
            raw = fec.bits_to_bytes(_slice(f[q + np.arange(132) * SPS], ref.center, ref.outer))
            cc, pi, lcss, e = L2.decode_emb(raw)
            if cc is None or (args.cc is not None and cc != args.cc) or e > 1:
                continue
            v = (f[q + np.arange(132) * SPS] - ref.center) / (ref.outer / 3)
            serr = float(np.mean(np.abs(v - np.clip(np.round((v + 3) / 2) * 2 - 3, -3, 3))))
            key = (e, serr)
            if bestv is None or key < bestv[0]:
                bestv = (key, q, cc, pi, lcss, e, raw)
        return bestv

    period = 288 * SPS
    anchors = [b for b in bursts]
    for a in anchors:
        for direction in (1, -1):
            pos, misses, k = a.start, 0, 0
            while misses < 2 and k < 12:
                k += 1
                pos = pos + direction * period
                if pos < 0 or pos + BURST >= n or covered(pos):
                    break
                r = emb_at(pos, a)
                if r is None:
                    misses += 1
                    continue
                misses = 0
                _, q, cc, pi, lcss, e, raw = r
                letter = "BCDEF"[k - 1] if (direction == 1 and a.kind == "VOICE_A" and k <= 5) else "?"
                bursts.append(Burst(start=float(q), fam="emb", voice=True, errs=e, cc=cc, dt=None, st_err=None,
                                    center=a.center, outer=a.outer, corr=0.0,
                                    kind=f"VOICE_{letter} pi{pi} lcss{lcss}", slot=a.slot, raw=raw,
                                    anchor_fam=a.fam))
                pos = q
    bursts.sort(key=lambda b: b.start)
    if args.cach:
        # The 12 symbols before each BS-sourced burst are its CACH (24 bits, TX(23) first)
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import dmr_cach
        cdec = dmr_cach.CachDecoder()
        for b in bursts:
            b.cach = None
            if b.fam == "bs" or getattr(b, "anchor_fam", None) == "bs":
                st = int(round(b.start))
                if st - 12 * SPS >= 0:
                    cb = _slice(f[st - 12 * SPS + np.arange(12) * SPS], b.center, b.outer)
                    b.cach = cdec.push(dmr_cach.bits_to_int(cb))

    # LCs: voice LC header / terminator (RS(12,9) checked by pdu), CSBK opcode, embedded LC from B-E (voice E)
    frags = {}
    for b in bursts:
        b.lc = None
        if b.raw is None:
            continue
        if b.kind in ("VLC_HDR", "TERM_LC"):
            lc = pdu.decode_voice_lc(b.raw, b.dt)
            if lc is not None:
                b.lc = {"flco": lc.flco, "src": lc.src_id, "dst": lc.dst_id, "so": lc.service_options}
        elif b.kind == "CSBK":
            c = pdu.decode_csbk(b.raw)
            if c is not None:
                b.lc = {"csbko": c.opcode, "crc_ok": c.crc_ok, "src": c.src_id, "dst": c.dst_id}
        elif b.kind == "VOICE_A":
            frags = {}
        elif b.kind[:7] in ("VOICE_B", "VOICE_C", "VOICE_D", "VOICE_E"):
            frags[b.kind[6]] = fec.embedded_signalling(b.raw)
            if b.kind[6] == "E" and all(k in frags for k in "BCDE"):
                lc9 = fec.embedded_lc_decode(sum((frags[k] for k in "BCDE"), []))
                if lc9 is not None:
                    lc = pdu.parse_full_lc(lc9)
                    b.lc = {"emb_flco": lc.flco, "fid": lc.fid, "src": lc.src_id, "dst": lc.dst_id,
                            "raw": bytes(lc9).hex()}     # talker alias / GPS blocks are decoded from the raw LC
                else:
                    b.lc = {"emb": "bad"}

    # Per-burst power, spread, frequency offset.
    for b in bursts:
        a = int(b.start)
        b.pwr = 10 * np.log10(np.mean(p[a:a + BURST]) + 1e-12)
        blocks = p[a + 2 * SPS:a + BURST - 2 * SPS]
        blocks = blocks[:len(blocks) // 24 * 24].reshape(-1, 24).mean(axis=1)     # 0.5 ms blocks
        bd = 10 * np.log10(blocks + 1e-12)
        b.spread = float(bd.max() - bd.min())
        b.step = b.spread > args.step_db

    # Window.
    lo_ms = args.from_ms if args.from_ms is not None else -1e9
    hi_ms = args.to_ms if args.to_ms is not None else 1e12

    print(f"# {args.file}: {n / FS_CH:.2f} s at 48 kS/s, {meta['freq'] / 1e6:.5f} MHz (tuned "
          f"{meta['center'] / 1e6:.5f}, ppm {meta['ppm']}), overflows {meta.get('overflows', '?')}, "
          f"gain {meta.get('gain', '?')}, ADC clipping {meta.get('clip_pct_analyzed')} %")
    if (meta.get("clip_pct_analyzed") or 0) > 1.0:
        print("# WARNING: the capture is saturated (> 1 % of I/Q samples at the rails): power, ramps and frequency "
              "offset are unreliable and bursts may be missed")
    if t0:
        print(f"# sample 0 = {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(t0))}.{int(t0 * 1000) % 1000:03d}"
              f" (t0={t0:.3f})")
    print(f"# noise floor {noise:.1f} dBFS (1 ms), carrier threshold {on_th:.1f}/{off_th:.1f} dBFS, "
          f"dBm ~= dBFS {dbm_off:+.1f}{'' if args.dbm_offset is not None else ' (from thermal noise, rough)'}")
    if weak:
        print(f"# {weak} sync match(es) below the carrier threshold dropped as noise (--keep-weak to list them)")
    tref = bursts[0].start if bursts else 0
    if not args.edges_only:
        print()
        print("    t_ms   phase sl  family  err cc kind                      dBFS   ~dBm  sprd  foffHz  lc")
        for b in bursts:
            tm = ms(b.start)
            if not lo_ms <= tm <= hi_ms:
                continue
            rel = ms(b.start - tref)
            phase = (rel + SLOT_MS / 2) % SLOT_MS - SLOT_MS / 2
            sl = "AB"[int(((rel + SLOT_MS / 2) % (2 * SLOT_MS)) // SLOT_MS)]
            fam = b.fam + ("" if b.fam == "emb" else ("-v" if b.voice else "-d"))
            err = f"{b.errs}" + (f"+{b.st_err}" if b.st_err else "")
            cc = "-" if b.cc is None else str(b.cc)
            kind = b.kind + (f" ts{b.slot}" if b.fam == "bs" and b.slot else "")
            print(f"{tm:9.2f} {phase:+6.2f} {sl}  {fam:7s} {err:>4s} {cc:>2s} {kind:24s} {b.pwr:6.1f} "
                  f"{b.pwr + dbm_off:6.1f} {b.spread:5.1f}{'!' if b.step else ' '} {b.center:+6.0f}"
                  + (f"  {b.lc}" if b.lc else ""))

    # Edges.
    print()
    print("carrier edges (0.1 ms envelope):")
    edges_out = []
    for a, bb, open_end in segs:
        ta = ms(cross(a, on_th))
        if lo_ms <= ta <= hi_ms or lo_ms <= ms(bb) <= hi_ms:
            ru, plat = ramp_up(a)
            print(f"  {'KEY-UP':9s} {ta:9.2f} ms  ramp {ru:4.2f} ms  level {plat:6.1f} dBFS")
            seg = {"up_ms": round(ta, 2), "ramp_up_ms": round(ru, 2), "level_dbfs": round(float(plat), 1), "steps": []}
            edges_out.append(seg)
            # Steps inside the segment: 1 ms averages 1 ms apart differing by > step_db.
            x = env1[a + 96:bb - 96]
            if len(x) > 96:
                d = x[48:] - x[:-48]
                hits = np.nonzero(np.abs(d) > args.step_db)[0]
                last = -10 ** 9
                for h in hits:
                    if h - last < 240:
                        continue
                    last = h
                    j = h + int(np.argmax(np.abs(d[h:h + 96]))) if h + 96 <= len(d) else h
                    print(f"  {'STEP':9s} {ms(a + 96 + j + 24):9.2f} ms  {d[j]:+5.1f} dB")
                    seg["steps"].append([round(ms(a + 96 + j + 24), 2), round(float(d[j]), 1)])
            if not open_end:
                tb = ms(cross(bb, off_th))
                rd, plat2 = ramp_down(bb)
                print(f"  {'KEY-DOWN':9s} {tb:9.2f} ms  ramp {rd:4.2f} ms  (on {tb - ta:8.2f} ms)")
                seg.update(down_ms=round(tb, 2), ramp_down_ms=round(rd, 2), on_ms=round(tb - ta, 2))

    # Summary.
    print()
    print("summary:")
    sel = [b for b in bursts if lo_ms <= ms(b.start) <= hi_ms]
    counts = {}
    for b in sel:
        rel = ms(b.start - tref)
        sl = "AB"[int(((rel + SLOT_MS / 2) % (2 * SLOT_MS)) // SLOT_MS)]
        key = (b.fam + ("" if b.fam == "emb" else ("-v" if b.voice else "-d")), b.kind.split()[0], sl)
        counts[key] = counts.get(key, 0) + 1
    for k in sorted(counts):
        print(f"  {k[0]:7s} {k[1]:10s} slot {k[2]}: {counts[k]}")
    print(f"  total bursts {len(sel)}, carrier segments {len(segs)}")
    ms_b = [b for b in sel if b.fam == "ms"]
    rep = [b for b in sel if b.fam in ("ts1", "ts2", "bs")]
    gaps = []
    for b in ms_b:
        nxt = [r for r in rep if 45 <= ms(r.start - b.start) <= 135]
        if nxt:
            gaps.append(ms(nxt[0].start - b.start))
    if gaps:
        g = np.array(gaps)
        print(f"  inbound(MS) -> repeat(TS/BS) gap: n={len(g)} min {g.min():.2f} median {np.median(g):.2f} "
              f"max {g.max():.2f} ms")
    elif ms_b:
        print(f"  no repeats (TS/BS) 45-135 ms after the {len(ms_b)} MS bursts")
    same = [ms(b2.start - b1.start) for b1, b2 in zip(sel, sel[1:])
            if b1.fam == b2.fam or (b1.fam == "emb" or b2.fam == "emb")]
    sp = [x for x in same if 55 <= x <= 65]
    if sp:
        print(f"  same-slot spacing (55-65 ms): n={len(sp)} mean {np.mean(sp):.3f} sd {np.std(sp):.3f} ms")
    ov = [(b1, b2) for b1, b2 in zip(sel, sel[1:]) if ms(b2.start - b1.start) < 27.5]
    steps = [b for b in sel if b.step]
    print(f"  overlapping bursts (starts < 27.5 ms apart): {len(ov)}"
          + "".join(f"\n    {ms(x.start):.2f} {x.fam} / {ms(y.start):.2f} {y.fam}" for x, y in ov[:20]))
    print(f"  bursts with a power step > {args.step_db} dB inside: {len(steps)}"
          + (" at " + ", ".join(f"{ms(b.start):.1f}" for b in steps[:20]) if steps else ""))
    # Carrier time not explained by any decoded burst.
    cov = np.zeros(n, bool)
    for b in bursts:
        cov[int(b.start):int(b.start) + BURST] = True
    unexpl = []
    for a, bb, _ in segs:
        u = ~cov[a:bb]
        if u.sum() > 5 * FS_CH / 1000:
            unexpl.append((ms(a), u.sum() / FS_CH * 1000))
    if unexpl:
        print("  carrier with no decoded burst (>5 ms): "
              + ", ".join(f"segment @{a:.1f}: {u:.1f} ms" for a, u in unexpl[:20]))
    if args.cach:
        cs = [b for b in sel if getattr(b, "cach", None) is not None]
        print()
        print("cach (BS-sourced bursts):")
        for b in cs:
            c = b.cach
            if not c["ok"]:
                print(f"  {ms(b.start):9.2f} tact bad")
                continue
            line = f"  {ms(b.start):9.2f} tact ok AT {c['at']} TC {c['tc']} LCSS {c['lcss']} payload {c['payload']:05X}"
            if "slc" in c:
                slc, ok = c["slc"]
                line += f"  slc {slc:07X} {'ok' if ok else 'bad'} {dmr_cach.describe_slc(slc)}"
            print(line)
        good = [b.cach for b in cs if b.cach["ok"]]
        alt = sum(1 for x, y in zip(good, good[1:]) if x["tc"] != y["tc"])
        slcs = [b.cach["slc"] for b in cs if "slc" in b.cach]
        print(f"  summary: {len(cs)} BS bursts, tact ok {len(good)}, TC alternating {alt}/{max(len(good) - 1, 0)}, "
              f"AT busy {sum(c['at'] for c in good)}, short LCs {sum(1 for _, ok in slcs if ok)} ok / "
              f"{sum(1 for _, ok in slcs if not ok)} bad"
              + (f", last {dmr_cach.describe_slc(slcs[-1][0])}" if slcs else ""))
    if args.json:
        def slot_of(b):
            rel = ms(b.start - tref)
            return "AB"[int(((rel + SLOT_MS / 2) % (2 * SLOT_MS)) // SLOT_MS)]
        out = {
            "file": args.file, "meta": meta, "seconds": n / FS_CH, "noise_dbfs": round(noise, 1),
            "dbm_offset": round(float(dbm_off), 1),
            "bursts": [{"t_ms": round(ms(b.start), 2), "slot": slot_of(b), "family": b.fam, "voice": bool(b.voice),
                        "sync_errors": b.errs, "slot_type_errors": b.st_err, "cc": b.cc, "dt": b.dt,
                        "kind": b.kind.split()[0], "detail": b.kind, "cach_ts": b.slot if b.fam == "bs" else None,
                        "dbfs": round(float(b.pwr), 1), "spread_db": round(b.spread, 1), "step": bool(b.step),
                        "foff_hz": round(float(b.center)), "lc": b.lc,
                        "cach": getattr(b, "cach", None)} for b in sel],
            "edges": edges_out,
            "summary": {
                "counts": {" ".join(k): v for k, v in sorted(counts.items())},
                "same_slot_spacing_ms": {"n": len(sp), "mean": round(float(np.mean(sp)), 3),
                                         "sd": round(float(np.std(sp)), 3)} if sp else None,
                "repeat_gap_ms": sorted(round(x, 2) for x in gaps),
                "overlaps": len(ov), "power_steps": len(steps),
                "unexplained_carrier": [[round(a, 1), round(u, 1)] for a, u in unexpl],
            },
        }
        with open(args.json, "w") as fh:
            json.dump(out, fh, indent=1, default=lambda o: o.item() if hasattr(o, "item") else str(o))
        print(f"# wrote {args.json}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("capture", "analyze"):
        s = sub.add_parser(name)
        s.add_argument("--freq", type=float, default=461.6875, help="channel MHz")
        s.add_argument("--offset-khz", type=float, default=-200.0, help="channel minus tuner center (kHz)")
        s.add_argument("--rate", type=float, default=1.536e6)
        s.add_argument("--ppm", type=float, default=0.7)
        if name == "capture":
            s.add_argument("--seconds", type=float, required=True)
            s.add_argument("--out", required=True)
            s.add_argument("--gain", type=float, default=15.0, help="30 (the gateway's) clips the ADC ~25%% with the bias-tee LNA")
            s.add_argument("--no-biastee", action="store_true")
            s.add_argument("--service", default=SERVICE, help="'' to not stop/start any service")
        else:
            s.add_argument("file")
            s.add_argument("--cc", type=int, default=2, help="color code for EMB checks (-1: any)")
            s.add_argument("--from", dest="from_ms", type=float)
            s.add_argument("--to", dest="to_ms", type=float)
            s.add_argument("--edges-only", action="store_true")
            s.add_argument("--dbm-offset", type=float, help="dBFS -> dBm (default: from the noise floor)")
            s.add_argument("--threshold-db", type=float, default=10.0, help="carrier on, above the noise floor")
            s.add_argument("--step-db", type=float, default=3.0)
            s.add_argument("--min-corr", type=float, default=0.75, help="sync correlation threshold")
            s.add_argument("--max-sync-errors", type=int, default=8)
            s.add_argument("--keep-weak", action="store_true", help="also list sync matches with no carrier (noise)")
            s.add_argument("--json", default=None, help="also write bursts, edges and summary as JSON to this file")
            s.add_argument("--cach", action="store_true", help="decode the CACH of BS-sourced bursts (split duplex)")
    args = ap.parse_args()
    if args.cmd == "capture":
        return cmd_capture(args)
    args.freq_given = "--freq" in sys.argv
    if args.cc is not None and args.cc < 0:
        args.cc = None
    return cmd_analyze(args)


if __name__ == "__main__":
    sys.exit(main())
