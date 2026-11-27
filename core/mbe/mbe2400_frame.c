// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The 2400 bit/s front end's 72-bit frame (mbe2400.h, docs/spec/mbe-2400.md 3): the nine fields in three 24-bit words,
// the first two extended Golay(24,12) code words (core/fec), the second whitened from the first's data, and the
// words' bits interleaved over the frame.
#include "mbe2400.h"
#include "../fec/golay.h"

// Air bit a = 24 block + 2 p + h (block 0..2, pair p 0..11, h 0..1) carries bit 8 - 4 block + PAIR_BIT[p] + 12 h of
// word PAIR_WORD[p]: each word sends four adjacent pairs of bits (k, k + 12) in each block of 24
static const uint8_t PAIR_WORD[12] = { 0, 2, 1, 0, 1, 0, 2, 1, 2, 1, 0, 2 };
static const uint8_t PAIR_BIT[12] = { 2, 3, 3, 3, 1, 1, 2, 2, 0, 0, 0, 1 };

// The whitening of the second word: x0 = 16 d, x = 173 x + 13849 mod 2^16, step s's bit 15 on the word's bit 24 - s
static uint32_t whitening(uint16_t d0)
{
	uint32_t x = (uint32_t)d0 << 4, m = 0;

	for (int s = 23; s >= 0; s--) {
		x = (173u * x + 13849u) & 0xFFFFu;
		m |= (x >> 15) << s;
	}
	return m;
}

void mbe2400_frame_pack(const uint16_t b[MBE2400_FIELDS], uint8_t out[MBE2400_BYTES])
{
	uint16_t d0 = (uint16_t)(((b[0] >> 1) & 63) << 6 | ((b[2] >> 2) & 15) << 2 | ((b[3] >> 7) & 3));
	uint16_t d1 = (uint16_t)(((b[3] >> 2) & 31) << 7 | ((b[4] >> 2) & 31) << 2 | ((b[5] >> 2) & 3));
	uint32_t w[3];

	w[0] = fec_golay24_encode(d0);
	w[1] = fec_golay24_encode(d1) ^ whitening(d0);
	w[2] = (uint32_t)(b[5] & 3) << 22 | (uint32_t)(b[6] & 15) << 18 | (uint32_t)(b[7] & 15) << 14 |
	       (uint32_t)((b[8] >> 1) & 7) << 11 | (uint32_t)(b[1] & 15) << 7 | (uint32_t)(b[2] & 3) << 5 |
	       (uint32_t)(b[3] & 3) << 3 | (uint32_t)(b[4] & 3) << 1 | (b[0] & 1u);
	for (int i = 0; i < MBE2400_BYTES; i++)
		out[i] = 0;
	for (int blk = 0, a = 0; blk < 3; blk++)
		for (int p = 0; p < 12; p++)
			for (int h = 0; h < 2; h++, a++)
				out[a >> 3] |= (uint8_t)(((w[PAIR_WORD[p]] >> (8 - 4 * blk + PAIR_BIT[p] + 12 * h)) & 1) << (a & 7));
}

uint8_t mbe2400_frame_unpack(const uint8_t in[MBE2400_BYTES], uint16_t b[MBE2400_FIELDS])
{
	uint32_t w[3] = { 0, 0, 0 }, w2;
	uint16_t d0, d1;
	int c0, c1;

	for (int blk = 0, a = 0; blk < 3; blk++)
		for (int p = 0; p < 12; p++)
			for (int h = 0; h < 2; h++, a++)
				w[PAIR_WORD[p]] |= (uint32_t)((in[a >> 3] >> (a & 7)) & 1) << (8 - 4 * blk + PAIR_BIT[p] + 12 * h);
	// Hard decisions only. A first word with four errors is passed on as received (the frame is then bad); a second
	// one is read as its 23-bit word without the parity bit
	c0 = fec_golay24_decode(w[0], &d0);
	if (c0 < 0) {
		d0 = (uint16_t)(w[0] >> 12);
		c0 = 4;
	}
	w[1] ^= whitening(d0);
	c1 = fec_golay24_decode(w[1], &d1);
	if (c1 < 0) {
		fec_golay23_decode(w[1] >> 1, &d1);
		c1 = 4;
	}
	w2 = w[2];
	b[0] = (uint16_t)((d0 >> 6) << 1 | (w2 & 1));
	b[1] = (uint16_t)((w2 >> 7) & 15);
	b[2] = (uint16_t)(((d0 >> 2) & 15) << 2 | ((w2 >> 5) & 3));
	b[3] = (uint16_t)((d0 & 3) << 7 | (d1 >> 7) << 2 | ((w2 >> 3) & 3));
	b[4] = (uint16_t)(((d1 >> 2) & 31) << 2 | ((w2 >> 1) & 3));
	b[5] = (uint16_t)((d1 & 3) << 2 | ((w2 >> 22) & 3));
	b[6] = (uint16_t)((w2 >> 18) & 15);
	b[7] = (uint16_t)((w2 >> 14) & 15);
	b[8] = (uint16_t)(((w2 >> 11) & 7) << 1);
	return MBE2400_ERRS(c0, c1);
}

const uint8_t mbe2400_silence[MBE2400_BYTES] = { 0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8 };
