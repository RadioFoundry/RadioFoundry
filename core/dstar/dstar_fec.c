// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV coding (dstar_fec.h)
#include <string.h>
#include "dstar/dstar_fec.h"

// Its own copy, not core/aprs's ax25_fcs (the same CRC): D-STAR builds without APRS, and ten lines aren't worth a
// shared module between two protocol layers that are switched separately
uint16_t dstar_crc(const uint8_t *p, size_t n)
{
	uint16_t crc = 0xFFFFu;
	while (n--) {
		crc ^= *p++;
		for (int b = 0; b < 8; b++) {
			crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
		}
	}
	return (uint16_t)~crc;
}

void dstar_header_crc_set(uint8_t hdr[DSTAR_HEADER_BYTES])
{
	uint16_t crc = dstar_crc(hdr, DSTAR_HEADER_BYTES - 2u);
	hdr[39] = (uint8_t)crc;
	hdr[40] = (uint8_t)(crc >> 8);
}

bool dstar_header_crc_ok(const uint8_t hdr[DSTAR_HEADER_BYTES])
{
	uint16_t crc = dstar_crc(hdr, DSTAR_HEADER_BYTES - 2u);
	return hdr[39] == (uint8_t)crc && hdr[40] == (uint8_t)(crc >> 8);
}

// The standard's figure (appendix 1.1): seven stages shifting one way, the feedback the XOR of the last stage and the
// fourth, fed back into the first stage and XORed with the data. As a sequence: s[n] = s[n - 4] ^ s[n - 7], the seven
// bits before s[0] all ones. Bit 0 of the state below is s[n - 1]
static uint8_t pn_next(uint8_t *st)
{
	uint8_t b = (uint8_t)(((*st >> 3) ^ (*st >> 6)) & 1u);
	*st = (uint8_t)(((*st << 1) | b) & 0x7Fu);
	return b;
}

bool dstar_pn_bit(uint32_t i)
{
	uint8_t st = 0x7Fu, b = 0;
	for (uint32_t k = 0; k <= i % 127u; k++) {
		b = pn_next(&st);
	}
	return b != 0;
}

// The interleaver (appendix 2.2): the coded bits fill a 24-row matrix column by column (coded bit 24 c + r in row r,
// column c), 28 columns in rows 0-11 and 27 in rows 12-23 (660 = 12 x 28 + 12 x 27), and go on air row by row.
// The encoder walks the air positions in order; the decoder maps a coded index back to its air position
#define IL_ROWS		24u
#define IL_LONG_ROWS	12u

void dstar_header_encode(const uint8_t hdr[DSTAR_HEADER_BYTES], uint8_t air[DSTAR_HEADER_AIR_BYTES])
{
	// The convolutional code (appendix 2.1, the older translation names the polynomials): G1 = 1 + D + D^2 first,
	// then G2 = 1 + D^2, from zeroed registers, the header least significant bit first, then two zero bits
	uint8_t coded[DSTAR_HEADER_AIR_BYTES];
	memset(coded, 0, sizeof(coded));
	uint8_t x1 = 0, x2 = 0;
	for (uint32_t k = 0; k < DSTAR_HEADER_BITS / 2u; k++) {
		uint8_t u = k < DSTAR_HEADER_BYTES * 8u ? (uint8_t)dstar_bit(hdr, k) : 0u;
		dstar_bit_put(coded, 2u * k, (u ^ x1 ^ x2) != 0);
		dstar_bit_put(coded, 2u * k + 1u, (u ^ x2) != 0);
		x2 = x1;
		x1 = u;
	}
	memset(air, 0, DSTAR_HEADER_AIR_BYTES);
	uint8_t st = 0x7Fu;
	uint32_t j = 0;
	for (uint32_t r = 0; r < IL_ROWS; r++) {
		uint32_t cols = r < IL_LONG_ROWS ? 28u : 27u;
		for (uint32_t c = 0; c < cols; c++, j++) {
			bool b = dstar_bit(coded, IL_ROWS * c + r) ^ (pn_next(&st) != 0);
			dstar_bit_put(air, j, b);
		}
	}
}

// The soft value of coded bit c: its air position through the interleaver, descrambled
static int32_t coded_soft(const uint8_t *soft, const uint8_t *pn, uint32_t c)
{
	uint32_t col = c / IL_ROWS, r = c % IL_ROWS;
	uint32_t j = r < IL_LONG_ROWS ? r * 28u + col : IL_LONG_ROWS * 28u + (r - IL_LONG_ROWS) * 27u + col;
	int32_t s = dstar_soft_get(soft, j);
	return dstar_bit(pn, j % 127u) ? -s : s;
}

dstar_header_result_t dstar_header_decode(const uint8_t soft[DSTAR_HEADER_SOFT_BYTES], uint8_t hdr[DSTAR_HEADER_BYTES])
{
	// The coded bits are read straight out of the air-order buffer through the interleaver and the scrambler, with
	// the scrambler's 127-bit period in a 16-byte table, so the decoder needs no second 330-byte copy on the stack of
	// whichever task runs it
	uint8_t pn[16];
	memset(pn, 0, sizeof(pn));
	uint8_t st = 0x7Fu;
	for (uint32_t i = 0; i < 127u; i++) {
		dstar_bit_put(pn, i, pn_next(&st) != 0);
	}
	// Viterbi over the four states (x1 = the last input bit, x2 the one before: state = x1 | x2 << 1). Path metrics
	// are correlations, larger is better; a step's four survivor choices (the x2 of the predecessor) take a nibble
	enum { STEPS = DSTAR_HEADER_BITS / 2u };
	uint8_t dec[(STEPS + 1u) / 2u];
	int32_t pm[4] = { 0, -1000000, -1000000, -1000000 };	// the encoder starts in state 0
	for (uint32_t k = 0; k < STEPS; k++) {
		int32_t s1 = coded_soft(soft, pn, 2u * k), s2 = coded_soft(soft, pn, 2u * k + 1u);
		int32_t npm[4];
		uint8_t d = 0;
		for (uint8_t ns = 0; ns < 4u; ns++) {
			uint8_t u = ns & 1u, x1 = (uint8_t)(ns >> 1);
			int32_t best = 0;
			for (uint8_t x2 = 0; x2 < 2u; x2++) {
				uint8_t p = (uint8_t)(x1 | (x2 << 1));
				int32_t m = pm[p] + ((u ^ x1 ^ x2) ? s1 : -s1) + ((u ^ x2) ? s2 : -s2);
				if (x2 == 0 || m > best) {
					best = m;
					if (x2) {
						d |= (uint8_t)(1u << ns);
					}
				}
			}
			npm[ns] = best;
		}
		memcpy(pm, npm, sizeof(pm));
		if (k & 1u) {
			dec[k >> 1] = (uint8_t)((dec[k >> 1] & 0x0Fu) | (uint8_t)(d << 4));
		} else {
			dec[k >> 1] = d;
		}
	}
	// The two tail bits bring the encoder back to state 0
	memset(hdr, 0, DSTAR_HEADER_BYTES);
	uint8_t s = 0;
	for (uint32_t k = STEPS; k-- > 0;) {
		if (k < DSTAR_HEADER_BYTES * 8u && (s & 1u)) {
			hdr[k >> 3] |= (uint8_t)(1u << (k & 7u));
		}
		uint8_t d = (uint8_t)(((k & 1u) ? dec[k >> 1] >> 4 : dec[k >> 1]) & 0x0Fu);
		s = (uint8_t)((s >> 1) | (((d >> s) & 1u) << 1));
	}

	// Count the coded bits the decision overrules: re-encode and compare signs
	dstar_header_result_t res = { dstar_header_crc_ok(hdr), 0 };
	uint8_t x1 = 0, x2 = 0;
	for (uint32_t k = 0; k < STEPS; k++) {
		uint8_t u = k < DSTAR_HEADER_BYTES * 8u ? (uint8_t)dstar_bit(hdr, k) : 0u;
		int32_t s1 = coded_soft(soft, pn, 2u * k), s2 = coded_soft(soft, pn, 2u * k + 1u);
		if ((s1 > 0) != ((u ^ x1 ^ x2) != 0)) {
			res.corrected++;
		}
		if ((s2 > 0) != ((u ^ x2) != 0)) {
			res.corrected++;
		}
		x2 = x1;
		x1 = u;
	}
	return res;
}

void dstar_data_scramble(uint8_t d[3])
{
	uint8_t st = 0x7Fu;
	for (uint32_t i = 0; i < DSTAR_DATA_BITS; i++) {
		if (pn_next(&st)) {
			d[i >> 3] ^= (uint8_t)(1u << (i & 7u));
		}
	}
}
