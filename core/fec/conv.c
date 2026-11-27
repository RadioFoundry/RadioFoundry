// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Rate 1/2 convolutional codes of constraint length 3 to 5 and their Viterbi decoder (conv.h).
#include "conv.h"

#include <string.h>

#define METRIC_LIMIT	16000	// renormalize past this: a step adds 254 at most

// The constraint length: the register bits the polynomials use
static unsigned conv_k(const fec_conv_code_t *c)
{
	unsigned k = 0;

	for (unsigned g = c->g1 | c->g2; g; g >>= 1)
		k++;
	return k;
}

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
	unsigned r = 0, mask = (1u << conv_k(c)) - 1u;

	for (uint16_t i = 0; i < nbits; i++) {
		uint8_t o;

		r = ((r << 1) | bit_get(in, i)) & mask;
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

// A step's survivor decisions, one bit per state, at bit i x states of dec: a nibble for 4 states, whole bytes (low
// first) for 8 or 16. Packed so that D-STAR's 330-step header takes 165 bytes of a receiving task's stack, not 660
static void dec_put(uint8_t *dec, uint16_t i, unsigned states, uint16_t d)
{
	if (states == 4) {
		uint8_t *b = &dec[i >> 1];

		*b = (uint8_t)((i & 1) ? (*b & 0x0F) | (d << 4) : (*b & 0xF0) | d);
		return;
	}
	dec += (uint32_t)i * (states >> 3);
	dec[0] = (uint8_t)d;
	if (states == 16)
		dec[1] = (uint8_t)(d >> 8);
}

static uint16_t dec_get(const uint8_t *dec, uint16_t i, unsigned states)
{
	if (states == 4)
		return (uint16_t)((dec[i >> 1] >> ((i & 1) * 4)) & 0x0F);
	dec += (uint32_t)i * (states >> 3);
	return (uint16_t)(states == 16 ? dec[0] | dec[1] << 8 : dec[0]);
}

static int8_t soft_at(const void *ctx, uint16_t i)
{
	return ((const int8_t *)ctx)[i];
}

int fec_conv_decode(const fec_conv_code_t *c, const int8_t *soft, uint16_t nbits, uint16_t *dec, uint8_t *out)
{
	// nbits words hold the decisions of any length's code (FEC_CONV_DEC_BYTES is at most two bytes a step)
	return fec_conv_decode_fn(c, soft_at, soft, nbits, (uint8_t *)dec, out);
}

int fec_conv_decode_fn(const fec_conv_code_t *c, fec_conv_soft_fn soft, const void *ctx, uint16_t nbits, uint8_t *dec,
		       uint8_t *out)
{
	const unsigned k = conv_k(c), states = 1u << (k - 1), mask = (1u << k) - 1u, top = states >> 1;
	int16_t m[2][FEC_CONV_STATES];
	uint8_t out_of[FEC_CONV_STATES * 2];	// coded bits of each (state, input) branch
	unsigned cur = 0, s;
	int errs = 0;

	for (s = 0; s < states * 2; s++)
		out_of[s] = branch(c, s);
	// Only state 0 at the start: the others far behind
	for (s = 0; s < states; s++)
		m[0][s] = s ? -METRIC_LIMIT : 0;
	for (uint16_t i = 0; i < nbits; i++) {
		int16_t a = soft(ctx, (uint16_t)(2 * i)), b = soft(ctx, (uint16_t)(2 * i + 1));
		// The four branch metrics (a correlation: larger is likelier), by coded bits G1 G2
		int16_t bm[4] = { (int16_t)(-a - b), (int16_t)(-a + b), (int16_t)(a - b), (int16_t)(a + b) };
		int16_t *from = m[cur], *to = m[cur ^ 1], best = -32767;
		uint16_t d = 0;

		// State s' = (s << 1 | u) & (states - 1) is reached from s0 = s' >> 1 and s1 = s0 | top (the bit that
		// drops out); on a tie s0's path stays
		for (s = 0; s < states; s++) {
			unsigned s0 = s >> 1, s1 = s0 | top, u = s & 1;
			int16_t p0 = (int16_t)(from[s0] + bm[out_of[(s0 << 1 | u) & mask]]);
			int16_t p1 = (int16_t)(from[s1] + bm[out_of[(s1 << 1 | u) & mask]]);

			if (p1 > p0) {
				to[s] = p1;
				d |= (uint16_t)(1u << s);
			} else {
				to[s] = p0;
			}
			if (to[s] > best)
				best = to[s];
		}
		dec_put(dec, i, states, d);
		if (best > METRIC_LIMIT || best < -METRIC_LIMIT)
			for (s = 0; s < states; s++)
				to[s] = (int16_t)(to[s] < -METRIC_LIMIT + best ? -METRIC_LIMIT : to[s] - best);
		cur ^= 1;
	}
	// Back from state 0 (the tail put it there)
	s = 0;
	for (uint16_t i = nbits; i-- > 0;) {
		bit_put(out, i, (uint8_t)(s & 1));
		s = (s >> 1) | ((dec_get(dec, i, states) >> s) & 1u) << (k - 2);
	}
	// Disagreements with the soft values, on the path taken (from state 0, where the traceback ends)
	s = 0;
	for (uint16_t i = 0; i < nbits; i++) {
		int8_t a = soft(ctx, (uint16_t)(2 * i)), b = soft(ctx, (uint16_t)(2 * i + 1));
		uint8_t o;

		s = ((s << 1) | bit_get(out, i)) & mask;
		o = out_of[s];
		if (a && (a > 0) != ((o >> 1) & 1))
			errs++;
		if (b && (b > 0) != (o & 1))
			errs++;
	}
	return errs;
}
