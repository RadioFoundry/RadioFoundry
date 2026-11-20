// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DB25-D chip identification (boards/db25d/rfchip_id_db25d.h): which register 0 values each position takes and the
// batch letters they give, including the 0x7813 part as the main transceiver (docs/spec/hardware-db25d.md 4.2)
#include <stdio.h>
#include "rfchip_id_db25d.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	// Main transceiver: BK4819 (DBB), FD6818 (D6x, D4), the 0x7813 part on the FD6818 path
	CHECK(db25d_chip_kind(0x4819, false) == CHIP_BK4819);
	CHECK(db25d_chip_kind(0x6818, false) == CHIP_FD6818);
	CHECK(db25d_chip_kind(0x7813, false) == CHIP_7813);
	CHECK(db25d_chip_fd6818_family(db25d_chip_kind(0x7813, false)));
	CHECK(db25d_chip_fd6818_family(db25d_chip_kind(0x6818, false)));
	CHECK(!db25d_chip_fd6818_family(db25d_chip_kind(0x4819, false)));
	// Never the transceiver: the AT1141 synthesizer, the 0x1846 part (no driver), a dead bus
	CHECK(db25d_chip_kind(0x1100, false) == CHIP_NONE);
	CHECK(db25d_chip_kind(0x1846, false) == CHIP_NONE);
	CHECK(db25d_chip_kind(0x0000, false) == CHIP_NONE);
	CHECK(db25d_chip_kind(0xFFFF, false) == CHIP_NONE);

	// LO: every kind
	CHECK(db25d_chip_kind(0x4819, true) == CHIP_BK4819);
	CHECK(db25d_chip_kind(0x6818, true) == CHIP_FD6818);
	CHECK(db25d_chip_kind(0x7813, true) == CHIP_7813);
	CHECK(db25d_chip_kind(0x1100, true) == CHIP_AT1141);
	CHECK(db25d_chip_kind(0x1846, true) == CHIP_NONE);

	// The letters of the batch codes: DBB, D64, D66, D6T, and D44 for a 0x7813 main
	CHECK(db25d_chip_letter(CHIP_BK4819) == 'B');
	CHECK(db25d_chip_letter(CHIP_FD6818) == '6');
	CHECK(db25d_chip_letter(CHIP_7813) == '4');
	CHECK(db25d_chip_letter(CHIP_AT1141) == 'T');
	CHECK(db25d_chip_letter(CHIP_NONE) == '?');

	if (failures) {
		printf("db25dChipIdTest: %d failures\n", failures);
		return 1;
	}
	printf("db25dChipIdTest: all passed\n");
	return 0;
}
