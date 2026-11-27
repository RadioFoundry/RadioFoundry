// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A hardware real-time clock that keeps the time through power-off (a backup cell behind it), the board's side of
// docs/spec/radio-features.md 5.6. Time is UTC seconds since 1970-01-01. A board with no clock, or one that does not
// keep time while off, leaves board.rtc NULL.
#ifndef HAL_RTC_H
#define HAL_RTC_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	// The UTC Unix time the clock holds. False when it holds none (never set, or its backup lost) or the bus did not
	// answer. May take a few milliseconds on a serial bus
	bool (*read)(int64_t *unix_s);
	// Set the clock to the UTC Unix time and mark it valid. False when the bus did not answer or the time is out of
	// its range. The clock takes the new time within a second, so do not read it back at once
	bool (*set)(int64_t unix_s);
} hal_rtc_t;

#endif
