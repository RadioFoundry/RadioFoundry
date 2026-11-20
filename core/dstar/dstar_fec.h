// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV coding (docs/spec/dstar-modem.md 3, from the JARL D-STAR standard, appendices 1 and 2): the radio
// header's CRC, its rate 1/2 K=3 convolutional code, the 24-row interleaver and the x^7 + x^4 + 1 scrambler, and the
// scrambling of the 24-bit data frames. No RTOS, no board, integer only.
//
// Bits travel in "air order": bit i of a buffer is byte i / 8, bit i % 8 (LSB first). The standard sends every
// byte least significant bit first (appendix 1.4), so a header or data byte packed this way is already in the order
// it goes on air, and a sync pattern the standard writes left to right is stored with its leftmost bit as bit 0.
#ifndef RFOUNDRY_DSTAR_FEC_H
#define RFOUNDRY_DSTAR_FEC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DSTAR_HEADER_BYTES	41u	// flags 3, RPT2 8, RPT1 8, UR 8, MY 8, MY suffix 4, P_FCS 2
#define DSTAR_HEADER_BITS	660u	// (41 x 8 + 2 tail bits) x 2
#define DSTAR_HEADER_AIR_BYTES	((DSTAR_HEADER_BITS + 7u) / 8u)
#define DSTAR_DATA_BITS		24u	// one data frame (slow data or the resync pattern)

static inline bool dstar_bit(const uint8_t *buf, uint32_t i)
{
	return ((buf[i >> 3] >> (i & 7u)) & 1u) != 0;
}

static inline void dstar_bit_put(uint8_t *buf, uint32_t i, bool v)
{
	if (v) {
		buf[i >> 3] |= (uint8_t)(1u << (i & 7u));
	} else {
		buf[i >> 3] &= (uint8_t)~(1u << (i & 7u));
	}
}

// CRC-CCITT over the header's first 39 bytes. The standard names the polynomial (x^16 + x^12 + x^5 + 1) and its
// bit-reversed form 0x8408 but not the preset or the final inversion; this is the ISO 3309 / X.25 FCS (preset 0xFFFF,
// complemented), the CRC-CCITT the standard's data packets pair with their ISO 3309 CRC-32. dstar-modem.md 3.1 says
// what settles it on air
uint16_t dstar_crc(const uint8_t *p, size_t n);
// The FCS goes low byte first, like every other field, least significant bit first
void dstar_header_crc_set(uint8_t hdr[DSTAR_HEADER_BYTES]);
bool dstar_header_crc_ok(const uint8_t hdr[DSTAR_HEADER_BYTES]);

// Bit i of the scrambler's sequence from its all-ones start (period 127)
bool dstar_pn_bit(uint32_t i);

// The 41 header bytes as the 660 bits sent after the frame sync: coded, interleaved, scrambled
void dstar_header_encode(const uint8_t hdr[DSTAR_HEADER_BYTES], uint8_t air[DSTAR_HEADER_AIR_BYTES]);

// Soft bits: the receiver's confidence per air bit, positive for 1, negative for 0, 0 for nothing known. The header
// decoder takes them packed four bits each (two per byte, bit i in the low nibble of byte i / 2 when i is even) so a
// receiver holds 330 bytes for the header instead of 660; the values are -7..7
#define DSTAR_SOFT_MAX		7
#define DSTAR_HEADER_SOFT_BYTES	(DSTAR_HEADER_BITS / 2u)

static inline void dstar_soft_put(uint8_t *packed, uint32_t i, int8_t s)
{
	if (s > DSTAR_SOFT_MAX) {
		s = DSTAR_SOFT_MAX;
	} else if (s < -DSTAR_SOFT_MAX) {
		s = -DSTAR_SOFT_MAX;
	}
	uint8_t n = (uint8_t)s & 0x0Fu;
	uint8_t *b = &packed[i >> 1];
	*b = (i & 1u) ? (uint8_t)((*b & 0x0Fu) | (uint8_t)(n << 4)) : (uint8_t)((*b & 0xF0u) | n);
}

static inline int8_t dstar_soft_get(const uint8_t *packed, uint32_t i)
{
	uint8_t n = (i & 1u) ? (uint8_t)(packed[i >> 1] >> 4) : (uint8_t)(packed[i >> 1] & 0x0Fu);
	return (int8_t)((n & 0x08u) ? (int)n - 16 : (int)n);
}

typedef struct {
	bool crc_ok;
	uint16_t corrected;	// coded bits the decoder disagreed with (a quality figure: 0 on a clean signal)
} dstar_header_result_t;

// Undo the scrambler and interleaver and run a soft Viterbi decoder over the 660 soft bits; hdr gets the 41 bytes
// whatever the CRC says
dstar_header_result_t dstar_header_decode(const uint8_t soft[DSTAR_HEADER_SOFT_BYTES], uint8_t hdr[DSTAR_HEADER_BYTES]);

// A data frame's three bytes XORed with the first 24 bits of the scrambler sequence (the scrambler restarts at every
// data frame); the same call scrambles and descrambles
void dstar_data_scramble(uint8_t d[3]);

#endif
