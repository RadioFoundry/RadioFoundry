// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DTMF digits and their tone pairs (ITU-T Q.23), shared by the radio core and the boards' tx_dtmf hooks
// (docs/spec/radio-features.md 3.7)
#ifndef HAL_DTMF_H
#define HAL_DTMF_H

#include <stdint.h>
#include <stdbool.h>

#define DTMF_LOW_1	697u
#define DTMF_LOW_2	770u
#define DTMF_LOW_3	852u
#define DTMF_LOW_4	941u
#define DTMF_HIGH_1	1209u
#define DTMF_HIGH_2	1336u
#define DTMF_HIGH_3	1477u
#define DTMF_HIGH_4	1633u

// The pair for a digit ('0'-'9', '*', '#', 'A'-'D'): false for anything else
static inline bool dtmf_pair(char digit, uint16_t *low_hz, uint16_t *high_hz)
{
	static const uint16_t low[4] = { DTMF_LOW_1, DTMF_LOW_2, DTMF_LOW_3, DTMF_LOW_4 };
	static const uint16_t high[4] = { DTMF_HIGH_1, DTMF_HIGH_2, DTMF_HIGH_3, DTMF_HIGH_4 };
	static const char keys[] = "123A456B789C*0#D";	// row by row, as the keypad
	for (uint8_t i = 0; i < 16; i++) {
		if (keys[i] == digit) {
			*low_hz = low[i / 4];
			*high_hz = high[i % 4];
			return true;
		}
	}
	return false;
}

static inline bool dtmf_valid(char digit)
{
	uint16_t lo, hi;
	return dtmf_pair(digit, &lo, &hi);
}

#endif
