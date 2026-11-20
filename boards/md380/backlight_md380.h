// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-380 backlight PWM: TIM8 CH1 in PWM mode 1, 1800 counts a period (ARR 1799) from the 72 MHz APB2 timer clock. The
// output is on while the count is below the compare value (CV), so CV 0 is off and CV 1800 (above ARR) on through the
// whole period. The rates differ only in the prescaler, so a CV gives the same duty at every rate; the standard one,
// 40 kHz, is far above the FM transmit audio band (#330). The slower ones are for the backlight tests only.
// Pure, for the host tests (tests/md380BacklightTest.c)
#ifndef MD380_BACKLIGHT_H
#define MD380_BACKLIGHT_H

#include <stdint.h>

#define MD380_BL_TIMER_HZ	72000000u
#define MD380_BL_PERIOD		1800u		// counts a period: ARR + 1, and the CV that is full
#define MD380_BL_RATE_STD	0u		// 40 kHz

// Prescaler divisors (PSC + 1), fastest rate first: 40 kHz down to 100 Hz
static const uint16_t md380_bl_div[] = { 1, 2, 3, 4, 5, 6, 8, 10, 15, 20, 30, 40, 60, 90, 180, 400 };
#define MD380_BL_RATES		((uint8_t)(sizeof(md380_bl_div) / sizeof(md380_bl_div[0])))

// Rate i's frequency in Hz, rounded down; an index past the table is the slowest
static inline uint32_t md380_bl_rate_hz(uint8_t i)
{
	if (i >= MD380_BL_RATES) {
		i = MD380_BL_RATES - 1u;
	}
	return MD380_BL_TIMER_HZ / ((uint32_t)md380_bl_div[i] * MD380_BL_PERIOD);
}

// The core's percent as a CV: linear, 0 off, 100 or more full
static inline uint16_t md380_bl_cv(uint8_t percent)
{
	return percent >= 100u ? (uint16_t)MD380_BL_PERIOD : (uint16_t)(percent * (MD380_BL_PERIOD / 100u));
}

#endif
