// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

// Rate 1/2 convolutional codes of constraint length 3 to 5 (4 to 16 states): the encoder and a Viterbi decoder, for
// every mode on the air that uses one (NXDN's K = 5, D-STAR's K = 3 radio header). Each input bit gives two coded
// bits, G1's then G2's. A polynomial is a mask over the encoder's register: bit 0 the input bit, bit k the input k bits
// back (D^k). The encoder starts in state 0; a caller that ends the block with K - 1 zero (tail) bits gets it back to
// state 0, which the decoder assumes.
//
// The decoder takes soft values, one per coded bit: positive says 1, negative 0, the size how sure, 0 nothing known
// (a punctured or erased bit). Hard decisions are soft values of one size (fec_conv_soft_from_bits). The soft values
// come from an array in coded-bit order (fec_conv_decode) or, for a caller whose bits sit elsewhere (behind an
// interleaver and a scrambler it would rather not undo into a copy), from a function asked for coded bit i
// (fec_conv_decode_fn). Its memory is the caller's: the survivor decisions, 2^(K - 1) bits per input bit
// (FEC_CONV_DEC_BYTES); on the stack, 64 bytes of path metrics and 32 of branch outputs. About 10 x 2^(K - 1)
// operations per input bit (each state: two adds, a compare, a select). The path metrics are 16-bit, renormalized
// when they grow. All functions are reentrant.

#ifndef FEC_CONV_H
#define FEC_CONV_H

#include <stdint.h>

#define FEC_CONV_STATES	16	// the most: K = 5

// The constraint length is the polynomials': one more than the highest D^k either has (3..5). A field of its own would
// only be a second statement of the same fact, free to disagree with the first
typedef struct {
	uint8_t g1, g2;		// polynomial masks, bit 0 = D^0
} fec_conv_code_t;

// The decisions fec_conv_decode_fn keeps for nbits input bits of a code of constraint length k (3..5), in bytes
#define FEC_CONV_DEC_BYTES(k, nbits)	((((uint32_t)(nbits) << ((k) - 1)) + 7u) / 8u)

// nbits input bits from in (MSB first, tail bits included) to 2 x nbits coded bits at out (MSB first)
void fec_conv_encode(const fec_conv_code_t *c, const uint8_t *in, uint16_t nbits, uint8_t *out);

// soft: 2 x nbits values (G1, G2 per input bit). dec: nbits words of the caller's. out: nbits decoded bits, MSB
// first. The path ends in state 0. Returns the number of coded bits with a non-zero soft value whose sign disagrees
// with the decoded word re-encoded: 0 for a clean block, a count of the bits corrected
int fec_conv_decode(const fec_conv_code_t *c, const int8_t *soft, uint16_t nbits, uint16_t *dec, uint8_t *out);

// The same decoder with coded bit i's soft value from soft(ctx, i), i < 2 x nbits; each is asked for twice, in order
// (the decoding, then the count). dec: FEC_CONV_DEC_BYTES(k, nbits) bytes of the caller's
typedef int8_t (*fec_conv_soft_fn)(const void *ctx, uint16_t i);
int fec_conv_decode_fn(const fec_conv_code_t *c, fec_conv_soft_fn soft, const void *ctx, uint16_t nbits, uint8_t *dec,
		       uint8_t *out);

// Hard bits (MSB first) as soft values of size 1, for a decoder fed without soft decisions
void fec_conv_soft_from_bits(const uint8_t *bits, uint16_t n, int8_t *soft);

#endif
