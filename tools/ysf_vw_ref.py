#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Yaesu System Fusion Voice FR (VW) reference (docs/spec/ysf-modem.md 4.1), written apart from core/ysf/ysf_vw.c to
check it: the full-rate voice frame as P25 Phase 1 codes it (TIA-102.BAAA 5.3) and a whole VW transmission as
core/ysf sends one.

    tools/ysf_vw_ref.py vectors       the host test's vectors (tests/vectors/ysf-vw.txt)
    tools/ysf_vw_ref.py --self-test

Written another way than the C: bit lists, the Golay code decoded from a syndrome table of every pattern of three
errors or fewer, the interleave as a list of (word, bit) pairs, the scrambler's sequence as a plain recurrence. The
FICH, DCH units and headers are tools/ysf_ref.py's.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ysf_ref as ref  # noqa: E402

# Golay (23,12): the (24,12) code's parity rows (TIA-102.BAAA table 5-3, the same matrix as Yaesu's appendix A)
# without their overall parity bit
G24 = (0xC75, 0x63B, 0xF68, 0x7B4, 0x3DA, 0xD99, 0x6CD, 0x367, 0xDC6, 0xA97, 0x93E, 0x8EB)
H15 = (0xF, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x7, 0x6, 0x5, 0x3)     # Hamming (15,11): each data bit's parity (table 5-4)


def golay23_encode(d):
    p = 0
    for j in range(12):
        if d >> (11 - j) & 1:
            p ^= G24[j] >> 1
    return d << 11 | p


SYNDROME = {}
for _a in range(23):
    for _b in range(_a, 23):
        for _c in range(_b, 23):
            _e = (1 << _a) | (1 << _b) | (1 << _c)
            SYNDROME.setdefault((_e & 0x7FF) ^ (golay23_encode(_e >> 11) & 0x7FF), _e)
SYNDROME[0] = 0


def golay23_decode(cw):
    e = SYNDROME[(cw & 0x7FF) ^ (golay23_encode(cw >> 11) & 0x7FF)]
    return (cw ^ e) >> 11, bin(e).count("1")


def hamming15_encode(d):
    p = 0
    for j in range(11):
        if d >> (10 - j) & 1:
            p ^= H15[j]
    return d << 4 | p


def hamming15_decode(cw):
    s = (cw & 0xF) ^ (hamming15_encode(cw >> 4) & 0xF)
    if s == 0:
        return cw >> 4, 0
    if s & (s - 1) == 0:
        return cw >> 4, 1
    return (cw ^ (0x10 << (10 - H15.index(s)))) >> 4, 1


# Table 5-1: air bit i comes from (code word, bit of it; bit 0 is the word's last)
INTERLEAVE = (
    (0, 22), (1, 21), (2, 20), (3, 19), (4, 10), (5, 1), (1, 20), (0, 21), (3, 18), (2, 19), (5, 0), (4, 9),
    (0, 20), (1, 19), (2, 18), (3, 17), (4, 8), (6, 14), (1, 18), (0, 19), (3, 16), (2, 17), (6, 13), (4, 7),
    (0, 18), (1, 17), (2, 16), (3, 15), (4, 6), (6, 12), (1, 16), (0, 17), (3, 14), (2, 15), (6, 11), (4, 5),
    (0, 16), (1, 15), (2, 14), (3, 13), (4, 4), (6, 10), (1, 14), (0, 15), (3, 12), (2, 13), (6, 9), (4, 3),
    (0, 14), (1, 13), (2, 12), (3, 11), (4, 2), (6, 8), (1, 12), (0, 13), (3, 10), (2, 11), (6, 7), (4, 1),
    (0, 12), (1, 11), (2, 10), (3, 9), (4, 0), (6, 6), (1, 10), (0, 11), (3, 8), (2, 9), (6, 5), (5, 14),
    (0, 10), (1, 9), (2, 8), (3, 7), (5, 13), (6, 4), (1, 8), (0, 9), (3, 6), (2, 7), (6, 3), (5, 12),
    (0, 8), (1, 7), (2, 6), (3, 5), (5, 11), (6, 2), (1, 6), (0, 7), (3, 4), (2, 5), (6, 1), (5, 10),
    (0, 6), (1, 5), (2, 4), (3, 3), (5, 9), (6, 0), (1, 4), (0, 5), (3, 2), (2, 3), (7, 6), (5, 8),
    (0, 4), (1, 3), (2, 2), (3, 1), (5, 7), (7, 5), (1, 2), (0, 3), (3, 0), (2, 1), (7, 4), (5, 6),
    (0, 2), (1, 1), (2, 0), (4, 14), (5, 5), (7, 3), (1, 0), (0, 1), (4, 13), (3, 22), (7, 2), (5, 4),
    (0, 0), (2, 22), (3, 21), (4, 12), (5, 3), (7, 1), (2, 21), (1, 22), (4, 11), (3, 20), (7, 0), (5, 2),
)
DATA_LEN = (12, 12, 12, 12, 11, 11, 11, 7)
CODE_LEN = (23, 23, 23, 23, 15, 15, 15, 7)


def masks(u0):
    """The scrambling of c1..c6 (5.3): p(0) = 16 u0, p(n) = 173 p(n-1) + 13849 mod 65536, bit 15 of each in turn."""
    seq, p = [], 16 * u0
    for _ in range(114):
        p = (173 * p + 13849) % 65536
        seq.append(p >> 15)
    out, o = [0], 0
    for n in CODE_LEN[1:7]:
        out.append(ref.bits_int(seq[o:o + n]))
        o += n
    return out + [0]


def frame_encode(bits88):
    u, o = [], 0
    for n in DATA_LEN:
        u.append(ref.bits_int(bits88[o:o + n]))
        o += n
    m = masks(u[0])
    c = [golay23_encode(u[0])] + [golay23_encode(u[k]) ^ m[k] for k in (1, 2, 3)] + \
        [hamming15_encode(u[k]) ^ m[k] for k in (4, 5, 6)] + [u[7]]
    return [c[w] >> b & 1 for w, b in INTERLEAVE]


def frame_decode(bits144):
    """(88 bits, total corrected, corrected in c0, corrected in c4)"""
    c = [0] * 8
    for i, (w, b) in enumerate(INTERLEAVE):
        c[w] |= bits144[i] << b
    u0, e0 = golay23_decode(c[0])
    m = masks(u0)
    u, total, e4 = [u0], e0, 0
    for k in (1, 2, 3):
        d, e = golay23_decode(c[k] ^ m[k])
        u.append(d)
        total += e
    for k in (4, 5, 6):
        d, e = hamming15_decode(c[k] ^ m[k])
        u.append(d)
        total += e
        if k == 4:
            e4 = e
    u.append(c[7])
    return sum((ref.int_bits(v, n) for v, n in zip(u, DATA_LEN)), []), total, e0, e4


def imbe_pattern(n):
    """Known pseudo-random 88-bit frames (the same rule as the host test's)."""
    x = (n * 2654435761 + 54321) & 0xFFFFFFFF
    bits = []
    for _ in range(88):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        bits.append(x & 1)
    return bits


def tx_fn(n):
    """FN, FT of the n-th communication frame: the FT 1 pair after the header, then FN 0 of FT 0."""
    return (1 if n == 1 else 0), (1 if n < 2 else 0)


def transmission(src, dest="ALL", rem=b" " * 20, voices=None, frames=4, headers=1, dgid=0, order="data", cm=0):
    """The bits of a VW transmission as core/ysf sends it: headers and terminator as DN's with DT 3 and FN 0 FT 0,
    then communication frames of five full-rate frames each, the first with CSD3 in place of its first three."""
    dest_b, src_b = ref.callsign(dest), ref.callsign(src)
    out = []
    for _ in range(headers):
        out += ref.frame_header(ref.fich_pack(fi=0, cm=cm, ft=0, dt=3, sc=dgid), dest_b, src_b, order=order)
    n = 0
    for f in range(frames):
        fn, ft = tx_fn(f)
        pay = []
        first = 0
        if f == 0:
            pay = ref.dch_encode(rem, order) + [0] * 72
            first = 3
        for _ in range(first, 5):
            pay += frame_encode(voices(n) if voices else [0] * 88)
            n += 1
        out += ref.SYNC + ref.fich_encode(ref.fich_pack(fi=1, cm=cm, fn=fn, ft=ft, dt=3, sc=dgid)) + pay
    out += ref.frame_header(ref.fich_pack(fi=2, cm=cm, ft=0, dt=3, sc=dgid), dest_b, src_b, order=order)
    return out


REM = b"               00123"     # CSD3 as the test sends it: no repeater, our radio ID last


def vectors():
    h = ref.hexs
    print("# SPDX-License-Identifier: MIT")
    print("# Copyright (c) 2026 The RadioFoundry Authors")
    print("# Yaesu System Fusion Voice FR (VW) vectors from tools/ysf_vw_ref.py (do not edit; regenerate with:")
    print("# tools/ysf_vw_ref.py vectors), read by tests/host/ysfVwTest.c. Bit strings are packed most significant bit")
    print("# first.")
    print("#")
    print("# frame <88 information bits> <144 coded bits as sent>")
    for b in ([0] * 88, [1] * 88, imbe_pattern(0), imbe_pattern(1), imbe_pattern(99)):
        print("frame %s %s" % (h(ref.bytes_of(b)), h(ref.bytes_of(frame_encode(b)))))
    print("#")
    print("# tx <src> <dest> <CSD3> <headers> <communication frames> <the whole transmission's bits, 960 a frame>:")
    print("# voices are the host test's imbe_pattern(n), n counting from 0")
    print("tx N0CALL ALL %s 1 4 %s" % (h(REM), h(ref.bytes_of(transmission("N0CALL", "ALL", REM, imbe_pattern,
                                                                           frames=4)))))


def self_test():
    fails = 0

    def check(cond, what):
        nonlocal fails
        if not cond:
            fails += 1
            print("FAIL", what)

    check(sorted(INTERLEAVE) == sorted((w, b) for w in range(8) for b in range(CODE_LEN[w])), "interleave covers")
    words = [golay23_encode(d) for d in range(4096)]
    check(min(bin(w).count("1") for w in words[1:]) == 7, "golay23 distance")
    check(len(SYNDROME) == 2048, "golay23 perfect")
    check(min(bin(hamming15_encode(d)).count("1") for d in range(1, 2048)) == 3, "hamming15 distance")
    for n in range(40):
        b = imbe_pattern(n)
        air = frame_encode(b)
        check(frame_decode(air) == (b, 0, 0, 0), "frame round trip %d" % n)
        bad = list(air)
        for w in range(7):
            pos = [i for i, (cw, _) in enumerate(INTERLEAVE) if cw == w][n % 5::7][:3 if w < 4 else 1]
            for i in pos:
                bad[i] ^= 1
        got = frame_decode(bad)
        check(got[0] == b and got[1] == sum(min(3, len([i for i, (cw, _) in enumerate(INTERLEAVE) if cw == w]
                                                         [n % 5::7][:3 if w < 4 else 1])) for w in range(7)),
              "frame corrects %d" % n)
    t = transmission("N0CALL", frames=3)
    check(len(t) == 960 * 5, "transmission length")
    # The first frame's CSD3 decodes from where it is put
    pay = t[960 + 240:960 * 2]
    got, order = ref.dch_decode([3 if b else -3 for b in pay[:360]], 20)
    check(got == b" " * 20 and order == "data", "csd3 unit")
    check(pay[360:432] == [0] * 72, "csd3 pad")
    print("ysf_vw_ref self-test: %s" % ("FAIL" if fails else "pass"))
    return 1 if fails else 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--self-test":
        return self_test()
    if len(sys.argv) > 1 and sys.argv[1] == "vectors":
        vectors()
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
