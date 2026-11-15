// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

// The three shortened Reed-Solomon codes over GF(64) of TIA-102.BAAA-A 5.9, cut down from
// RS(63,k): (24,12,13) link control, (24,16,9) encryption sync, (36,20,17) header.
// A code word is an array of "hex bits", one 6-bit symbol per uint8_t, in transmission order:
// the data symbols first, then the parity symbols (the standard's systematic matrices).
// All functions are reentrant.

#ifndef FEC_RS63_H
#define FEC_RS63_H

#include <stdint.h>

typedef enum {
	FEC_RS_24_12_13,	// n = 24, k = 12, 12 parity symbols, corrects 6 errors
	FEC_RS_24_16_9,		// n = 24, k = 16, 8 parity symbols, corrects 4 errors
	FEC_RS_36_20_17,	// n = 36, k = 20, 16 parity symbols, corrects 8 errors
} fec_rs_code_t;

#define FEC_RS_MAX_N 36

unsigned fec_rs63_n(fec_rs_code_t code);
unsigned fec_rs63_k(fec_rs_code_t code);

// cw[0..k-1] holds the data symbols (values 0-63); cw[k..n-1] is filled with the parity.
void fec_rs63_encode(fec_rs_code_t code, uint8_t *cw);

// Errors-and-erasures decode in place. erasures lists symbol indexes (0 = first transmitted),
// may be NULL when nerasures is 0. Corrects while 2*errors + erasures <= n-k.
// Returns the number of symbols changed (>= 0), or -1 when uncorrectable (cw unchanged).
int fec_rs63_decode(fec_rs_code_t code, uint8_t *cw, const uint8_t *erasures, uint8_t nerasures);

#endif
