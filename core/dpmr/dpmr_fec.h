// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// dPMR coding (ETSI TS 102 490 V1.7.1 clause 7): the CRCs, the shortened Hamming (12,8) code, the x^9 + x^5 + 1
// scrambler and the 12-row block interleaver that protect the header information (HI), the control channel (CCH) and
// the end data (END). The traffic channel needs none of it: a voice frame is the AMBE+2 codec's own 72-bit air frame
// (core/dmr/ambe/ambeFec.h), sent as the codec makes it.
//
// Every choice the text of clause 7 leaves open was settled against a real off-air dPMR446 transmission (the Signal ID
// Wiki recording, tests/host/dpmrCaptureTest.c), not guessed: the CRCs start from zero; the scrambler's output is its
// last stage, so its first nine bits are the all-ones preset; the Hamming code sends the eight data bits, most
// significant first, then C3..C0. With those, 178 of 180 Hamming words of nine off-air headers decode with no error
// and every CRC checks; any other choice of the three fails most words. Where the text contradicts itself (the end
// frame's scrambling, below) the radio decided too.
//
// Bits are numbered from the most significant bit of byte 0, the order they go on air.
#ifndef RFOUNDRY_DPMR_FEC_H
#define RFOUNDRY_DPMR_FEC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

static inline bool dpmr_bit(const uint8_t *buf, uint32_t i)
{
	return ((buf[i >> 3] >> (7u - (i & 7u))) & 1u) != 0;
}

static inline void dpmr_bit_put(uint8_t *buf, uint32_t i, bool v)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7u));
	if (v) {
		buf[i >> 3] |= m;
	} else {
		buf[i >> 3] &= (uint8_t)~m;
	}
}

// nbits bits of v (its low bits, most significant first) at bit at, and back
void dpmr_put(uint8_t *buf, uint32_t at, uint32_t v, uint32_t nbits);
uint32_t dpmr_get(const uint8_t *buf, uint32_t at, uint32_t nbits);

// --- CRCs (7.2), over nbits bits most significant first, register preset to zero, no final inversion ---
uint8_t dpmr_crc7(const uint8_t *buf, uint32_t nbits);	// x^7 + x^3 + 1: the CCH and the END data
uint8_t dpmr_crc8(const uint8_t *buf, uint32_t nbits);	// x^8 + x^2 + x + 1: the header information

// --- shortened Hamming (12,8) (7.3): a byte to a 12-bit word (data in the top 8 bits) and back ---
uint16_t dpmr_ham_encode(uint8_t data);
// Corrects one error. Returns 0 (clean), 1 (one bit corrected) or 2 (more: detected, data left as received)
int dpmr_ham_decode(uint16_t word, uint8_t *data);

// --- scrambling (7.4): XOR nbits bits from bit at with the x^9 + x^5 + 1 sequence, its register preset to all ones.
// Each block starts the sequence afresh; the same call descrambles ---
void dpmr_scramble(uint8_t *buf, uint32_t at, uint32_t nbits);

// --- interleaving (7.5): written into 12 rows down the columns, read out along the rows. cols is 6 for the CCH (the
// TCH matrix, 72 bits) and 10 for the HI (120 bits) ---
void dpmr_interleave(const uint8_t *in, uint8_t *out, uint32_t cols);
void dpmr_deinterleave(const uint8_t *in, uint8_t *out, uint32_t cols);

// --- the coded blocks (7.6 - 7.8) ---

#define DPMR_HI_BYTES		10u	// 72 bits of HI and its CRC8
#define DPMR_HI_CODED_BITS	120u
#define DPMR_CCH_BYTES		6u	// 41 bits of CCH and its CRC7
#define DPMR_CCH_CODED_BITS	72u
#define DPMR_END_BYTES		3u	// 17 bits of END and its CRC7
#define DPMR_END_CODED_BITS	72u	// two copies of 36

// What one block's decode found
typedef struct {
	uint8_t fixed;		// Hamming words corrected
	uint8_t bad;		// Hamming words past correcting
	bool crc_ok;
} dpmr_block_result_t;

// n data bytes (the fields and the CRC, already in place) to 12 n coded bits: Hamming each byte, interleave (12 x n),
// scramble. out holds 12 n / 8 bytes
void dpmr_block_encode(const uint8_t *data, uint32_t n, uint8_t *out);
// The inverse; data gets the n bytes as corrected. The CRC is the caller's: crc_ok is left false here
void dpmr_block_decode(const uint8_t *coded, uint32_t n, uint8_t *data, dpmr_block_result_t *res);

// The HI block (7.7): 72 field bits (dpmr_frame.h packs them) + CRC8, coded to 120 bits; and back, crc_ok set
void dpmr_hi_encode(uint8_t data[DPMR_HI_BYTES], uint8_t out[DPMR_HI_CODED_BITS / 8u]);
void dpmr_hi_decode(const uint8_t coded[DPMR_HI_CODED_BITS / 8u], uint8_t data[DPMR_HI_BYTES], dpmr_block_result_t *res);
// The CCH block (7.6): 41 field bits + CRC7 to 72 bits; and back
void dpmr_cch_encode(uint8_t data[DPMR_CCH_BYTES], uint8_t out[DPMR_CCH_CODED_BITS / 8u]);
void dpmr_cch_decode(const uint8_t coded[DPMR_CCH_CODED_BITS / 8u], uint8_t data[DPMR_CCH_BYTES],
		     dpmr_block_result_t *res);
// The END block (7.8): 17 field bits + CRC7, Hamming coded to 36 bits, sent twice, and the 72 scrambled as one block
// (no interleaving); the decode takes the first copy whose CRC checks. Clause 9.6 and its figure scramble each copy on
// its own instead; a real radio's end frame follows 7.8, and so does this
void dpmr_end_encode(uint8_t data[DPMR_END_BYTES], uint8_t out[DPMR_END_CODED_BITS / 8u]);
void dpmr_end_decode(const uint8_t coded[DPMR_END_CODED_BITS / 8u], uint8_t data[DPMR_END_BYTES],
		     dpmr_block_result_t *res);

#endif
