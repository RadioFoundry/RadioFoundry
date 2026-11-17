// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The full-rate vocoder's dequantizer tables (imbe_tab.c)
#ifndef IMBE_TAB_H
#define IMBE_TAB_H

#include <stdint.h>

extern const uint8_t imbe_ba_bits[48 * 5];	// bits of the gain coefficients G2..G6, for each L = 9..56
extern const uint16_t imbe_ba_step[48 * 5];	// their step sizes, units of 1e-5
extern const uint8_t imbe_hoba[636];		// bits of the higher-order coefficients, nibbles; L - 6 of them for each L
extern const uint16_t imbe_b2[64];		// the first gain's codebook: (value + 3) * 4096
extern const uint8_t imbe_qstep[10];		// step size by bits, units of 0.01
extern const uint16_t imbe_sdev[9];		// standard deviation by coefficient, units of 0.001

#endif
