#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Yaesu System Fusion reference (docs/spec/ysf-modem.md), written apart from core/ysf to check it, and a decoder
and signal maker for the bench (docs/testing/ysf-bench.md).

Worked from Yaesu's "Amateur Radio Digital Standards" (v1.0 of 15 Jan 2013, rev 1.02 of 29 Jul 2015), sections 3-4
and appendices A-B, and nothing else. It is deliberately written another way than the C:

  - bits are Python lists, most significant first, one int per bit, never packed
  - the CRC is polynomial long division of the message times x^16 by x^16 + x^12 + x^5 + 1, then inverted (the C
    runs the figure's shift register)
  - the Golay code multiplies by appendix A's generator matrix as printed, row by row; its decoder tries every one of
    the 4096 code words (the C searches error patterns)
  - the convolutional code convolves the input with the two generator polynomials' coefficient lists
  - the interleavers build the standard's write-by-row, read-by-column matrices as lists of lists
  - the whitening simulates figure 4-28's nine registers by name

    tools/ysf_ref.py vectors > tests/vectors/ysf.txt   write the vectors the host test reads
    tools/ysf_ref.py --self-test                       check the reference against itself and the CRC catalog
    tools/ysf_ref.py rrc                               the pulse table of core/ysf/ysf_c4fm.c
    tools/ysf_ref.py synth OUT [--src CALL ...]        a whole DN transmission as I/Q or discriminator audio
    tools/ysf_ref.py decode CAPTURE --offset HZ        decode a capture: syncs, FICH, callsigns, voice bits, levels

synth and decode need numpy (decode of an I/Q capture also scipy); the rest needs nothing outside the standard library.
"""
import math
import os
import sys

# --- bits ---


def bits_of(data):
    """Bytes to bits, each byte most significant bit first (3.2 (2): dibits 'starting from the leading bit')."""
    return [(b >> (7 - i)) & 1 for b in data for i in range(8)]


def bytes_of(bits):
    bits = list(bits) + [0] * (-len(bits) % 8)
    return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def int_bits(v, n):
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]


def bits_int(bits):
    v = 0
    for b in bits:
        v = (v << 1) | b
    return v


SYNC = int_bits(0xD471C9634D, 40)

# --- CRC (4.5 (1) (b)) ---

CRC_POLY = [1] + int_bits(0x1021, 16)  # x^16 + x^12 + x^5 + 1, 17 coefficients from x^16 down


def crc16(bits):
    """Remainder of bits(x) x^16 by the polynomial (registers from zero), every bit inverted."""
    work = list(bits) + [0] * 16
    for i in range(len(bits)):
        if work[i]:
            for j, p in enumerate(CRC_POLY):
                work[i + j] ^= p
    return [1 - b for b in work[-16:]]


# --- Golay (24,12,8), appendix A ---

GOLAY_ROWS = [  # the generator matrix as printed, row d11 first
    "100000000000110001110101",
    "010000000000011000111011",
    "001000000000111101101000",
    "000100000000011110110100",
    "000010000000001111011010",
    "000001000000110110011001",
    "000000100000011011001101",
    "000000010000001101100111",
    "000000001000110111000110",
    "000000000100101010010111",
    "000000000010100100111110",
    "000000000001100011101011",
]


def golay_encode(d12):
    out = [0] * 24
    for i, d in enumerate(d12):
        if d:
            out = [a ^ int(b) for a, b in zip(out, GOLAY_ROWS[i])]
    return out


_GOLAY_BOOK = None


def golay_decode(w24):
    """The nearest code word by trying all 4096; None when the nearest is 4 or more away (not unique)."""
    global _GOLAY_BOOK
    if _GOLAY_BOOK is None:
        _GOLAY_BOOK = [(bits_int(golay_encode(int_bits(m, 12))), m) for m in range(4096)]
    w = bits_int(w24)
    best = min(_GOLAY_BOOK, key=lambda e: bin(e[0] ^ w).count("1"))
    if bin(best[0] ^ w).count("1") > 3:
        return None
    return int_bits(best[1], 12)


# --- convolutional code, appendix B ---

G1 = [1, 0, 0, 1, 1]  # coefficients of x^0 .. x^4: 1 + x^3 + x^4
G2 = [1, 1, 1, 0, 1]  # 1 + x + x^2 + x^4


def convolve(bits):
    """Rate 1/2, K = 5, four zero tail bits appended by the caller. Output G1, G2 alternately."""
    out = []
    for k in range(len(bits)):
        y1 = y2 = 0
        for i in range(5):
            u = bits[k - i] if k - i >= 0 else 0
            y1 ^= G1[i] & u
            y2 ^= G2[i] & u
        out += [y1, y2]
    return out


def viterbi(soft):
    """Soft values (positive = 1) in coded order back to the input bits, ending in state zero."""
    n = len(soft) // 2
    INF = float("inf")
    metric = {(): 0.0}
    # Keep the path per state: state = the last four inputs as a tuple
    paths = {(0, 0, 0, 0): (0.0, [])}
    for k in range(n):
        a, b = soft[2 * k], soft[2 * k + 1]
        nxt = {}
        for st, (m, path) in paths.items():
            for u in (0, 1):
                hist = (u,) + st  # u[k], u[k-1] .. u[k-4]
                y1 = sum(G1[i] & hist[i] for i in range(5)) & 1
                y2 = sum(G2[i] & hist[i] for i in range(5)) & 1
                mm = m + (a if y1 else -a) + (b if y2 else -b)
                ns = hist[:4]
                if ns not in nxt or mm > nxt[ns][0]:
                    nxt[ns] = (mm, path + [u])
        paths = nxt
    del metric, INF
    return paths[(0, 0, 0, 0)][1]


# --- interleavers ---


def dibit_interleave(bits, width):
    """Dibits written row by row into rows of `width`, 20 rows, read out column by column (figures 4-26, 4-30, 4-31)."""
    dib = [bits[i:i + 2] for i in range(0, len(bits), 2)]
    rows = [dib[r * width:(r + 1) * width] for r in range(20)]
    out = []
    for c in range(width):
        for r in range(20):
            out += rows[r][c]
    return out


def dibit_deinterleave(bits, width):
    order = dibit_interleave(list(range(len(bits))), width)  # positions as a permutation (pairs stay together)
    out = [0] * len(bits)
    for j, src in enumerate(order):
        out[src] = bits[j]
    return out


def vch_interleave(bits):
    """104 bits written into 4 rows of 26, read out column by column (figure 4-32)."""
    rows = [bits[r * 26:(r + 1) * 26] for r in range(4)]
    return [rows[r][c] for c in range(26) for r in range(4)]


def vch_deinterleave(bits):
    order = vch_interleave(list(range(104)))
    out = [0] * 104
    for j, src in enumerate(order):
        out[src] = bits[j]
    return out


# --- whitening, figure 4-28 ---


def pn(n):
    """S08..S00 from 1 1 1 0 0 1 0 0 1; out = S00; the feedback S00 XOR S04 enters S08 as the rest shift down."""
    reg = {"S08": 1, "S07": 1, "S06": 1, "S05": 0, "S04": 0, "S03": 1, "S02": 0, "S01": 0, "S00": 1}
    out = []
    for _ in range(n):
        out.append(reg["S00"])
        fb = reg["S00"] ^ reg["S04"]
        for i in range(8):
            reg["S%02d" % i] = reg["S%02d" % (i + 1)]
        reg["S08"] = fb
    return out


def whiten(bits):
    return [b ^ p for b, p in zip(bits, pn(len(bits)))]


# --- coded units ---


def fich_encode(v32):
    d = int_bits(v32, 32)
    d += crc16(d)
    g = []
    for w in range(4):
        g += golay_encode(d[12 * w:12 * w + 12])
    return dibit_interleave(convolve(g + [0] * 4), 5)


def fich_decode(soft):
    u = viterbi(dibit_deinterleave(soft, 5))[:96]
    d = []
    for w in range(4):
        r = golay_decode(u[24 * w:24 * w + 24])
        if r is None:
            return None
        d += r
    if crc16(d[:32]) != d[32:]:
        return None
    return bits_int(d[:32])


def dch_encode(data, order="data"):
    """order 'data': whiten the data, CRC over the whitened data (figure 4-27); 'all': CRC, then whiten both."""
    b = bits_of(data)
    if order == "data":
        b = whiten(b)
        b += crc16(b)
    else:
        b = whiten(b + crc16(b))
    width = 5 if len(data) == 10 else 9
    return dibit_interleave(convolve(b + [0] * 4), width)


def dch_decode(soft, nbytes):
    width = 5 if nbytes == 10 else 9
    u = viterbi(dibit_deinterleave(soft, width))[:nbytes * 8 + 16]
    if crc16(u[:-16]) == u[-16:]:
        return bytes_of(whiten(u[:-16])), "data"
    w = whiten(u)
    if crc16(w[:-16]) == w[-16:]:
        return bytes_of(w[:-16]), "all"
    return None, None


def vd2_voice_encode(bits49):
    s = []
    for b in bits49[:27]:
        s += [b, b, b]
    s += bits49[27:] + [0]
    return vch_interleave(whiten(s))


def vd2_voice_decode(soft104):
    s = vch_deinterleave(soft104)
    s = [-v if p else v for v, p in zip(s, pn(104))]
    out = [1 if s[3 * i] + s[3 * i + 1] + s[3 * i + 2] > 0 else 0 for i in range(27)]
    out += [1 if v > 0 else 0 for v in s[81:103]]
    return out


def fich_pack(fi=0, cs=2, cm=0, bn=0, bt=0, fn=0, ft=5, dev=0, mr=0, voip=0, dt=2, sql=0, sc=0):
    return (fi << 30 | cs << 28 | cm << 26 | bn << 24 | bt << 22 | fn << 19 | ft << 16 | dev << 14 | mr << 11 |
            voip << 10 | dt << 8 | sql << 7 | sc)


def callsign(s):
    return s.upper().encode("ascii")[:10].ljust(10, b" ")


def frame_header(fich, dest, src, down=b" " * 10, up=b" " * 10, order="data"):
    u1 = dch_encode(dest + src, order)
    u2 = dch_encode(down + up, order)
    pay = []
    for s in range(5):
        pay += u1[72 * s:72 * s + 72] + u2[72 * s:72 * s + 72]
    return SYNC + fich_encode(fich) + pay


def frame_vd2(fich, dch10, voices, order="data"):
    u = dch_encode(dch10, order)
    pay = []
    for s in range(5):
        pay += u[40 * s:40 * s + 40] + vd2_voice_encode(voices[s])
    return SYNC + fich_encode(fich) + pay


def transmission(src, dest="ALL", voices=None, frames=12, headers=1, order="data", dgid=0):
    """The bits of a DN transmission as core/ysf sends it: headers, rolling V/D 2 frames FN 0-5, the terminator."""
    csd = {"dest": callsign(dest), "src": callsign(src), "down": b" " * 10, "up": b" " * 10}
    rem = b" " * 20
    dch = [csd["dest"], csd["src"], csd["down"], csd["up"], rem[:10], rem[10:]]
    out = []
    for _ in range(headers):
        out += frame_header(fich_pack(fi=0, sc=dgid), csd["dest"], csd["src"], order=order)
    n = 0
    for f in range(frames):
        fn = f % 6
        v = []
        for _ in range(5):
            v.append(voices(n) if voices else [0] * 49)
            n += 1
        out += frame_vd2(fich_pack(fi=1, fn=fn, sc=dgid), dch[fn], v, order)
    out += frame_header(fich_pack(fi=2, sc=dgid), csd["dest"], csd["src"], order=order)
    return out


# What a Yaesu radio's DN over looks like, as an FT-5D's were received (docs/spec/ysf-modem.md 3.5): 240 symbols of
# alternating -3 / +3 after its key-up, one header, radio ID mode (CM 1: the destination field is "*****" and the
# caller's radio ID), FT 6 with ten bytes of data (DT1) at FN 6, the radio ID again in Rem4, the terminator. The
# callsign, ID and data here are made up; the layout is the radio's. DT1's content isn't defined by the standard (the
# radio's ended in a byte that sums the six before it); a receiver must only keep it out of the callsign fields.
YAESU_DT1 = bytes.fromhex("11223344556677000000")


def yaesu_transmission(src, radio_id, voices=None, frames=9, preamble=240):
    rid = radio_id.upper().encode("ascii")[:5].ljust(5, b" ")
    dest = b"*****" + rid
    srcb = callsign(src)
    dch = [dest, srcb, b" " * 10, b" " * 10, b" " * 10, b" " * 5 + rid, YAESU_DT1]
    out = []
    for i in range(preamble):
        out += [1, 1] if i % 2 == 0 else [0, 1]  # -3, +3 (table 3-1)
    out += frame_header(fich_pack(fi=0, cm=1, ft=6), dest, srcb)
    n = 0
    for f in range(frames):
        fn = f % 7
        v = []
        for _ in range(5):
            v.append(voices(n) if voices else [0] * 49)
            n += 1
        out += frame_vd2(fich_pack(fi=1, cm=1, fn=fn, ft=6), dch[fn], v)
    out += frame_header(fich_pack(fi=2, cm=1, ft=6), dest, srcb)
    return out


def voice_pattern(n):
    """Known pseudo-random 49-bit frames (the same rule as the host test's)."""
    x = (n * 2654435761 + 12345) & 0xFFFFFFFF
    bits = []
    for _ in range(49):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        bits.append(x & 1)
    return bits


# --- the pulse ---


def rrc(t, a=0.2):
    if abs(t) < 1e-12:
        return 1 - a + 4 * a / math.pi
    if abs(abs(t) - 1 / (4 * a)) < 1e-9:
        return a / math.sqrt(2) * ((1 + 2 / math.pi) * math.sin(math.pi / (4 * a)) +
                                   (1 - 2 / math.pi) * math.cos(math.pi / (4 * a)))
    return (math.sin(math.pi * t * (1 - a)) + 4 * a * t * math.cos(math.pi * t * (1 + a))) / \
        (math.pi * t * (1 - (4 * a * t) ** 2))


def rrc_table(per=32, span=6):
    return [round(16384 * rrc(i / per)) for i in range(-span * per, span * per + 1)]


LEVEL = {(0, 0): 1, (0, 1): 3, (1, 0): -1, (1, 1): -3}  # table 3-1


# --- vectors ---


def hexs(b):
    return b.hex().upper()


VOICE_VECTORS = [[0] * 49, [1] * 49, voice_pattern(0), voice_pattern(7)]


def vectors():
    print("# SPDX-License-Identifier: MIT")
    print("# Copyright (c) 2026 The RadioFoundry Authors")
    print("# Yaesu System Fusion vectors from tools/ysf_ref.py (do not edit; regenerate with: tools/ysf_ref.py vectors),")
    print("# read by tests/host/ysfTest.c. Bit strings are packed most significant bit first; numbers are hex.")
    print("#")
    print("# crc <bytes> <CRC as a number>")
    for s in (b"123456789", b"\x00\x00\x00\x00", b"N0CALL    "):
        print("crc %s %04X" % (hexs(s), bits_int(crc16(bits_of(s)))))
    print("#")
    print("# pn <first 511 whitening bits>")
    print("pn %s" % hexs(bytes_of(pn(511))))
    print("#")
    print("# golay <12 data bits> <24-bit code word>")
    for d in (0x000, 0x001, 0x800, 0xABC, 0x5A5, 0xFFF):
        print("golay %03X %06X" % (d, bits_int(golay_encode(int_bits(d, 12)))))
    print("#")
    print("# fich <32 bits> <200 coded bits as sent>")
    for f in (fich_pack(fi=0), fich_pack(fi=1, fn=3), fich_pack(fi=2, sc=42), fich_pack(fi=1, dt=3, fn=1, ft=6),
              0xFFFFFFFF & ~0x8000, 0):
        print("fich %08X %s" % (f, hexs(bytes_of(fich_encode(f)))))
    print("#")
    print("# dch <order: data | all> <10 or 20 bytes> <200 or 360 coded bits as sent>")
    for order in ("data", "all"):
        for d in (b"ALL       ", b"N0CALL    ", b"ALL       N0CALL    ", b" " * 20):
            print("dch %s %s %s" % (order, hexs(d), hexs(bytes_of(dch_encode(d, order)))))
    print("#")
    print("# vd2 <49 voice bits, packed> <104 bits as sent>")
    for v in VOICE_VECTORS:
        print("vd2 %s %s" % (hexs(bytes_of(v)), hexs(bytes_of(vd2_voice_encode(v)))))
    print("#")
    print("# tx <src> <dest> <headers> <V/D 2 frames> <the whole transmission's bits, 960 a frame>: voices are the")
    print("# host test's voice_pattern(n), n counting from 0")
    print("tx N0CALL ALL 1 7 %s" % hexs(bytes_of(transmission("N0CALL", "ALL", voice_pattern, frames=7))))
    print("#")
    print("# yaesu <src> <radio ID> <preamble symbols> <V/D 2 frames> <bits>: a Yaesu radio's DN over as received from")
    print("# one (yaesu_transmission: the layout real, the callsign and data made up), voices voice_pattern(n)")
    print("yaesu N0CALL E0123 240 9 %s" % hexs(bytes_of(yaesu_transmission("N0CALL", "E0123", voice_pattern))))


def self_test():
    fails = 0

    def check(cond, what):
        nonlocal fails
        if not cond:
            fails += 1
            print("FAIL", what)

    # CRC-16/GSM: the catalog's check value for "123456789"
    check(bits_int(crc16(bits_of(b"123456789"))) == 0xCE3C, "crc check value")
    # The whitening sequence: maximal length 511 with 256 ones, first bytes as the spec's figure gives them
    seq = pn(1022)
    check(seq[:511] == seq[511:], "pn period")
    check(sum(seq[:511]) == 256, "pn weight")
    check(bytes_of(seq[:64]) == bytes.fromhex("93D751219C2F6CD0"), "pn start %s" % hexs(bytes_of(seq[:64])))
    # The Golay code: distance 8, systematic
    words = [bits_int(golay_encode(int_bits(m, 12))) for m in range(4096)]
    check(min(bin(w).count("1") for w in words[1:]) == 8, "golay distance")
    check(all(w >> 12 == m for m, w in enumerate(words)), "golay systematic")
    for m in (0x123, 0xFED):
        w = golay_encode(int_bits(m, 12))
        for e in ((0,), (3, 17), (1, 12, 23)):
            bad = list(w)
            for i in e:
                bad[i] ^= 1
            check(bits_int(golay_decode(bad)) == m, "golay corrects %s" % (e,))
    # Units round trip through the soft decoders, clean and with errors
    for f in (fich_pack(fi=1, fn=4, sc=7), 0x12345678):
        air = fich_encode(f)
        check(len(air) == 200, "fich 200 bits")
        soft = [3 if b else -3 for b in air]
        for i in (5, 50, 120, 180):
            soft[i] = -soft[i]
        check(fich_decode(soft) == f, "fich round trip")
    for d, order in ((b"ALL       ", "data"), (b"N0CALL    G4XYZ     ", "all")):
        air = dch_encode(d, order)
        check(len(air) == (200 if len(d) == 10 else 360), "dch length")
        soft = [3 if b else -3 for b in air]
        soft[7] = -soft[7]
        got, o = dch_decode(soft, len(d))
        check(got == d and o == order, "dch round trip %s" % order)
    v = voice_pattern(3)
    air = vd2_voice_encode(v)
    soft = [3 if b else -3 for b in air]
    check(vd2_voice_decode(soft) == v, "vd2 round trip")
    check(len(transmission("N0CALL", frames=6)) == 960 * 8, "transmission length")
    y = yaesu_transmission("N0CALL", "E0123", frames=7)
    check(len(y) == 480 + 960 * 9, "yaesu transmission length")
    check(y[480:520] == SYNC and fich_decode([3 if b else -3 for b in y[520:720]]) == fich_pack(fi=0, cm=1, ft=6),
          "yaesu header after the preamble")
    # The pulse: unit energy and Nyquist when matched (integer symbol shifts of the pair are near zero)
    e = sum(rrc(i / 64) ** 2 for i in range(-8 * 64, 8 * 64 + 1)) / 64
    check(abs(e - 1) < 0.01, "rrc energy %f" % e)
    print("ysf_ref self-test: %s" % ("FAIL" if fails else "pass"))
    return 1 if fails else 0


# --- bench: signals and captures (numpy) ---


def rrc_np(t, a=0.2):
    """rrc() over an array: the same formula, its two removable points filled the same way."""
    import numpy as np
    t = np.asarray(t, dtype=float)
    out = np.empty_like(t)
    zero = np.abs(t) < 1e-12
    edge = np.abs(np.abs(t) - 1 / (4 * a)) < 1e-9
    rest = ~(zero | edge)
    tr = t[rest]
    out[rest] = (np.sin(np.pi * tr * (1 - a)) + 4 * a * tr * np.cos(np.pi * tr * (1 + a))) / \
        (np.pi * tr * (1 - (4 * a * tr) ** 2))
    out[zero] = rrc(0.0, a)
    out[edge] = rrc(1 / (4 * a), a)
    return out


def baseband(bits, fs, dev_hz):
    """The transmission's bits as instantaneous frequency (Hz) at fs: levels through the RRC pulse (+-6 symbols)."""
    import numpy as np
    lv = np.array([LEVEL[(bits[i], bits[i + 1])] for i in range(0, len(bits), 2)], dtype=float)
    sps = fs / 4800.0
    # Impulses at the symbol centers convolved with the pulse sampled at fs: the same sum, done at once
    n = int(len(lv) * sps + 13 * sps)
    imp = np.zeros(n)
    centers = np.round((np.arange(len(lv)) + 6.0) * sps).astype(int)
    imp[centers] = lv
    k = np.arange(-int(6 * sps), int(6 * sps) + 1)
    return np.convolve(imp, rrc_np(k / sps), mode="same") * dev_hz


def synth(args):
    import numpy as np
    bits = transmission(args.src, args.dest, voice_pattern, frames=args.frames, headers=args.headers,
                        order=args.order, dgid=args.dgid)
    fs = args.rate
    lead = np.zeros(int(0.2 * fs))
    f = np.concatenate([lead, baseband(bits, fs, args.dev), lead])
    if args.audio:
        # Discriminator audio: a 16-bit WAV at fs, full scale = 2 x the outer deviation
        import wave
        pcm = np.clip(f / (2 * 3 * args.dev) * 32767, -32768, 32767).astype("<i2")
        with wave.open(args.out, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(int(fs))
            w.writeframes(pcm.tobytes())
    else:
        ph = 2 * np.pi * np.cumsum(f + args.offset) / fs
        iq = np.exp(1j * ph) * args.amp
        # cs8 for rtl_sdr captures and hackrf_transfer; cs16 for SDRs that take 16-bit I/Q (a PlutoSDR through
        # SoapySDR or iio tools)
        full = 32767 if getattr(args, "format", "cs8") == "cs16" else 127
        raw = np.empty(2 * len(iq), dtype=np.int16 if full > 127 else np.int8)
        raw[0::2] = np.clip(np.round(iq.real * full), -full, full)
        raw[1::2] = np.clip(np.round(iq.imag * full), -full, full)
        raw.tofile(args.out)
    print("synth: %s, %d frames, %.2f s at %d Hz" % (args.out, len(bits) // 960, len(f) / fs, fs))


def demod_symbols(disc, fs, sps_ppm=0.0):
    """Discriminator (Hz) -> RRC matched filter -> the best of 20 sampling phases per frame-sized window."""
    import numpy as np
    sps = fs / 4800.0 * (1 + sps_ppm * 1e-6)
    taps = np.array([rrc(i / sps) for i in np.arange(-6 * sps, 6 * sps + 1)]) / sps
    mf = np.convolve(disc, taps, mode="same")
    return mf, sps


def decode_records(capture, offset=0.0, fmt="cs8", rate=None, bw=12.5, dev=900.0, threshold=0.8, voice=False):
    """Decode a capture into one dict per frame sync found: time, sync bit errors, polarity, the inner and outer levels
    in Hz and their DC, the FICH fields (None when its CRC failed), and the callsign units or DCH it carried."""
    import numpy as np
    if capture.endswith(".wav"):
        import wave
        with wave.open(capture) as w:
            fs = float(w.getframerate())
            x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float)
        disc = x / 32767 * (2 * 3 * dev)  # synth --audio's scale; a radio's tap needs --dev to match its level
    else:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import sdr_fm_check as fm
        fm.set_format(fmt)
        if rate:
            fm.set_rate(rate)
        chunk = int(fm.FS_IN / 4) // (fm.D1 * fm.D2 * fm.D3) * (fm.D1 * fm.D2 * fm.D3)    # 0.25 s, whole outputs
        disc = np.asarray(fm.demodulate(capture, offset, bw, chunk)[0], dtype=float)
        fs = 16000.0    # sdr_fm_check's discriminator output rate
    if fs < 40000:
        # Three samples a symbol and a bit are too few to sample the eye between them by straight lines (it read 4 of
        # the sync's 40 bits wrong on a clean signal): up to 48 kHz first
        from scipy.signal import resample_poly
        up = int(round(48000 / fs))
        disc = resample_poly(disc, up, 1)
        fs = fs * up
    mf, sps = demod_symbols(disc, fs)
    sync_lv = np.array([LEVEL[(SYNC[i], SYNC[i + 1])] for i in range(0, 40, 2)], dtype=float)
    # The sync's levels correlated against the matched filter at every sample, normalized: 1 is a perfect match
    idx = (np.arange(20) * sps).astype(int)
    span = int(20 * sps) + 1
    corr = np.zeros(len(mf))
    ref = sync_lv / np.sqrt(np.sum(sync_lv ** 2))
    for i in range(0, len(mf) - span):
        seg = mf[i + idx]
        corr[i] = np.dot(seg, ref) / (np.sqrt(np.sum(seg * seg)) + 1e-9)
    out = []
    i = 0
    n = int(480 * sps)
    while i < len(corr):
        if abs(corr[i]) <= threshold:
            i += 1
            continue
        j = i + int(np.argmax(np.abs(corr[i:i + int(2 * sps)])))
        if j + n >= len(mf):
            break
        sign = 1.0 if corr[j] > 0 else -1.0
        lv = sign * np.interp(j + np.arange(480) * sps, np.arange(len(mf)), mf)
        # The DC and the +1 level from the sync's 20 known levels: a least-squares line through them, which the
        # payload's own imbalance can't move
        unit, dc = np.polyfit(sync_lv, lv[:20], 1)
        y = (lv - dc) / unit
        bits, soft = [], []
        for v in y:
            bits += [1 if v < 0 else 0, 1 if abs(v) > 2 else 0]
            soft += [-v, abs(v) - 2]
        a = np.abs(lv - dc)
        rec = {"t": j / fs, "sync_errors": sum(x != b for x, b in zip(bits[:40], SYNC)), "inverted": sign < 0,
               "inner_hz": float(np.mean(a[np.abs(y) <= 2])), "outer_hz": float(np.mean(a[np.abs(y) > 2])),
               "dc_hz": float(dc), "fich": None}
        fich = fich_decode(soft[40:240])
        if fich is not None:
            f = {"raw": fich, "fi": fich >> 30 & 3, "fn": fich >> 19 & 7, "ft": fich >> 16 & 7, "dt": fich >> 8 & 3,
                 "dev": fich >> 14 & 1, "mr": fich >> 11 & 7, "sql": fich >> 7 & 1, "sc": fich & 0x7F,
                 "cm": fich >> 26 & 3}
            rec["fich"] = f
            pay = soft[240:]
            if f["fi"] in (0, 2):
                u1, u2 = [], []
                for k in range(5):
                    u1 += pay[144 * k:144 * k + 72]
                    u2 += pay[144 * k + 72:144 * k + 144]
                rec["csd1"], rec["csd1_order"] = dch_decode(u1, 20)
                rec["csd2"], rec["csd2_order"] = dch_decode(u2, 20)
            elif f["dt"] == 2:
                u = []
                for k in range(5):
                    u += pay[144 * k:144 * k + 40]
                rec["dch"], rec["dch_order"] = dch_decode(u, 10)
                if voice:
                    rec["voice"] = [bytes_of(vd2_voice_decode(pay[144 * k + 40:144 * k + 144])).hex() for k in range(5)]
        out.append(rec)
        i = j + int(470 * sps)
    return out


DT_NAMES = ["VD1", "data FR", "VD2 (DN)", "voice FR (VW)"]


def describe(rec):
    """A record as the decode command prints it."""
    line = "t=%.3f s sync errors %d%s, levels inner %.0f outer %.0f Hz (ratio %.2f), dc %+.0f Hz" % (
        rec["t"], rec["sync_errors"], " (inverted)" if rec["inverted"] else "", rec["inner_hz"], rec["outer_hz"],
        rec["outer_hz"] / max(rec["inner_hz"], 1e-9), rec["dc_hz"])
    f = rec["fich"]
    if f is None:
        return [line + ", FICH bad"]
    lines = [line + ", FICH %08X %s fn %d/%d dt %s dev %s sql %d sc %d" % (
        f["raw"], ["HC", "CC", "TC", "test"][f["fi"]], f["fn"], f["ft"], DT_NAMES[f["dt"]],
        "narrow" if f["dev"] else "wide", f["sql"], f["sc"])]
    for key, name in (("csd1", "CSD1 dest/src"), ("csd2", "CSD2 down/up"), ("dch", "DCH fn %d" % f["fn"])):
        if key in rec:
            d = rec[key]
            lines.append("    %s: %s" % (name, "CRC bad" if d is None else "%r (%s order)" % (d, rec[key + "_order"])))
    for v in rec.get("voice", []):
        lines.append("    voice %s" % v.upper())
    return lines


def decode(args):
    recs = decode_records(args.capture, args.offset, args.format, args.rate, args.bw, args.dev, args.threshold,
                          args.voice)
    for r in recs:
        for line in describe(r):
            print(line)
    print("decode: %d frames" % len(recs))
    return 0 if recs else 1


def main():
    if sys.argv[1:] == ["vectors"]:
        vectors()
        return 0
    if sys.argv[1:] == ["rrc"]:
        v = rrc_table()
        for i in range(0, len(v), 12):
            print("\t" + " ".join("%d," % x for x in v[i:i + 12]))
        return 0
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd")
    s = sub.add_parser("synth", help="a DN transmission as CS8 I/Q (default) or a WAV of discriminator audio")
    s.add_argument("out")
    s.add_argument("--src", default="N0CALL")
    s.add_argument("--dest", default="ALL")
    s.add_argument("--frames", type=int, default=30, help="V/D mode 2 frames (100 ms each)")
    s.add_argument("--headers", type=int, default=1)
    s.add_argument("--dgid", type=int, default=0, help="the FICH squelch code (DG-ID)")
    s.add_argument("--order", choices=("data", "all"), default="data", help="DCH whitening order (ysf_fec.h)")
    s.add_argument("--rate", type=int, default=1536000)
    s.add_argument("--dev", type=float, default=900.0, help="Hz of a +1 symbol (900 wide, 450 narrow)")
    s.add_argument("--offset", type=float, default=0.0, help="carrier offset from the center, Hz")
    s.add_argument("--amp", type=float, default=0.7, help="I/Q amplitude, 0-1 of full scale")
    s.add_argument("--audio", action="store_true", help="write discriminator audio (WAV) instead of I/Q")
    s.add_argument("--format", choices=("cs8", "cs16"), default="cs8", help="I/Q samples: 8 or 16 bit")
    d = sub.add_parser("decode", help="decode a capture: CS8 I/Q (as sdr_fm_check.py reads) or a WAV of audio")
    d.add_argument("capture")
    d.add_argument("--offset", type=float, default=0.0, help="channel frequency minus tuner center, Hz")
    d.add_argument("--rate", type=float, default=None, help="I/Q sample rate (default: sdr_fm_check's)")
    d.add_argument("--format", default="cs8", help="I/Q format: cs8 or cs16 (as sdr_fm_check.py)")
    d.add_argument("--bw", type=float, default=12.5)
    d.add_argument("--dev", type=float, default=900.0, help="WAV input: Hz of a +1 symbol at synth's scale")
    d.add_argument("--threshold", type=float, default=0.8, help="sync correlation to accept (0-1)")
    d.add_argument("--voice", action="store_true", help="print each 49-bit voice frame")
    a = ap.parse_args()
    if a.cmd == "synth":
        synth(a)
        return 0
    if a.cmd == "decode":
        return decode(a)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
