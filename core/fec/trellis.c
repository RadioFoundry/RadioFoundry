// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

#include "trellis.h"

// Table 7-2: next constellation point for [state][input]. The state is the previous input
// symbol, so the same table also drives the Viterbi trellis (next state = input).
static const uint8_t fsm_half[4][4] = {
	{ 0, 15, 12, 3 },
	{ 4, 11, 8, 7 },
	{ 13, 2, 1, 14 },
	{ 9, 6, 5, 10 },
};
static const uint8_t fsm_34[8][8] = {
	{ 0, 8, 4, 12, 2, 10, 6, 14 },
	{ 4, 12, 2, 10, 6, 14, 0, 8 },
	{ 1, 9, 5, 13, 3, 11, 7, 15 },
	{ 5, 13, 3, 11, 7, 15, 1, 9 },
	{ 3, 11, 7, 15, 1, 9, 5, 13 },
	{ 7, 15, 1, 9, 5, 13, 3, 11 },
	{ 2, 10, 6, 14, 0, 8, 4, 12 },
	{ 6, 14, 0, 8, 4, 12, 2, 10 },
};

// Table 7-3 with Table 9-1 applied: constellation point -> (dibit 0 << 2) | dibit 1.
static const uint8_t point_bits[16] = {
	0x2, 0xA, 0x7, 0xF, 0xE, 0x6, 0xB, 0x3, 0xD, 0x5, 0x8, 0x0, 0x1, 0x9, 0x4, 0xC,
};

#define STEPS (FEC_TRELLIS_IN + 1)	// data symbols plus the flushing zero

static unsigned point_for(fec_trellis_rate_t rate, unsigned state, unsigned sym)
{
	return rate == FEC_TRELLIS_HALF ? fsm_half[state][sym & 3] : fsm_34[state][sym & 7];
}

// Table 7-4. Its printed rows follow a regular pattern, used here instead of a 98-byte table:
// the first 26 outputs take dibit pairs 8 apart from index 0, then three runs of 24 outputs
// start at input 2, 4 and 6.
static unsigned il_index(unsigned j)
{
	if (j < 26) {
		return 8 * (j / 2) + (j & 1);
	}
	unsigned g = (j - 26) / 24;
	unsigned k = (j - 26) % 24;
	return 2 * (g + 1) + 8 * (k / 2) + (k & 1);
}

void fec_trellis_interleave(const uint8_t *in98, uint8_t *out98)
{
	for (unsigned j = 0; j < FEC_TRELLIS_OUT; j++) {
		out98[j] = in98[il_index(j)];
	}
}

void fec_trellis_deinterleave(const uint8_t *in98, uint8_t *out98)
{
	for (unsigned j = 0; j < FEC_TRELLIS_OUT; j++) {
		out98[il_index(j)] = in98[j];
	}
}

void fec_trellis_encode(fec_trellis_rate_t rate, const uint8_t *in, uint8_t *out)
{
	uint8_t raw[FEC_TRELLIS_OUT];
	unsigned state = 0;

	for (unsigned t = 0; t < STEPS; t++) {
		unsigned sym = t < FEC_TRELLIS_IN ? in[t] : 0;
		unsigned bits = point_bits[point_for(rate, state, sym)];
		raw[2 * t] = bits >> 2;
		raw[2 * t + 1] = bits & 3;
		state = sym & (rate == FEC_TRELLIS_HALF ? 3 : 7);
	}
	fec_trellis_interleave(raw, out);
}

static unsigned bitdist(unsigned a, unsigned b)
{
	unsigned x = (a ^ b) & 0xF;
	return (x & 1) + ((x >> 1) & 1) + ((x >> 2) & 1) + ((x >> 3) & 1);
}

int fec_trellis_decode(fec_trellis_rate_t rate, const uint8_t *in, uint8_t *out)
{
	uint8_t raw[FEC_TRELLIS_OUT];
	uint32_t pred[STEPS + 1];	// pred[t + 1]: packed predecessor state per state after step t
	uint16_t metric[8];
	uint16_t next[8];
	unsigned nst = rate == FEC_TRELLIS_HALF ? 4 : 8;
	unsigned sb = rate == FEC_TRELLIS_HALF ? 2 : 3;

	fec_trellis_deinterleave(in, raw);
	for (unsigned s = 0; s < nst; s++) {
		metric[s] = s ? 0x4000 : 0;	// the encoder starts in state 0
	}
	for (unsigned t = 0; t < STEPS; t++) {
		unsigned rx = ((raw[2 * t] & 3) << 2) | (raw[2 * t + 1] & 3);
		uint32_t packed = 0;
		// The flush step admits only input 0.
		unsigned nx = t < FEC_TRELLIS_IN ? nst : 1;
		for (unsigned x = 0; x < nx; x++) {
			unsigned best = 0xFFFF;
			unsigned from = 0;
			for (unsigned s = 0; s < nst; s++) {
				unsigned m = metric[s] + bitdist(point_bits[point_for(rate, s, x)], rx);
				if (m < best) {
					best = m;
					from = s;
				}
			}
			next[x] = (uint16_t)best;
			packed |= (uint32_t)from << (x * sb);
		}
		for (unsigned x = 0; x < nx; x++) {
			metric[x] = next[x];
		}
		for (unsigned x = nx; x < nst; x++) {
			metric[x] = 0xFFFF;
		}
		pred[t + 1] = packed;
	}
	int total = metric[0];
	unsigned cur = 0;	// state after the flush step
	for (unsigned t = STEPS; t >= 1; t--) {
		// pred[t] is indexed by the input of step t-1, giving the input of step t-2.
		cur = (pred[t] >> (cur * sb)) & (nst - 1);
		if (t >= 2) {
			out[t - 2] = (uint8_t)cur;
		}
	}
	return total;
}
