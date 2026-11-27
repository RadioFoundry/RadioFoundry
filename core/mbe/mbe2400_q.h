// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What the 2400 bit/s front end's files share (mbe2400.c, mbe2400_tab.c): the codebooks. Not for other code.
#ifndef MBE2400_Q_H
#define MBE2400_Q_H

#include <stdint.h>
#include "mbe2400.h"

extern const uint8_t mbe2400_L[MBE2400_PITCHES];	// harmonics by pitch code
extern const uint8_t mbe2400_J[48 * 3];		// the first three block lengths for L = 9..56 (the fourth takes the rest)
extern const uint16_t mbe2400_dg[64];		// gain steps by b2, log2 Q13
extern const int8_t mbe2400_prba24[512 * 3];	// G2..G4 by b3, Q4
extern const int8_t mbe2400_prba58[128 * 4];	// G5..G8 by b4, Q7
extern const int8_t mbe2400_hoc[4 * 16 * 4];	// block i's coefficients 3..6 by b(5 + i), Q7

#endif
