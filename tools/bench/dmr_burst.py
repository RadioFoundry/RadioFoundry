#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
DMR data bursts built on the test host, for a bench transmitter that sends 33-byte bursts as given (the bench line
protocol's bench_tx raw job): preamble CSBKs, data headers and rate 1/2 blocks, with the user data the test chose, encrypted or not.
The bench builds the usual packets itself; this is for what it has no job for (a privacy header, an encrypted packet,
a malformed one). Written from ETSI TS 102 361-1 (9.1, 9.2.6, B.1.1, B.3.1, B.3.4, B.3.5, B.3.8, B.3.9) and
TS 102 361-3 (5.6, 7.2); the privacy header is docs/spec/dmr-data.md 13. Pure Python: runs on the test host.

    import dmr_burst
    bursts = dmr_burst.data_bursts(dst, src, sap, user, cc=1, group=False, preambles=2, privacy=None)
    -> list of 33-byte bursts, sent back to back on one slot
"""

DT_CSBK, DT_DATA_HEADER, DT_RATE_12 = 3, 6, 7
DPF_UNCONFIRMED, DPF_PROPRIETARY = 2, 15
SAP_IP, SAP_PROPRIETARY = 4, 9
CSBKO_PREAMBLE = 0x3D
PRIVACY_MFID = 0x10
MS_DATA_SYNC = 0xD5D7F77FD757
TMS_PORT = 4007

# B.3.5 Hamming(15,11,3) and B.3.4 Hamming(13,9,3), by the syndrome each data bit gives (bit 3 = parity bit 0)
_H15113 = (0x9, 0xD, 0xF, 0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3)
_H1393 = (0xF, 0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3)
_GOLAY23 = 0xC75


def _hamming(data, columns):
    n = len(columns)
    parity = 0
    for i in range(n):
        if (data >> (n - 1 - i)) & 1:
            parity ^= columns[i]
    return (data << 4) | parity


def golay2087(data):
    """B.3.1: 8 data bits, the Golay(23,12) parity of them, then an overall parity bit (20 bits)."""
    r = data << 11
    for b in range(22, 10, -1):
        if r & (1 << b):
            r ^= _GOLAY23 << (b - 11)
    cw = (data << 11) | (r & 0x7FF)
    return (cw << 1) | (bin(cw).count("1") & 1)


def bptc19696(payload):
    """B.1.1: 12 payload octets into the 196 info bits (a list of 0/1, in the order sent)."""
    m = [0] * 196
    idx = lambda r, c: 1 + 15 * r + c
    for n in range(96):
        bit = (payload[n // 8] >> (7 - n % 8)) & 1
        m[idx(0, n + 3) if n < 8 else idx(1 + (n - 8) // 11, (n - 8) % 11)] = bit
    for r in range(9):
        row = 0
        for c in range(11):
            row = (row << 1) | m[idx(r, c)]
        cw = _hamming(row, _H15113)
        for c in range(15):
            m[idx(r, c)] = (cw >> (14 - c)) & 1
    for c in range(15):
        col = 0
        for r in range(9):
            col = (col << 1) | m[idx(r, c)]
        cw = _hamming(col, _H1393)
        for r in range(13):
            m[idx(r, c)] = (cw >> (12 - r)) & 1
    out = [0] * 196
    for k in range(196):
        out[(k * 181) % 196] = m[k]
    return out


def burst(payload, data_type, cc, sync=MS_DATA_SYNC):
    """One 33-byte burst: the BPTC coded payload in bits 0-97 and 166-263, the slot type (color code, data type) in
    98-107 and 156-165, the sync in 108-155."""
    bits = [0] * 264
    info = bptc19696(payload)
    for i, b in enumerate(info):
        bits[i if i < 98 else i + 68] = b
    st = golay2087(((cc & 0x0F) << 4) | (data_type & 0x0F))
    for i in range(10):
        bits[98 + i] = (st >> (19 - i)) & 1
        bits[156 + i] = (st >> (9 - i)) & 1
    for i in range(48):
        bits[108 + i] = (sync >> (47 - i)) & 1
    return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, 264, 8))


def crc16(octets10, mask):
    """B.3.8: CRC-CCITT over 10 octets, inverted, masked (0xCC data header, 0xA5 CSBK)."""
    crc = 0
    for b in octets10[:10]:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return (~crc ^ ((mask << 8) | mask)) & 0xFFFF


def crc32(data):
    """B.3.9: the octets of each 16-bit word swapped, polynomial 0x04C11DB7, initial 0."""
    crc = 0
    n = len(data) + (len(data) & 1)
    for i in range(n):
        j = i ^ 1
        crc ^= (data[j] if j < len(data) else 0) << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def _with_crc16(ten, mask):
    return bytes(ten) + crc16(ten, mask).to_bytes(2, "big")


def preamble(group, dst, src, follow):
    """A preamble CSBK (9.1.5, 7.2.x): data follows, `follow` CSBKs and headers after it."""
    b = bytes([0x80 | CSBKO_PREAMBLE, 0x00, 0x80 | (0x40 if group else 0), min(follow, 255)]) + \
        dst.to_bytes(3, "big") + src.to_bytes(3, "big")
    return _with_crc16(b, 0xA5)


def unconfirmed_header(group, dst, src, sap, pad, blocks):
    """An unconfirmed data header (9.2.6): `blocks` = every burst after it, the privacy header included."""
    b = bytes([(0x80 if group else 0) | (pad & 0x10) | DPF_UNCONFIRMED, (sap << 4) | (pad & 0x0F)]) + \
        dst.to_bytes(3, "big") + src.to_bytes(3, "big") + bytes([0x80 | blocks, 0])
    return _with_crc16(b, 0xCC)


def privacy_header(sap, alg, key_id=0, mi=0, encrypted=True, mfid=PRIVACY_MFID):
    """The privacy header (docs/spec/dmr-data.md 13.1): the user data's SAP, the MFID, algorithm and encrypted flag,
    the key ID and the message indicator."""
    b = bytes([(sap << 4) | DPF_PROPRIETARY, mfid, ((alg & 0x07) << 4) | (1 if encrypted else 0), key_id, 0, 0]) + \
        mi.to_bytes(4, "big")
    return _with_crc16(b, 0xCC)


def bp_xor(word, data):
    """A 16-bit static keystream over the octets, high octet first (docs/spec/dmr-data.md 13.2)."""
    return bytes(d ^ ((word >> 8) if i % 2 == 0 else (word & 0xFF)) for i, d in enumerate(data))


def data_bursts(dst, src, sap, user, cc=1, group=False, preambles=2, privacy=None, first_sap=None):
    """An unconfirmed packet as bursts: preambles, the header, the privacy header when `privacy` is a dict of
    privacy_header()'s arguments other than sap (its "word" key, when given, encrypts the user data with bp_xor), then
    the rate 1/2 blocks with the CRC-32. first_sap: the first header's SAP when it isn't the user data's (9: the
    privacy header carries it)."""
    data = bytes(user)
    heads = []
    if privacy is not None:
        p = dict(privacy)
        word = p.pop("word", None)
        if word is not None:
            data = bp_xor(word, data)
        heads.append(privacy_header(sap, **p))
    blocks = -(-(len(data) + 4) // 12)
    pad = blocks * 12 - 4 - len(data)
    body = data + bytes(pad)
    body += crc32(body).to_bytes(4, "little")
    out = [burst(preamble(group, dst, src, preambles - 1 - i + 1 + len(heads) + blocks), DT_CSBK, cc)
           for i in range(preambles)]
    out.append(burst(unconfirmed_header(group, dst, src, sap if first_sap is None else first_sap, pad,
                                        len(heads) + blocks), DT_DATA_HEADER, cc))
    out += [burst(h, DT_DATA_HEADER, cc) for h in heads]
    out += [burst(body[i:i + 12], DT_RATE_12, cc) for i in range(0, len(body), 12)]
    return out


def _checksum(b):
    s = sum(int.from_bytes(b[i:i + 2], "big") for i in range(0, len(b) - 1, 2)) + (b[-1] << 8 if len(b) % 2 else 0)
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def ip_address(dmr_id, group=False):
    """The IPv4 address of a DMR ID (TS 102 361-3 5.6): 12.x.y.z, a group 225.x.y.z."""
    return bytes([225 if group else 12]) + dmr_id.to_bytes(3, "big")


def udp_ipv4(src_id, dst_id, sport, dport, payload, group=False, ident=1):
    """IPv4 / UDP without options (SAP 4 user data), both checksums set."""
    src, dst = ip_address(src_id), ip_address(dst_id, group)
    udp_len = 8 + len(payload)
    ip = bytearray([0x45, 0]) + (20 + udp_len).to_bytes(2, "big") + ident.to_bytes(2, "big") + bytes(2) + \
        bytes([1 if group else 64, 17, 0, 0]) + src + dst
    ip[10:12] = _checksum(ip).to_bytes(2, "big")
    udp = bytearray(sport.to_bytes(2, "big") + dport.to_bytes(2, "big") + udp_len.to_bytes(2, "big") + bytes(2)) + \
        bytes(payload)
    c = _checksum(src + dst + bytes([0, 17]) + udp_len.to_bytes(2, "big") + bytes(udp))
    udp[6:8] = (c or 0xFFFF).to_bytes(2, "big")
    return bytes(ip + udp)


def tms_text(text, seq=1, ack=False):
    """A text message on UDP 4007: the length, the header octets and the text in UTF-16LE (docs/spec/dmr-data.md)."""
    body = bytes([0xE0 if ack else 0xA0, 0x00, 0x80 | (seq & 0x1F), 0x04]) + text.encode("utf-16-le")
    return len(body).to_bytes(2, "big") + body


def _self_test():
    # B.3.1: data 0 codes to 0; every codeword differs from the others in at least 8 bits
    assert golay2087(0) == 0
    cws = [golay2087(d) for d in range(256)]
    assert min(bin(a ^ b).count("1") for i, a in enumerate(cws) for b in cws[i + 1:]) == 8
    # Every row and column of the matrix is a codeword: a zero payload codes to zero, a burst is 33 octets
    assert bptc19696(bytes(12)) == [0] * 196
    assert len(burst(bytes(12), DT_DATA_HEADER, 1)) == 33
    h = unconfirmed_header(False, 1, 2, SAP_IP, 0, 3)
    assert crc16(h, 0xCC) == int.from_bytes(h[10:12], "big")
    assert bp_xor(0x1F00, bp_xor(0x1F00, b"abc")) == b"abc"
    print("dmr_burst self-test ok")


if __name__ == "__main__":
    _self_test()
