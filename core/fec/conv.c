// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Rate 1/2, constraint length 5 convolutional code and its Viterbi decoder (conv.h).
#include "conv.h"

#include <string.h>

#define METRIC_LIMIT	16000	// renormalize past this: a step adds 254 at most

static uint8_t parity5(unsigned x)
{
	x &= 0x1F;
	x ^= x >> 4;
	x ^= x >> 2;
	x ^= x >> 1;
	return (uint8_t)(x & 1);
}

static uint8_t bit_get(const uint8_t *p, uint16_t i)
{
	return (uint8_t)((p[i >> 3] >> (7 - (i & 7))) & 1);
}

static void bit_put(uint8_t *p, uint16_t i, uint8_t b)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7));

	p[i >> 3] = (uint8_t)(b ? (p[i >> 3] | m) : (p[i >> 3] & ~m));
}

// The two coded bits for the register r (bit 0 the newest input), G1's in bit 1
static uint8_t branch(const fec_conv_code_t *c, unsigned r)
{
	return (uint8_t)(parity5(r & c->g1) << 1 | parity5(r & c->g2));
}

void fec_conv_encode(const fec_conv_code_t *c, const uint8_t *in, uint16_t nbits, uint8_t *out)
{
	unsigned r = 0;

	for (uint16_t i = 0; i < nbits; i++) {
		uint8_t o;

		r = ((r << 1) | bit_get(in, i)) & 0x1F;
		o = branch(c, r);
		bit_put(out, (uint16_t)(2 * i), o >> 1);
		bit_put(out, (uint16_t)(2 * i + 1), o & 1);
	}
}

void fec_conv_soft_from_bits(const uint8_t *bits, uint16_t n, int8_t *soft)
{
	for (uint16_t i = 0; i < n; i++)
		soft[i] = bit_get(bits, i) ? 1 : -1;
}

int fec_conv_decode(const fec_conv_code_t *c, const int8_t *soft, uint16_t nbits, uint16_t *dec, uint8_t *out)
{
	int16_t m[2][FEC_CONV_STATES];
	uint8_t out_of[FEC_CONV_STATES * 2];	// coded bits of each (state, input) branch
	unsigned cur = 0, s;
	int errs = 0;

	for (s = 0; s < FEC_CONV_STATES * 2; s++)
		out_of[s] = branch(c, s);
	// Only state 0 at the start: the others far behind
	for (s = 0; s < FEC_CONV_STATES; s++)
		m[0][s] = s ? -METRIC_LIMIT : 0;
	for (uint16_t i = 0; i < nbits; i++) {
		int16_t a = soft[2 * i], b = soft[2 * i + 1];
		// The four branch metrics (a correlation: larger is likelier), by coded bits G1 G2
		int16_t bm[4] = { (int16_t)(-a - b), (int16_t)(-a + b), (int16_t)(a - b), (int16_t)(a + b) };
		int16_t *from = m[cur], *to = m[cur ^ 1], best = -32767;
		uint16_t d = 0;

		// State s' = (s << 1 | u) & 15 is reached from s0 = s' >> 1 and s1 = s0 | 8 (the bit that drops out)
		for (s = 0; s < FEC_CONV_STATES; s++) {
			unsigned s0 = s >> 1, s1 = s0 | 8, u = s & 1;
			int16_t p0 = (int16_t)(from[s0] + bm[out_of[(s0 << 1 | u) & 0x1F]]);
			int16_t p1 = (int16_t)(from[s1] + bm[out_of[(s1 << 1 | u) & 0x1F]]);

			if (p1 > p0) {
				to[s] = p1;
				d |= (uint16_t)(1u << s);
			} else {
				to[s] = p0;
			}
			if (to[s] > best)
				best = to[s];
		}
		dec[i] = d;
		if (best > METRIC_LIMIT || best < -METRIC_LIMIT)
			for (s = 0; s < FEC_CONV_STATES; s++)
				to[s] = (int16_t)(to[s] < -METRIC_LIMIT + best ? -METRIC_LIMIT : to[s] - best);
		cur ^= 1;
	}
	// Back from state 0 (the tail put it there)
	s = 0;
	for (uint16_t i = nbits; i-- > 0;) {
		bit_put(out, i, (uint8_t)(s & 1));
		s = (s >> 1) | ((dec[i] >> s) & 1u) << 3;
	}
	// Disagreements with the soft values, on the path taken (from state 0, where the traceback ends)
	s = 0;
	for (uint16_t i = 0; i < nbits; i++) {
		uint8_t o;

		s = ((s << 1) | bit_get(out, i)) & 0x1F;
		o = out_of[s];
		if (soft[2 * i] && (soft[2 * i] > 0) != ((o >> 1) & 1))
			errs++;
		if (soft[2 * i + 1] && (soft[2 * i + 1] > 0) != (o & 1))
			errs++;
	}
	return errs;
}
