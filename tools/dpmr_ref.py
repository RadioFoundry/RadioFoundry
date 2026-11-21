#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Independent reference for dPMR coding (core/dpmr), written from ETSI TS 102 490 V1.7.1 rather than from the C, so
the two agreeing is a real cross-check of the bit layout. Bits are lists of 0/1, the first sent first.

It emits the frame syncs, the CRC check values, coded header information, control channel and end blocks for fixed
fields, the channel codes, and whole frames as their symbols (a header with its preamble, the four payload frames of
a superframe, an end frame); tests/vectors/dpmr.txt is this output, and the C test (tests/host/dpmrTest.c)
reproduces each line with the public core/dpmr functions, then receives the frames through the shared framer.

The self-test also holds this reference to blocks a real dPMR446 radio sent: coded header information, control
channel and end bits read off the air from a public recording (the Signal ID Wiki's dPMR example), which this encoder
must reproduce bit for bit from the fields they decode to. Those blocks are what settled the details clause 7 leaves
open (the CRC presets, which scrambler stage is the output, the Hamming bit order): see core/dpmr/dpmr_fec.h. Only
these few coded blocks are kept, as the air* lines: no samples and no voice.

    tools/dpmr_ref.py vectors       print the vector file
    tools/dpmr_ref.py --self-test   check this reference against the off-air blocks and known-answer values
"""
import sys

FS1 = 0x57FF5F75D577
FS2 = 0x5FF77D
FS3 = 0x7DDFF5
FS4 = 0xFD55F5DF7FDD

# Table 7.1, the generator matrix's parity columns C3..C0 for data bits X7..X1, 1
HAMMING_ROWS = [0b1110, 0b0111, 0b1010, 0b0101, 0b1011, 0b1100, 0b0110, 0b0011]


def int_bits(value, n):
    return [(value >> (n - 1 - i)) & 1 for i in range(n)]


def bits_int(bits):
    v = 0
    for b in bits:
        v = (v << 1) | b
    return v


def bits_hex(bits):
    return "%0*x" % ((len(bits) + 3) // 4, bits_int(bits))


def hex_bits(text, n):
    return int_bits(int(text, 16), n)


def crc(bits, poly, width):
    """A CRC as a shift register preset to zero, the message most significant bit first."""
    reg = 0
    for b in bits:
        fb = ((reg >> (width - 1)) & 1) ^ b
        reg = (reg << 1) & ((1 << width) - 1)
        if fb:
            reg ^= poly
    return reg


def crc7(bits):
    return crc(bits, 0b0001001, 7)      # x^7 + x^3 + 1


def crc8(bits):
    return crc(bits, 0b00000111, 8)     # x^8 + x^2 + x + 1


def hamming(byte_bits):
    parity = 0
    for i, b in enumerate(byte_bits):
        if b:
            parity ^= HAMMING_ROWS[i]
    return byte_bits + int_bits(parity, 4)


def scramble(bits):
    """x^9 + x^5 + 1, all-ones preset, each block from the start: the register's last stage (S9) is the sequence."""
    s = [1] * 9                         # s[0] = S1 .. s[8] = S9
    out = []
    for b in bits:
        seq = s[8]
        fb = s[8] ^ s[4]
        s = [fb] + s[:8]
        out.append(b ^ seq)
    return out


def interleave(bits, cols):
    """Written down the 12 rows column by column, read along the rows (7.5)."""
    rows = 12
    matrix = [[bits[c * rows + r] for c in range(cols)] for r in range(rows)]
    return [b for row in matrix for b in row]


def block(data_bits, cols):
    words = []
    for i in range(0, len(data_bits), 8):
        words += hamming(data_bits[i:i + 8])
    return scramble(interleave(words, cols))


def hi_fields(ht, called, own, mode, version, fmt, ci):
    return (int_bits(ht, 4) + int_bits(called, 24) + int_bits(own, 24) + int_bits(mode, 3) + int_bits(version, 2) +
            int_bits(fmt, 2) + [0, 0] + int_bits(ci, 11))


def hi_coded(*fields):
    data = hi_fields(*fields)
    assert len(data) == 72
    return block(data + int_bits(crc8(data), 8), 10)


def cch_fields(fn, id12, mode, version, fmt, sld):
    return (int_bits(fn, 2) + int_bits(id12, 12) + int_bits(mode, 3) + int_bits(version, 2) + int_bits(fmt, 2) +
            [0, 0] + int_bits(sld, 18))


def cch_coded(*fields):
    data = cch_fields(*fields)
    assert len(data) == 41
    return block(data + int_bits(crc7(data), 7), 6)


def end_coded(et, arq, wait, status):
    data = int_bits(et, 2) + int_bits(arq, 2) + int_bits(wait, 4) + int_bits(status, 5) + [0] * 4
    data += int_bits(crc7(data), 7)
    words = []
    for i in range(0, 24, 8):
        words += hamming(data[i:i + 8])
    # Repeated, then the 72 bits scrambled as one block: 7.8's words. 9.6 and figure 11 scramble each copy on its own,
    # but a real radio's end frame follows 7.8 (AIR_END)
    return scramble(words + words)


def cc_air(code12):
    out = []
    for b in int_bits(code12, 12):
        out += [1, 1] if b else [0, 1]
    return out


# Fixed fields for the vectors: arbitrary numbers, not anyone's identity
VEC_HI = (0, 0x123456, 0x0ABCDE, 0, 0, 1, 0)
VEC_CCH = [(fn, (0x123, 0x456, 0x0AB, 0xCDE)[fn], 0, 0, 1, 0) for fn in range(4)]
VEC_END = (0, 0, 0, 0)
VEC_CC = [0x145, 0x1A4, 0x529, 0xD11, 0xEE5]

# Off the air (the Signal ID Wiki dPMR446 recording, its levels turned: the recording's spectrum is inverted). An
# initial-services radio on common ID 0x57 (16 ones follow it, 8.1.1.2), peer to peer, channel code 0x1A4 (446.109375)
AIR_HI = [
    # an extended wake-up header (CI type 111, one more to come) and the last, normal one
    ("c9dc48e6cb653197343a52b85a6f82", (0, 0x57FFFF, 0x57FFFF, 0, 0, 1, 0x701)),
    ("c9dc58e2cb6571c7243c53f85a6384", (0, 0x57FFFF, 0x57FFFF, 0, 0, 1, 0x000)),
]
AIR_CCH = [
    ("be87ee743268eedbff", (0, 0x57F, 0, 0, 1, 0)),
    ("bc9feff43a68eedbbf", (1, 0xFFF, 0, 0, 1, 0)),
    ("3e87af7032286cd3bf", (2, 0x57F, 0, 0, 1, 0)),
    ("3c9faef03a286cd3ff", (3, 0xFFF, 0, 0, 1, 0)),
]
AIR_CC = ("57dd75", 0x1A4)
AIR_END = ("feb3df01c21a4ed088", (0, 0, 1, 0))    # a normal end, Tx wait 40 ms


PREAMBLE = int_bits(0x5F, 8) * 9                     # 6.1.6: 72 bits of +3 +3 -3 -3
VEC_FRAME_CC = 0x392                                 # a group A code two symbols from FS2: the receiver must tell them


def tch(fn, k):
    """A made-up 72-bit voice frame per place in the superframe: the framing carries it as it is."""
    v = 0
    for i in range(9):
        v = (v << 8) | ((fn * 64 + k * 16 + i * 29 + 7) & 0xFF)
    return int_bits(v, 72)


def header_frame(fields, code):
    """4.2.3, figure 5: preamble, FS1, HI0, CC, HI1 (384 bits)."""
    hi = hi_coded(*fields)
    return PREAMBLE + int_bits(FS1, 48) + hi + cc_air(code) + hi


def payload_frame(fn, called, own, code):
    """FS2 (frames 1, 3) or the channel code (2, 4), the CCH, four TCH (384 bits)."""
    ident = called if fn < 2 else own
    id12 = (ident >> 12) & 0xFFF if fn % 2 == 0 else ident & 0xFFF
    head = int_bits(FS2, 24) if fn % 2 == 0 else cc_air(code)
    out = head + cch_coded(fn, id12, 0, 0, 1, 0)
    for k in range(4):
        out += tch(fn, k)
    return out


def end_frame():
    return int_bits(FS3, 24) + end_coded(*VEC_END)


def vectors():
    lines = ["# SPDX-License-Identifier: MIT", "# Copyright (c) 2026 The RadioFoundry Authors",
             "# dPMR vectors from tools/dpmr_ref.py (ETSI TS 102 490), read by tests/host/dpmrTest.c. Regenerate:",
             "# tools/dpmr_ref.py vectors > tests/vectors/dpmr.txt",
             "fs1 %012x" % FS1, "fs2 %06x" % FS2, "fs3 %06x" % FS3, "fs4 %012x" % FS4]
    probe = int_bits(0x313233343536373839, 72)          # "123456789"
    lines.append("crc7 313233343536373839 %02x" % crc7(probe))
    lines.append("crc8 313233343536373839 %02x" % crc8(probe))
    for b in (0x00, 0x01, 0x80, 0xA5, 0xFF):
        lines.append("ham %02x %03x" % (b, bits_int(hamming(int_bits(b, 8)))))
    lines.append("hi %x %06x %06x %x %x %x %03x %s" % (VEC_HI + (bits_hex(hi_coded(*VEC_HI)),)))
    for c in VEC_CCH:
        lines.append("cch %x %03x %x %x %x %05x %s" % (c + (bits_hex(cch_coded(*c)),)))
    lines.append("end %x %x %x %x %s" % (VEC_END + (bits_hex(end_coded(*VEC_END)),)))
    for c in VEC_CC:
        lines.append("cc %03x %s" % (c, bits_hex(cc_air(c))))
    lines.append("frame hdr %03x %s" % (VEC_FRAME_CC, bits_hex(header_frame(VEC_HI, VEC_FRAME_CC))))
    for fn in range(4):
        lines.append("frame pay%d %03x %s" % (fn, VEC_FRAME_CC, bits_hex(payload_frame(fn, VEC_HI[1], VEC_HI[2],
                                                                                     VEC_FRAME_CC))))
    lines.append("frame end %03x %s" % (VEC_FRAME_CC, bits_hex(end_frame())))
    for air, f in AIR_HI:
        lines.append("airhi %s %x %06x %06x %x %x %x %03x" % ((air,) + f))
    for air, f in AIR_CCH:
        lines.append("aircch %s %x %03x %x %x %x %05x" % ((air,) + f))
    lines.append("airend %s %x %x %x %x" % ((AIR_END[0],) + AIR_END[1]))
    return "\n".join(lines) + "\n"


def self_test():
    assert crc8(int_bits(0x313233343536373839, 72)) == 0xF4, "CRC-8 (poly 0x07, zero preset) check value"
    for air, f in AIR_HI:
        assert bits_hex(hi_coded(*f)) == air, "header information not as a real radio sent it: %s" % air
    for air, f in AIR_CCH:
        assert bits_hex(cch_coded(*f)) == air, "control channel not as a real radio sent it: %s" % air
    assert bits_hex(cc_air(AIR_CC[1])) == AIR_CC[0], "channel code coding"
    assert bits_hex(end_coded(*AIR_END[1])) == AIR_END[0], "end data not as a real radio sent it"
    assert len(header_frame(VEC_HI, VEC_FRAME_CC)) == 384 and len(payload_frame(0, 1, 2, VEC_FRAME_CC)) == 384
    assert len(end_frame()) == 96
    print("dpmr_ref self-test: ok")


def main(argv):
    if "--self-test" in argv:
        self_test()
        return 0
    if "vectors" in argv:
        sys.stdout.write(vectors())
        return 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
