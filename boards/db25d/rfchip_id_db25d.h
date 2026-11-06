// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DB25-D chip identification (docs/spec/hardware-db25d.md 4.2): what a register 0 value means as the main
// transceiver or the LO, and the letter it gives the batch code. Pure, so the host tests run it (boards/db25d/tests).
#ifndef RFCHIP_ID_DB25D_H
#define RFCHIP_ID_DB25D_H

#include <stdint.h>
#include <stdbool.h>
#include "rf/bk4819/bk4819.h"
#include "rf/fd6818/fd6818sh.h"
#include "rf/at1141/at1141.h"

typedef enum { CHIP_NONE, CHIP_BK4819, CHIP_FD6818, CHIP_7813, CHIP_AT1141 } chip_t;

static inline chip_t db25d_chip_kind(uint16_t id, bool lo)
{
	switch (id) {
	case BK4819_ID: return CHIP_BK4819;
	case FD6818SH_ID: return CHIP_FD6818;
	// The FD6818 family's other part. As the main transceiver it takes the FD6818's tables, as the 2024 and later
	// firmware drives it there (4.2); no radio with one as the main is known
	case FD6818SH_ID_7813: return CHIP_7813;
	case AT1141_ID: return lo ? CHIP_AT1141 : CHIP_NONE;	// a synthesizer: never the transceiver
	default: return CHIP_NONE;
	}
}

// Driven through fd6818sh.c
static inline bool db25d_chip_fd6818_family(chip_t k)
{
	return k == CHIP_FD6818 || k == CHIP_7813;
}

static inline char db25d_chip_letter(chip_t k)
{
	switch (k) {
	case CHIP_BK4819: return 'B';
	case CHIP_FD6818: return '6';
	case CHIP_7813: return '4';
	case CHIP_AT1141: return 'T';
	default: return '?';
	}
}

#endif
