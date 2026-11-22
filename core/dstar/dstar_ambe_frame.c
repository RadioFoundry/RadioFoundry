// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The D-STAR AMBE 3600x2400 voice frame (dstar_ambe_frame.h), written from docs/spec/dstar-ambe-frame.md: the nine
// fields as three 24-bit words, two of them extended Golay words with the second whitened, and the air interleave
// derived there from off-air frames. The field map is mbelib's (ISC) as that page restates it.
#include "dstar/dstar_ambe_frame.h"
#include "dmr/ambe/ambeFec.h"

#include <string.h>

// Each word is held with its first bit (d0, d12, d25) on bit 23, the layout ambeGolay24Encode and ambeWhitening use,
// so W0 and W1 go to the shared DMR coder unchanged: one Golay coder and one sequence generator in the image
enum { W0, W1, W2 };

// The interleave (spec 4): air bit a, in block a / 24 at offset o, carries bit 8 - 4 * block + r + 12 * (o & 1) of word
// w, where (w, r) depends only on o / 2. As a formula over this 12-entry table it costs a few bytes of code where a
// table of all 72 positions would cost 72 or 144 bytes of flash
#define PAIR(w, r)	((uint8_t)((w) << 2 | (r)))
static const uint8_t pair_map[12] = {
	PAIR(W0, 2), PAIR(W2, 3), PAIR(W1, 3), PAIR(W0, 3), PAIR(W1, 1), PAIR(W0, 1),
	PAIR(W2, 2), PAIR(W1, 2), PAIR(W2, 0), PAIR(W1, 0), PAIR(W0, 0), PAIR(W2, 1),
};

// Which word air bit a belongs to, and which of its bits it is
static unsigned air_bit(unsigned a, unsigned *word)
{
	unsigned o = a % 24u;
	uint8_t e = pair_map[o >> 1];
	*word = e >> 2;
	return 8u - 4u * (a / 24u) + (e & 3u) + ((o & 1u) ? 12u : 0u);
}

// Field <-> word bits (spec 2). W0's data is b0's top six bits, b2's top four, b3's top two; W1's is b3's next five,
// b4's top five and b5's top two; W2 holds everything unprotected: the low bits of b5, b6, b7, b8's three sent bits,
// b1, then the low bits of b2, b3, b4 and b0
static uint16_t w0_data(const uint16_t b[DSTAR_AMBE_FIELDS])
{
	return (uint16_t)(((b[0] >> 1) & 63u) << 6 | ((b[2] >> 2) & 15u) << 2 | ((b[3] >> 7) & 3u));
}

static uint16_t w1_data(const uint16_t b[DSTAR_AMBE_FIELDS])
{
	return (uint16_t)(((b[3] >> 2) & 31u) << 7 | ((b[4] >> 2) & 31u) << 2 | ((b[5] >> 2) & 3u));
}

static uint32_t w2_bits(const uint16_t b[DSTAR_AMBE_FIELDS])
{
	return (uint32_t)(b[5] & 3u) << 22 | (uint32_t)(b[6] & 15u) << 18 | (uint32_t)(b[7] & 15u) << 14 |
	       (uint32_t)((b[8] >> 1) & 7u) << 11 | (uint32_t)(b[1] & 15u) << 7 | (uint32_t)(b[2] & 3u) << 5 |
	       (uint32_t)(b[3] & 3u) << 3 | (uint32_t)(b[4] & 3u) << 1 | (uint32_t)(b[0] & 1u);
}

void dstar_ambe_pack(const uint16_t b[DSTAR_AMBE_FIELDS], uint8_t frame[DSTAR_VOICE_BYTES])
{
	uint16_t u0 = w0_data(b);
	// The whitening runs 24 steps: the 24th lands on W1's overall parity bit, which mbelib leaves alone but a DVSI
	// encoder whitens (spec 3)
	uint32_t w[3] = { ambeGolay24Encode(u0), ambeGolay24Encode(w1_data(b)) ^ ambeWhitening(u0, 24), w2_bits(b) };
	memset(frame, 0, DSTAR_VOICE_BYTES);
	for (unsigned a = 0; a < DSTAR_VOICE_BITS; a++) {
		unsigned i, k = air_bit(a, &i);
		frame[a >> 3] |= (uint8_t)(((w[i] >> k) & 1u) << (a & 7u));
	}
}

void dstar_ambe_unpack(const uint8_t frame[DSTAR_VOICE_BYTES], uint16_t b[DSTAR_AMBE_FIELDS], dstar_ambe_fec_t *fec)
{
	uint32_t w[3] = { 0, 0, 0 };
	for (unsigned a = 0; a < DSTAR_VOICE_BITS; a++) {
		unsigned i, k = air_bit(a, &i);
		w[i] |= (uint32_t)((frame[a >> 3] >> (a & 7u)) & 1u) << k;
	}

	// W0 first: its corrected data seeds W1's whitening. A W0 with 4 errors is passed on as received; the codec drops
	// that frame whatever it holds (dstar_codec.c, bad_rule)
	uint16_t u0, u1;
	int n0 = ambeGolay24Decode(w[W0], &u0);
	if (n0 < 0)
		u0 = (uint16_t)(w[W0] >> 12);

	// A W1 with 4 errors still plays (the codec's bad-frame rule), as the 23-bit Golay word without its parity bit
	// decodes, which is what mbelib reads: right whenever the parity bit was one of the four
	uint32_t c1 = w[W1] ^ ambeWhitening(u0, 24);
	int n1 = ambeGolay24Decode(c1, &u1);
	if (n1 < 0)
		(void)ambeGolay23Decode(c1 >> 1, &u1);

	uint32_t r = w[W2];
	b[0] = (uint16_t)((u0 >> 6) << 1 | (r & 1u));
	b[1] = (uint16_t)((r >> 7) & 15u);
	b[2] = (uint16_t)(((u0 >> 2) & 15u) << 2 | ((r >> 5) & 3u));
	b[3] = (uint16_t)((u0 & 3u) << 7 | (u1 >> 7) << 2 | ((r >> 3) & 3u));
	b[4] = (uint16_t)(((u1 >> 2) & 31u) << 2 | ((r >> 1) & 3u));
	b[5] = (uint16_t)((u1 & 3u) << 2 | ((r >> 22) & 3u));
	b[6] = (uint16_t)((r >> 18) & 15u);
	b[7] = (uint16_t)((r >> 14) & 15u);
	b[8] = (uint16_t)(((r >> 11) & 7u) << 1);

	if (fec != NULL) {
		fec->c0_errors = (uint8_t)(n0 < 0 ? 0 : n0);
		fec->c1_errors = (uint8_t)(n1 < 0 ? 0 : n1);
		fec->c0_failed = n0 < 0;
		fec->c1_failed = n1 < 0;
	}
}
