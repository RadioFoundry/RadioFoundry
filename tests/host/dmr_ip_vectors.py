#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Test vectors for dmrDataTest (testSap3, testCRC9, testConfirmedTx), worked out from the standards apart from
core/dmr/dmrData.c: the CRCs by polynomial division as ETSI TS 102 361-1 B.3.8 (CRC-CCITT), B.3.9 (CRC-32) and
B.3.10 (CRC-9) describe them, the confirmed header as Table 9.10, the response as Figure 8.5 / 8.17, the compressed
UDP/IPv4 header as TS 102 361-3 Table 7.14. Run: python3 tests/host/dmr_ip_vectors.py"""
def polymod(m, g, gdeg):
    while m.bit_length() > gdeg:
        m ^= g << (m.bit_length() - 1 - gdeg)
    return m

def crc9(data, serial):
    bits = ''.join(f'{b:08b}' for b in data) + f'{serial:07b}'
    M = int(bits, 2)
    G = (1<<9)|(1<<6)|(1<<4)|(1<<3)|1
    r = polymod(M << 9, G, 9)
    return r ^ 0x1FF

def crc32(data):
    # B.3.9: octets taken in pairs, least significant octet of each 16-bit word first; message polynomial MSB first,
    # x^32 M(x) mod G, zero initial remainder, no inversion
    d = bytearray(data)
    if len(d) % 2: d.append(0)
    sw = bytearray()
    for i in range(0, len(d), 2): sw += bytes([d[i+1], d[i]])
    M = int.from_bytes(sw, 'big') if sw else 0
    G = 0x104C11DB7
    return polymod(M << 32, G, 32)

def ccitt(b10, mask):
    M = int.from_bytes(b10, 'big')
    r = polymod(M << 16, 0x11021, 16)
    return (r ^ 0xFFFF) ^ mask

for d, s in [(bytes(10), 0), (bytes(range(10)), 0), (bytes(range(10)), 5), (bytes([0xFF]*16), 127), (bytes(range(22)), 3)]:
    print('crc9', d.hex(), s, hex(crc9(d, s)))

# SAP 3 header for UDP 50000 -> 161, ip id 0x1234, individual: 12 34 00 00 00 C3 50 00 A1
def udpc(ipid, group, sp, dp):
    ids = {5016:1, 5017:2, 4007:98}
    spid, dpid = ids.get(sp, 0), ids.get(dp, 0)
    h = ipid.to_bytes(2,'big') + bytes([(0<<4)|(2 if group else 0), spid, dpid])
    if spid == 0: h += sp.to_bytes(2,'big')
    if dpid == 0: h += dp.to_bytes(2,'big')
    return h
print('udpc snmp', udpc(0x1234, False, 50000, 161).hex())
print('udpc text group', udpc(7, True, 5016, 5016).hex())
print('udpc lip->x', udpc(7, False, 5017, 6000).hex())

# A confirmed rate 1/2 packet: SAP 3, dst 100, src 10005, ns 5, first try; pdu = udpc(0x0001, F, 5016, 5016) + "ODR"
pdu = udpc(1, False, 5016, 5016) + b'ODR'
dl = 10
blocks = (len(pdu) + 4 + dl - 1) // dl
pad = blocks * dl - 4 - len(pdu)
hdr = bytes([0x40 | (0x10 if pad & 0x10 else 0) | 3, (3 << 4) | (pad & 0xF)]) + (100).to_bytes(3,'big') + (10005).to_bytes(3,'big') + bytes([0x80 | blocks, (5 << 4) | 8])
c = ccitt(hdr, 0xCCCC)
hdr += c.to_bytes(2, 'big')
print('cnf header', hdr.hex(), 'blocks', blocks, 'pad', pad)
data = pdu + bytes(pad)
cr = crc32(data)
data += cr.to_bytes(4, 'little')
for k in range(blocks):
    d = data[k*dl:(k+1)*dl]
    c9 = crc9(d, k) ^ 0x0F0
    print('cnf block', k, (bytes([(k << 1) | (c9 >> 8), c9 & 0xFF]) + d).hex())
# SACK response from 100 to 10005, sap 3, ns 5: block 1 of 2 missing
flags = bytes([0xFD] + [0xFF]*7)
rh = bytes([1, 3 << 4]) + (10005).to_bytes(3,'big') + (100).to_bytes(3,'big') + bytes([1, 0x80 | 5])
rh += ccitt(rh, 0xCCCC).to_bytes(2,'big')
rb = flags + crc32(flags).to_bytes(4,'little')
print('sack header', rh.hex(), 'block', rb.hex())
