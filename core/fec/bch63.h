// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

// Primitive BCH(63,16,23) code protecting the network identifier (TIA-102.BAAA-A 8.5.2).
// The standard appends a 64th overall parity bit; this module handles the 63-bit code word
// and leaves that bit to the caller. Systematic: data in bits 62..47, parity in bits 46..0.
// Bit 62 is the first transmitted bit. All functions are reentrant.

#ifndef FEC_BCH63_H
#define FEC_BCH63_H

#include <stdint.h>

uint64_t fec_bch63_encode(uint16_t data);

// Corrects up to 11 bit errors (algebraic: Berlekamp-Massey over GF(64) plus a Chien search).
// Returns the number of bits corrected, or -1 when the word is not within 11 of a code word
// (data untouched).
int fec_bch63_decode(uint64_t cw, uint16_t *data);

#endif
