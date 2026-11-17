// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

// Hamming codes of TIA-102.BAAA-A 5.8: the (15,11,3) standard code (voice) and the shortened
// (10,6,3) code (link control and encryption sync words). Systematic: data in the high bits,
// 4 parity bits in the low bits. The shortened code is not a plain truncation: its parity
// columns were chosen by the standard, so it has its own table.
// All functions are reentrant.

#ifndef FEC_HAMMING_H
#define FEC_HAMMING_H

#include <stdint.h>
#include <stdbool.h>

// (15,11,3): data bits 14..4, parity bits 3..0.
uint16_t fec_hamming15_encode(uint16_t data11);
// Corrects one bit error in place. false: the syndrome matches no single bit (cannot happen
// for this perfect code, kept for symmetry); the word is left alone.
bool fec_hamming15_correct(uint16_t *cw);

// (10,6,3): data bits 9..4, parity bits 3..0.
uint16_t fec_hamming10_encode(uint8_t data6);
// false: the syndrome matches no single bit (a detected multi-bit error); the word is left alone.
bool fec_hamming10_correct(uint16_t *cw);

#endif
