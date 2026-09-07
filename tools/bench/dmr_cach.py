#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""dmr_cach: the DMR CACH and short LC coding (ETSI TS 102 361-1 6.3, 9.1.4, B.3.5, B.3.7, B.4.1), the Python twin of
core/dmr/fec/dmrCach.c: TACT Hamming(7,4,3), the CACH interleave of figure B.9, the short LC's CRC-8 and its BPTC of
three Hamming(17,12,3) rows plus a column parity row sent by columns (figure B.6), and a decoder that assembles short
LCs from successive CACHs (LCSS 1, 3, 3, 2). Used by split_duplex_host.py (DMR_SHORTLC frames) and sdrmon.py
analyze --cach (the on-air check of the split duplex TX half, docs/spec/split-duplex.md 10.1).

    dmr_cach.py selftest                          the standard's vectors and round trips
    dmr_cach.py decode BITS24_HEX [...]           TACT and payload of CACHs (24 bits each, TX(23) first)
    dmr_cach.py slc ACT1 ACT2 TG1 TG2             the 9-byte DMR_SHORTLC of an Act_Updt (activity, group IDs)
"""
import sys

TACT_POS = (23, 19, 15, 11, 9, 5, 1)            # AT, TC, LS1, LS0, H2, H1, H0 in TX(23..0)
PAYLOAD_POS = (22, 21, 20, 18, 17, 16, 14, 13, 12, 10, 8, 7, 6, 4, 3, 2, 0)   # P16 .. P0
LCSS_FIRST, LCSS_LAST, LCSS_CONT = 1, 2, 3
SLCO_NULL, SLCO_ACTIVITY = 0, 1


def poly_parity(data, bits, g, r):
    reg = 0
    for i in range(bits - 1, -1, -1):
        fb = ((data >> i) & 1) ^ ((reg >> (r - 1)) & 1)
        reg = (reg << 1) & ((1 << r) - 1)
        if fb:
            reg ^= g & ((1 << r) - 1)
    return reg


def tact_encode(at, tc, lcss):
    d = ((at & 1) << 3) | ((tc & 1) << 2) | (lcss & 3)
    return (d << 3) | poly_parity(d, 4, 0x0B, 3)


def tact_decode(code):
    """(at, tc, lcss, errors fixed) or None when no single-bit fix gives a codeword"""
    code &= 0x7F
    for flip in (0,) + tuple(1 << b for b in range(7)):
        c = code ^ flip
        if poly_parity(c >> 3, 4, 0x0B, 3) == (c & 7):
            return (c >> 6) & 1, (c >> 5) & 1, (c >> 3) & 3, int(flip != 0)
    return None


def interleave(tact, payload):
    out = 0
    for i, pos in enumerate(TACT_POS):
        if (tact >> (6 - i)) & 1:
            out |= 1 << pos
    for i, pos in enumerate(PAYLOAD_POS):
        if (payload >> (16 - i)) & 1:
            out |= 1 << pos
    return out


def deinterleave(bits):
    t = 0
    p = 0
    for pos in TACT_POS:
        t = (t << 1) | ((bits >> pos) & 1)
    for pos in PAYLOAD_POS:
        p = (p << 1) | ((bits >> pos) & 1)
    return t, p


def cach_build(at, tc, lcss, payload):
    return interleave(tact_encode(at, tc, lcss), payload)


def h1712(data12):
    data12 &= 0xFFF
    return (data12 << 5) | poly_parity(data12, 12, 0x05, 5)


def h1712_correct(cw):
    cw &= 0x1FFFF
    for flip in (0,) + tuple(1 << b for b in range(17)):
        c = cw ^ flip
        if h1712(c >> 5) == c:
            return c
    return None


def crc8(msg, bits):
    crc = 0
    for i in range(bits - 1, -1, -1):
        fb = ((msg >> i) & 1) ^ (crc >> 7)
        crc = (crc << 1) & 0xFF
        if fb:
            crc ^= 0x07
    return crc


def slc_crc(slc):
    return crc8(slc & 0x0FFFFFFF, 28)


def slc_hash(address):
    return crc8(address & 0xFFFFFF, 24)


def slc_activity(ts1, ts2, hash1, hash2):
    return (SLCO_ACTIVITY << 24) | ((ts1 & 15) << 20) | ((ts2 & 15) << 16) | ((hash1 & 0xFF) << 8) | (hash2 & 0xFF)


def slc_encode(slc):
    """The four 17-bit CACH payloads (row 1 first) of a 28-bit short LC"""
    allbits = ((slc & 0x0FFFFFFF) << 8) | slc_crc(slc)
    enc = [h1712((allbits >> 24) & 0xFFF), h1712((allbits >> 12) & 0xFFF), h1712(allbits & 0xFFF)]
    enc.append(enc[0] ^ enc[1] ^ enc[2])
    rows = [0, 0, 0, 0]
    for i in range(68):
        bit = (enc[i % 4] >> (16 - i // 4)) & 1
        rows[i // 17] |= bit << (16 - i % 17)
    return rows


def slc_decode(rows):
    """(slc, ok) from four 17-bit payloads"""
    enc = [0, 0, 0, 0]
    for i in range(68):
        bit = (rows[i // 17] >> (16 - i % 17)) & 1
        enc[i % 4] |= bit << (16 - i // 4)
    ok = True
    for r in range(3):
        c = h1712_correct(enc[r])
        if c is None:
            ok = False
        else:
            enc[r] = c
    ok = ok and ((enc[0] ^ enc[1] ^ enc[2] ^ enc[3]) & 0x1FFFF) == 0
    allbits = ((enc[0] >> 5) << 24) | ((enc[1] >> 5) << 12) | (enc[2] >> 5)
    s = (allbits >> 8) & 0x0FFFFFFF
    return s, ok and slc_crc(s) == (allbits & 0xFF)


def slc_pack(rows):
    """DMR_SHORTLC's 9 bytes (split-duplex.md 5.3): the 68 bits row 1 first, MSB first, 4 pad bits"""
    out = bytearray(9)
    for i in range(68):
        if (rows[i // 17] >> (16 - i % 17)) & 1:
            out[i // 8] |= 0x80 >> (i % 8)
    return bytes(out)


def slc_unpack(b):
    rows = [0, 0, 0, 0]
    for i in range(68):
        if b[i // 8] & (0x80 >> (i % 8)):
            rows[i // 17] |= 1 << (16 - i % 17)
    return rows


def shortlc_frame_payload(slc):
    return slc_pack(slc_encode(slc))


def bits_to_int(bits):
    v = 0
    for b in bits:
        v = (v << 1) | (int(b) & 1)
    return v


def describe_slc(slc):
    slco = (slc >> 24) & 15
    if slco == SLCO_NULL:
        return "null"
    if slco == SLCO_ACTIVITY:
        return (f"activity ts1 {(slc >> 20) & 15} ts2 {(slc >> 16) & 15} hash1 {(slc >> 8) & 0xFF:02X} "
                f"hash2 {slc & 0xFF:02X}")
    return f"slco {slco} data {slc & 0xFFFFFF:06X}"


class CachDecoder:
    """Successive CACHs in air order -> per-CACH decode and the short LCs assembled from LCSS 1, 3, 3, 2"""

    def __init__(self):
        self.frag = None
        self.slcs = []          # (index, slc, ok)
        self.n = 0

    def push(self, bits24):
        """dict with tact ok, at, tc, lcss, payload, and slc when this CACH completed one"""
        tact, payload = deinterleave(bits24)
        d = tact_decode(tact)
        out = {"ok": d is not None, "payload": payload}
        self.n += 1
        if d is None:
            self.frag = None
            return out
        at, tc, lcss, fixed = d
        out.update(at=at, tc=tc, lcss=lcss, fixed=fixed)
        if lcss == LCSS_FIRST:
            self.frag = [payload]
        elif self.frag is not None and lcss == LCSS_CONT and len(self.frag) in (1, 2):
            self.frag.append(payload)
        elif self.frag is not None and lcss == LCSS_LAST and len(self.frag) == 3:
            self.frag.append(payload)
            slc, ok = slc_decode(self.frag)
            self.slcs.append((self.n, slc, ok))
            out["slc"] = (slc, ok)
            self.frag = None
        else:
            self.frag = None
        return out


def selftest():
    fails = 0

    def check(cond, what):
        nonlocal fails
        if not cond:
            print("FAIL", what)
            fails += 1
    # Table B.17 (Hamming(7,4,3) as the TACT code): every data word round trips and corrects any single bit
    for d in range(16):
        c = tact_encode(d >> 3, (d >> 2) & 1, d & 3)
        for b in range(8):
            r = tact_decode(c ^ ((1 << b) >> 1 if b else 0))
            check(r is not None and (r[0] << 3 | r[1] << 2 | r[2]) == d, f"tact {d} bit {b}")
    # The idle-channel short LC (null) and an activity update round trip through BPTC, pack and the decoder
    for slc in (0, slc_activity(8, 0, slc_hash(9), 0), slc_activity(8, 9, slc_hash(91), slc_hash(1234567))):
        rows = slc_encode(slc)
        check(slc_decode(rows) == (slc, True), f"slc {slc:07X}")
        check(slc_unpack(slc_pack(rows)) == rows, "pack")
        dec = CachDecoder()
        got = None
        for i, lcss in enumerate((LCSS_FIRST, LCSS_CONT, LCSS_CONT, LCSS_LAST)):
            r = dec.push(cach_build(1, i & 1, lcss, rows[i]))
            got = r.get("slc", got)
        check(got == (slc, True), f"decoder {slc:07X}")
        bad = list(rows)
        bad[1] ^= 1 << 7
        check(slc_decode(bad) == (slc, True), "one bit error corrected")
    check(deinterleave(interleave(0x55, 0x1ABCD)) == (0x55, 0x1ABCD), "interleave")
    print(f"# dmr_cach selftest: {'FAILED' if fails else 'PASSED'}")
    return 1 if fails else 0


def main(argv):
    if not argv or argv[0] == "selftest":
        return selftest()
    if argv[0] == "decode":
        dec = CachDecoder()
        for h in argv[1:]:
            r = dec.push(int(h, 16))
            if not r["ok"]:
                print(f"{h}: tact bad")
                continue
            line = f"{h}: tact ok AT {r['at']} TC {r['tc']} LCSS {r['lcss']} payload {r['payload']:05X}"
            if "slc" in r:
                line += f" slc {describe_slc(r['slc'][0])} {'ok' if r['slc'][1] else 'bad'}"
            print(line)
        return 0
    if argv[0] == "slc":
        a1, a2, g1, g2 = (int(x, 0) for x in argv[1:5])
        print(shortlc_frame_payload(slc_activity(a1, a2, slc_hash(g1) if g1 else 0, slc_hash(g2) if g2 else 0)).hex())
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
