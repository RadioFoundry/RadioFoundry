// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

// GF(2^6) arithmetic for the P25 Reed-Solomon and BCH codes (TIA-102.BAAA-A 5.9).
// The field is built on the primitive polynomial x^6+x+1; elements are 6-bit values
// held in a uint8_t (bit 0 is the coefficient of alpha^0). Inputs are masked to 6 bits.
// All functions are reentrant and use constant tables only.

#ifndef FEC_GF64_H
#define FEC_GF64_H

#include <stdint.h>

#define FEC_GF64_ORDER 63	// multiplicative group order, alpha^63 = 1

uint8_t fec_gf64_mul(uint8_t a, uint8_t b);
uint8_t fec_gf64_inv(uint8_t a);		// 0 for 0
uint8_t fec_gf64_alpha(unsigned e);		// alpha^e, any e
unsigned fec_gf64_log(uint8_t a);		// e with alpha^e = a; 0 for a = 0 (callers test a first)

// Berlekamp-Massey over GF(64) for syndromes S[0..nsyn-1] (S[j] = r(alpha^(j+1))).
// lambda[0..nsyn] holds on entry an erasure locator of degree nerase (lambda[0] = 1;
// the monic-at-zero form prod(1 + X x)); pass {1, 0, ...} for none. On return it holds the
// error-and-erasure locator. Returns its length L (the number of errors plus erasures
// implied by the syndromes); callers check that L roots exist.
int fec_gf64_bm(const uint8_t *syn, uint8_t nsyn, uint8_t *lambda, uint8_t nerase);

#endif
