// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

// Trellis code for P25 data blocks (TIA-102.BAAA-A clause 7): the rate 1/2 code (4-state FSM,
// dibit input) and the rate 3/4 code (8-state FSM, tribit input). 48 symbols plus one flushing
// zero symbol go in, 49 constellation points = 98 dibits come out, then the block interleaver.
// Dibits are the 2-bit values of Table 9-1 (01 = +3, 00 = +1, 10 = -1, 11 = -3), transmitted
// in array order. All functions are reentrant.

#ifndef FEC_TRELLIS_H
#define FEC_TRELLIS_H

#include <stdint.h>

typedef enum {
	FEC_TRELLIS_HALF,	// input symbols are dibits 0-3 (96 data bits)
	FEC_TRELLIS_3_4,	// input symbols are tribits 0-7 (144 data bits)
} fec_trellis_rate_t;

#define FEC_TRELLIS_IN 48	// symbols per block
#define FEC_TRELLIS_OUT 98	// dibits per block

// in[0..47] one symbol per byte (octets serialized MSB first and split as the standard says);
// out[0..97] interleaved dibits, ready for the modulator.
void fec_trellis_encode(fec_trellis_rate_t rate, const uint8_t *in, uint8_t *out);

// Hard-decision Viterbi decode of an interleaved block (one dibit 0-3 per byte). The flush
// symbol forces the final state, so a path always exists. Returns the number of received
// bits that disagree with the chosen code word (0 = clean), a measure of how many errors
// were corrected; large values mean the block is probably lost.
int fec_trellis_decode(fec_trellis_rate_t rate, const uint8_t *in, uint8_t *out);

// The block interleaver of Table 7-4 on its own (tests and soft-decision front ends).
void fec_trellis_interleave(const uint8_t *in98, uint8_t *out98);
void fec_trellis_deinterleave(const uint8_t *in98, uint8_t *out98);

#endif
