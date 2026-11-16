// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The FM squelch levels on the chip's noise reading (radio-features.md 3.5), shared by the radio core and a second
// receiver that runs its own squelch (hal_rx2.h), so both open on the same signal at the same level
#ifndef RADIO_SQUELCH_H
#define RADIO_SQUELCH_H

#include <stdint.h>

#define RADIO_SQUELCH_DEFAULT	5
#define RADIO_SQUELCH_MAX	9

// Software squelch on the chip's noise reading (1 dB units, larger = more noise). Level 0 is always open.
// Level 1..9 opens when noise < RADIO_SQL_OPEN_L1 - (level - 1) * RADIO_SQL_DB_PER_STEP, so 1 = 64 (loose) .. 9 = 40 (tight),
// and closes once noise rises RADIO_SQL_HYST_DB above that for RADIO_SQL_CLOSE_POLLS polls in a row.
#define RADIO_SQL_OPEN_L1	64
#define RADIO_SQL_DB_PER_STEP	3
#define RADIO_SQL_HYST_DB	3
#define RADIO_SQL_CLOSE_POLLS	2

// The noise reading a level opens below (255 for level 0: always open)
static inline uint8_t radio_squelch_open_below(uint8_t level)
{
	if (level == 0) {
		return 255;
	}
	if (level > RADIO_SQUELCH_MAX) {
		level = RADIO_SQUELCH_MAX;
	}
	return (uint8_t)(RADIO_SQL_OPEN_L1 - (level - 1) * RADIO_SQL_DB_PER_STEP);
}

#endif
