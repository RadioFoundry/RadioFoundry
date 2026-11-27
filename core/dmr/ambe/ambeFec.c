// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AMBE+2 3600x2450 air frame FEC and interleaving (see ambeFec.h for the definition)
#include "ambeFec.h"

#include <stddef.h>
#include <string.h>

#define GOLAY_POLY	0xC75u	// x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1

static const uint8_t cw_len[4] = { 24, 23, 11, 14 };

static int popcount32(uint32_t v)
{
	int n = 0;
	while (v) {
		v &= v - 1;
		n++;
	}
	return n;
}

static uint32_t golay23Parity(uint16_t data)
{
	uint32_t r = (uint32_t)(data & 0xFFFu) << 11;
	for (int bit = 22; bit >= 11; bit--) {
		if (r & (1u << bit))
			r ^= GOLAY_POLY << (bit - 11);
	}
	return r & 0x7FFu;
}

uint32_t ambeGolay23Encode(uint16_t data)
{
	return ((uint32_t)(data & 0xFFFu) << 11) | golay23Parity(data);
}

uint32_t ambeGolay24Encode(uint16_t data)
{
	uint32_t cw = ambeGolay23Encode(data);
	return (cw << 1) | (uint32_t)(popcount32(cw) & 1);
}

// Systematic search: an error pattern of weight <= 3 has at most 3 errors in the data part. For every data error
// pattern ed of weight w <= 3, the parity syndrome left over must have weight <= 3 - w. The codes' minimum distance (7
// and 8) makes the match unique.
static int golaySearch(uint16_t data, uint32_t syndrome, bool extended, uint16_t *fixed)
{
	uint32_t rows[12];
	for (int i = 0; i < 12; i++) {
		rows[i] = extended ? (ambeGolay24Encode((uint16_t)(1u << i)) & 0xFFFu) : golay23Parity((uint16_t)(1u << i));
	}
	if (popcount32(syndrome) <= 3) {
		*fixed = data;
		return popcount32(syndrome);
	}
	for (int i = 0; i < 12; i++) {
		uint32_t s1 = syndrome ^ rows[i];
		if (popcount32(s1) <= 2) {
			*fixed = data ^ (uint16_t)(1u << i);
			return 1 + popcount32(s1);
		}
	}
	for (int i = 0; i < 12; i++) {
		for (int j = i + 1; j < 12; j++) {
			uint32_t s2 = syndrome ^ rows[i] ^ rows[j];
			if (popcount32(s2) <= 1) {
				*fixed = data ^ (uint16_t)((1u << i) | (1u << j));
				return 2 + popcount32(s2);
			}
		}
	}
	for (int i = 0; i < 12; i++) {
		for (int j = i + 1; j < 12; j++) {
			for (int k = j + 1; k < 12; k++) {
				if ((syndrome ^ rows[i] ^ rows[j] ^ rows[k]) == 0) {
					*fixed = data ^ (uint16_t)((1u << i) | (1u << j) | (1u << k));
					return 3;
				}
			}
		}
	}
	*fixed = data;
	return -1;
}

int ambeGolay23Decode(uint32_t cw, uint16_t *data)
{
	uint16_t d = (uint16_t)((cw >> 11) & 0xFFFu);
	return golaySearch(d, (cw & 0x7FFu) ^ golay23Parity(d), false, data);
}

int ambeGolay24Decode(uint32_t cw, uint16_t *data)
{
	uint16_t d = (uint16_t)((cw >> 12) & 0xFFFu);
	return golaySearch(d, (cw & 0xFFFu) ^ (ambeGolay24Encode(d) & 0xFFFu), true, data);
}

uint32_t ambeC1Mask(uint16_t c0_data)
{
	uint32_t x = 16u * (c0_data & 0xFFFu);
	uint32_t mask = 0;
	for (int k = 1; k <= 23; k++) {
		x = (173u * x + 13849u) & 0xFFFFu;
		mask = (mask << 1) | (x >> 15);
	}
	return mask;	// first sequence bit on C1's most significant bit (bit 22)
}

uint8_t ambeAirBitOfSeq(uint8_t n)
{
	// S(18 q + k) goes to air bit 4 k + q
	return (uint8_t)(4u * (n % 18u) + n / 18u);
}

static inline int getBit(const uint8_t *buf, unsigned pos)
{
	return (buf[pos >> 3] >> (7u - (pos & 7u))) & 1;
}

static inline void putBit(uint8_t *buf, unsigned pos, int v)
{
	uint8_t m = (uint8_t)(0x80u >> (pos & 7u));
	if (v)
		buf[pos >> 3] |= m;
	else
		buf[pos >> 3] &= (uint8_t)~m;
}

void ambeInterleave(const uint32_t c[4], uint8_t frame[AMBE_FRAME_BYTES])
{
	unsigned n = 0;
	memset(frame, 0, AMBE_FRAME_BYTES);
	for (int w = 0; w < 4; w++) {
		for (int b = cw_len[w] - 1; b >= 0; b--, n++)
			putBit(frame, ambeAirBitOfSeq((uint8_t)n), (int)((c[w] >> b) & 1u));
	}
}

void ambeDeinterleave(const uint8_t frame[AMBE_FRAME_BYTES], uint32_t c[4])
{
	unsigned n = 0;
	for (int w = 0; w < 4; w++) {
		uint32_t v = 0;
		for (int b = 0; b < cw_len[w]; b++, n++)
			v = (v << 1) | (uint32_t)getBit(frame, ambeAirBitOfSeq((uint8_t)n));
		c[w] = v;
	}
}

static uint32_t packBits(const uint8_t *bits, int count)
{
	uint32_t v = 0;
	for (int i = 0; i < count; i++)
		v = (v << 1) | (bits[i] & 1u);
	return v;
}

static void unpackBits(uint32_t v, uint8_t *bits, int count)
{
	for (int i = 0; i < count; i++)
		bits[i] = (uint8_t)((v >> (count - 1 - i)) & 1u);
}

void ambeFecEncode(const uint8_t bits[AMBE_PARAM_BITS], uint8_t frame[AMBE_FRAME_BYTES])
{
	uint16_t d0 = (uint16_t)packBits(&bits[0], 12);
	uint16_t d1 = (uint16_t)packBits(&bits[12], 12);
	uint32_t c[4];
	c[0] = ambeGolay24Encode(d0);
	c[1] = ambeGolay23Encode(d1) ^ ambeC1Mask(d0);
	c[2] = packBits(&bits[24], 11);
	c[3] = packBits(&bits[35], 14);
	ambeInterleave(c, frame);
}

int ambeFecDecode(const uint8_t frame[AMBE_FRAME_BYTES], uint8_t bits[AMBE_PARAM_BITS], ambe_fec_result_t *res)
{
	ambe_fec_result_t r = { 0 };
	uint32_t c[4];
	uint16_t d0, d1;
	ambeDeinterleave(frame, c);

	int e0 = ambeGolay24Decode(c[0], &d0);
	if (e0 < 0) {
		r.c0_failed = true;
		d0 = (uint16_t)((c[0] >> 12) & 0xFFFu);	// best guess: the bits as received
	} else {
		r.c0_errors = (uint8_t)e0;
	}
	int e1 = ambeGolay23Decode(c[1] ^ ambeC1Mask(d0), &d1);
	r.c1_errors = (uint8_t)(e1 < 0 ? 3 : e1);	// cannot happen for the perfect (23,12) code

	unpackBits(d0, &bits[0], 12);
	unpackBits(d1, &bits[12], 12);
	unpackBits(c[2], &bits[24], 11);
	unpackBits(c[3], &bits[35], 14);

	r.errors = r.c0_failed ? 7 : (uint8_t)(r.c0_errors + r.c1_errors);
	if (res != NULL)
		*res = r;
	return r.errors;
}

void ambeBurstSplit(const uint8_t burst[AMBE_BURST_BYTES], uint8_t frames[3][AMBE_FRAME_BYTES])
{
	for (int f = 0; f < 3; f++)
		memcpy(frames[f], &burst[f * AMBE_FRAME_BYTES], AMBE_FRAME_BYTES);
}

void ambeBurstJoin(const uint8_t frames[3][AMBE_FRAME_BYTES], uint8_t burst[AMBE_BURST_BYTES])
{
	for (int f = 0; f < 3; f++)
		memcpy(&burst[f * AMBE_FRAME_BYTES], frames[f], AMBE_FRAME_BYTES);
}

// Payload bit p (0..215) of the three frames sits at burst bit p for p < 108, else p + 48
static inline unsigned fullBurstPos(unsigned p)
{
	return p < 108u ? p : p + 48u;
}

void ambeFullBurstSplit(const uint8_t burst[AMBE_FULL_BURST_BYTES], uint8_t frames[3][AMBE_FRAME_BYTES])
{
	for (unsigned p = 0; p < 3u * AMBE_FRAME_BITS; p++)
		putBit(frames[p / AMBE_FRAME_BITS], p % AMBE_FRAME_BITS, getBit(burst, fullBurstPos(p)));
}

void ambeFullBurstJoin(const uint8_t frames[3][AMBE_FRAME_BYTES], uint8_t burst[AMBE_FULL_BURST_BYTES])
{
	for (unsigned p = 0; p < 3u * AMBE_FRAME_BITS; p++)
		putBit(burst, fullBurstPos(p), getBit(frames[p / AMBE_FRAME_BITS], p % AMBE_FRAME_BITS));
}
