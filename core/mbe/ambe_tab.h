// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The half-rate vocoder's tables (ambe_tab.c)
#ifndef AMBE_TAB_H
#define AMBE_TAB_H

#include <stdint.h>

#define AMBE_B0_VOICE	120	// pitch codes of a voice frame: 0..119
#define AMBE_FRAME_BITS	72

extern const uint8_t ambe_L[AMBE_B0_VOICE];	// harmonics by pitch code
extern const uint32_t ambe_w0[AMBE_B0_VOICE];	// the fundamental by pitch code, mbe_params_t.w0
extern const uint8_t ambe_vuv[16];		// voicing codebook: bit j = the band of 500 j .. 500 (j + 1) Hz is voiced
extern const uint8_t ambe_J[48][4];		// lengths of the four residual blocks, for L = 9..56
extern const int16_t ambe_dg[64];		// gain step codebook, log2 Q11
extern const int16_t ambe_prba24[512][3];	// PRBA vector G2..G4, log2 Q11
extern const int16_t ambe_prba58[128][4];	// PRBA vector G5..G8
extern const int16_t ambe_hoc[4][16][4];	// higher-order coefficients C(i,3..6) of block i
extern const uint8_t ambe_ilv[AMBE_FRAME_BITS];	// the voice frame's bit order (ambe.h)

#endif
