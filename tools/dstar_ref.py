#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""D-STAR DV reference (docs/spec/dstar-modem.md 3), written apart from core/dstar to check it.

Worked from the JARL D-STAR standard (STD 6.0, English edition, chapter 4.1, chapter 6 and appendices 1-2, and the
older English translation's appendix figures) and nothing else. It is deliberately written another way than the C:

  - bits are Python lists in air order, one int per bit, never packed
  - the CRC runs a plain MSB-first x^16 + x^12 + x^5 + 1 shift register over the bits in the order they go on air
    (least significant bit of each byte first), then bit-reverses the register: the same as the reflected 0x8408
    loop in C only if both are right
  - the convolutional code is the polynomial pair applied to a list with explicit zero history
  - the interleaver builds the standard's printed 24 x 28 matrix (entry r, c = 24 c + r; the last column only in rows
    0-11) and reads it row by row
  - the scrambler simulates the appendix 1.1 figure's seven registers one by one

    tools/dstar_ref.py vectors > tests/vectors/dstar.txt     write the vectors the host test reads
    tools/dstar_ref.py --self-test                           check the reference against itself and the CRC catalog
    tools/dstar_ref.py pulse                                 the GMSK pulse table of core/dstar/dstar_gmsk.c
"""
import sys

HEADER_BYTES = 41


def bits_of(data):
    """Bytes to air-order bits: each byte least significant bit first (appendix 1.4)."""
    return [(b >> i) & 1 for b in data for i in range(8)]


def bytes_of(bits):
    out = bytearray((len(bits) + 7) // 8)
    for i, b in enumerate(bits):
        if b:
            out[i // 8] |= 1 << (i % 8)
    return bytes(out)


def pattern(s):
    """A pattern as the standard prints it, left to right = first on air."""
    return [int(c) for c in s if c in "01"]


FRAME_SYNC = pattern("111011001010000")
RESYNC = pattern("1010101010" "1101000" "1101000")
END = pattern("10" * 16 + "000100110101111" + "0")


def crc_ccitt(data):
    """x^16 + x^12 + x^5 + 1 on the air-order bits, register preset to ones, result complemented and bit-reversed so
    the FCS's first bit on air is the register's top bit (ISO 3309 / X.25)."""
    reg = 0xFFFF
    for bit in bits_of(data):
        top = (reg >> 15) & 1
        reg = (reg << 1) & 0xFFFF
        if top ^ bit:
            reg ^= 0x1021
    reg ^= 0xFFFF
    rev = int(format(reg, "016b")[::-1], 2)
    return rev  # sent low byte first: its bit 0 is the first FCS bit on air


def header_bytes(flags, rpt2, rpt1, ur, my, sfx):
    def f(s, n):
        return s.encode("ascii")[:n].ljust(n, b" ")
    body = bytes(flags) + f(rpt2, 8) + f(rpt1, 8) + f(ur, 8) + f(my, 8) + f(sfx, 4)
    c = crc_ccitt(body)
    return body + bytes([c & 0xFF, c >> 8])


def convolve(bits):
    """Rate 1/2, K = 3: G1 = 1 + D + D^2 then G2 = 1 + D^2 per input bit, two zero tail bits (appendix 2.1)."""
    u = list(bits) + [0, 0]
    hist = [0, 0] + u  # hist[k + 2] = u[k], two zeros before
    out = []
    for k in range(len(u)):
        d0, d1, d2 = hist[k + 2], hist[k + 1], hist[k]
        out.append(d0 ^ d1 ^ d2)
        out.append(d0 ^ d2)
    return out


def interleave_matrix():
    """The standard's printed matrix: rows 0-23, columns 0-27; None where row >= 12 in column 27."""
    m = []
    for r in range(24):
        row = []
        for c in range(28):
            v = 24 * c + r
            row.append(v if v < 660 else None)
        m.append(row)
    return m


def interleave(coded):
    return [coded[v] for row in interleave_matrix() for v in row if v is not None]


def deinterleave(air):
    order = [v for row in interleave_matrix() for v in row if v is not None]
    coded = [0] * len(order)
    for j, v in enumerate(order):
        coded[v] = air[j]
    return coded


def scrambler(n):
    """The appendix 1.1 figure: registers R1..R7 (R1 nearest the data), each shift moves R1 -> R2 ... R6 -> R7; the
    feedback is R4 XOR R7, it enters R1 and is XORed with the data. All registers start at 1."""
    reg = [1] * 7  # reg[0] = R1
    seq = []
    for _ in range(n):
        fb = reg[3] ^ reg[6]
        seq.append(fb)
        reg = [fb] + reg[:6]
    return seq


def scramble(bits):
    return [b ^ s for b, s in zip(bits, scrambler(len(bits)))]


def encode_header(hdr):
    return scramble(interleave(convolve(bits_of(hdr))))


def data_frame(three):
    """A data frame as sent: its 24 bits XORed with the scrambler restarted for the frame."""
    return scramble(bits_of(three))


def slow_blocks_message(msg):
    msg = msg.encode("ascii")[:20].ljust(20, b" ")
    return [bytes([0x40 | i]) + msg[5 * i:5 * i + 5] for i in range(4)]


def slow_blocks_header(hdr):
    out = []
    for at in range(0, HEADER_BYTES, 5):
        part = hdr[at:at + 5]
        out.append((bytes([0x50 | len(part)]) + part).ljust(6, b"\x66"))
    return out


VECTORS = [
    # flags, RPT2, RPT1, UR, MY, suffix
    ((0x00, 0x00, 0x00), "DIRECT", "DIRECT", "CQCQCQ", "N0CALL", "TEST"),
    ((0x40, 0x00, 0x00), "XX1XX  G", "XX1XX  B", "CQCQCQ", "N0CALL", "ID51"),
    ((0x48, 0x00, 0x00), "XX1XX  G", "XX1XX  C", "/XX1XXA", "AB1CD  P", "5100"),
    ((0x07, 0x01, 0x02), "", "", "", "", ""),
]


def gmsk_pulse(bt=0.5, per_bit=64, span=3):
    """The frequency pulse of GMSK: a one-bit rectangle through a Gaussian filter of bandwidth BT / T, as the
    deviation it adds at time t (in bits) from a bit's center, sampled every 1/per_bit bit over span bits, both
    ends included (the modulator interpolates between neighbors), scaled so a long run of one value gives 32767."""
    import math
    sigma = math.sqrt(math.log(2)) / (2 * math.pi * bt)
    def g(t):
        return 0.5 * (math.erf((t + 0.5) / (math.sqrt(2) * sigma)) - math.erf((t - 0.5) / (math.sqrt(2) * sigma)))
    return [round(32767 * g(-span / 2 + i / per_bit)) for i in range(span * per_bit + 1)]


def hexs(b):
    return b.hex().upper()


def vectors():
    print("# SPDX-License-Identifier: MIT")
    print("# Copyright (c) 2026 The RadioFoundry Authors")
    print("# D-STAR DV vectors from tools/dstar_ref.py (do not edit; regenerate with: tools/dstar_ref.py vectors), read by")
    print("# tests/host/dstarTest.c. Bit strings are in air order packed least significant bit first; numbers are hex.")
    print("#")
    print("# crc <bytes> <FCS as a number>")
    for s in (b"123456789", b"", b"D-STAR"):
        print("crc %s %04X" % (hexs(s) or "-", crc_ccitt(s)))
    print("#")
    print("# pn <first 127 scrambler bits, air order>")
    print("pn %s" % hexs(bytes_of(scrambler(127))))
    print("#")
    print("# header <41 header bytes> <660 coded, interleaved, scrambled bits>")
    for v in VECTORS:
        h = header_bytes(*v)
        print("header %s %s" % (hexs(h), hexs(bytes_of(encode_header(h)))))
    print("#")
    print("# data <3 bytes> <the frame's 24 bits as sent>")
    for d in (b"\x66\x66\x66", b"\x00\x00\x00", b"\x40He", b"llo"):
        print("data %s %s" % (hexs(d), hexs(bytes_of(data_frame(d)))))
    print("#")
    print("# slowmsg <message, 20 chars, spaces as _> <four 6-byte blocks>")
    m = "Hello from RFoundry!"
    print("slowmsg %s %s" % (m.replace(" ", "_"), " ".join(hexs(b) for b in slow_blocks_message(m))))
    print("#")
    print("# slowhdr <41 header bytes> <nine 6-byte blocks>")
    h = header_bytes(*VECTORS[1])
    print("slowhdr %s %s" % (hexs(h), " ".join(hexs(b) for b in slow_blocks_header(h))))
    print("#")
    print("# patterns: frame sync, resync, end (air order)")
    print("sync %s %s %s" % (hexs(bytes_of(FRAME_SYNC)), hexs(bytes_of(RESYNC)), hexs(bytes_of(END))))


def self_test():
    fails = 0

    def check(cond, what):
        nonlocal fails
        if not cond:
            fails += 1
            print("FAIL", what)

    # CRC-16/X-25 catalogue check value
    check(crc_ccitt(b"123456789") == 0x906E, "crc check value %04X" % crc_ccitt(b"123456789"))
    # The sequence has period 127 and 64 ones (maximal length)
    seq = scrambler(254)
    check(seq[:127] == seq[127:], "scrambler period")
    check(sum(seq[:127]) == 64, "scrambler weight")
    check(len(set(v for row in interleave_matrix() for v in row if v is not None)) == 660, "interleaver covers 660")
    for v in VECTORS:
        h = header_bytes(*v)
        air = encode_header(h)
        check(len(air) == 660, "660 bits")
        coded = deinterleave(scramble(air))
        check(coded == convolve(bits_of(h)), "round trip")
    print("dstar_ref self-test: %s" % ("FAIL" if fails else "pass"))
    return 1 if fails else 0


if __name__ == "__main__":
    if sys.argv[1:] == ["vectors"]:
        vectors()
    elif sys.argv[1:] == ["pulse"]:
        v = gmsk_pulse()
        for i in range(0, len(v), 16):
            print("\t" + " ".join("%d," % x for x in v[i:i + 16]))
    elif sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    else:
        print(__doc__)
        sys.exit(2)
