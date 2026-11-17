// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

// Golay codes of TIA-102.BAAA-A 5.7: the (23,12,7) standard code, the (24,12,8) extended code
// and the (18,6,8) shortened code used for the header. All are systematic: the data bits come
// first (most significant), then the parity bits as the standard's generator matrix gives them.
// Bit layout of a code word value: bit (n-1) is the first transmitted bit.
// All functions are reentrant and table-free beyond a 12-word matrix.

#ifndef FEC_GOLAY_H
#define FEC_GOLAY_H

#include <stdint.h>

// (23,12,7): data in bits 22..11, parity in bits 10..0.
uint32_t fec_golay23_encode(uint16_t data12);
// Corrects up to 3 bit errors. Returns the number corrected (0-3); the code is perfect so it
// never fails (4+ errors decode to the nearest wrong code word).
int fec_golay23_decode(uint32_t cw, uint16_t *data12);

// (24,12,8): data in bits 23..12, parity in bits 11..0 (bit 0 is the overall parity bit).
uint32_t fec_golay24_encode(uint16_t data12);
// Returns 0-3 errors corrected, or -1 when a 4-bit error is detected (data12 untouched).
int fec_golay24_decode(uint32_t cw, uint16_t *data12);

// (18,6,8): data in bits 17..12, parity in bits 11..0 (the (24,12,8) word minus its 6 top bits).
uint32_t fec_golay18_encode(uint8_t data6);
// Returns 0-3 errors corrected, or -1 when uncorrectable (data6 untouched).
int fec_golay18_decode(uint32_t cw, uint8_t *data6);

#endif
