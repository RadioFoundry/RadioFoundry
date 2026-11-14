// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family backlight PWM timing (display_uvf.c, docs/spec/hardware-dm1701.md 9.4). Pure, so the host tests check it.
#ifndef BACKLIGHT_UVF_H
#define BACKLIGHT_UVF_H

#include <stdint.h>

// The backlight pin has no timer channel. TIM1's update sets it and its CC1 match clears it, each through a one-word
// DMA write to the port's BSRR: two writes per cycle at any duty. Two rates (hardware-dm1701.md 9.4):
// - While transmitting, 40 kHz keeps the ripple's FM sidebands far below the carrier and three channels away; at
//   101 Hz it was an audible buzz in FM transmit, at 10 kHz sidebands about 33 dB down in the adjacent channel (#330).
// - Otherwise 2 kHz: the backlight driver doesn't follow 40 kHz, so the panel barely dimmed (Brightness 20 % looked
//   close to full, and the idle dim step hardly showed); at 2 kHz it dims as set, with no visible flicker.
#define UVF_BL_TIMER_HZ		72000000u
#define UVF_BL_PWM_HZ		40000u		// while transmitting
#define UVF_BL_PERIOD		(UVF_BL_TIMER_HZ / UVF_BL_PWM_HZ)	// timer counts per cycle (1800)
#define UVF_BL_IDLE_HZ		2000u		// otherwise
#define UVF_BL_IDLE_PERIOD	(UVF_BL_TIMER_HZ / UVF_BL_IDLE_HZ)	// (36000)

// CC1 value for a brightness at a period (UVF_BL_PERIOD or UVF_BL_IDLE_PERIOD): the count at which the pin clears,
// percent of the period. 0 = off, period = fully on (the caller drives the pin steady for both)
static inline uint16_t uvf_bl_compare(uint8_t percent, uint16_t period)
{
	if (percent == 0) {
		return 0;
	}
	if (percent >= 100) {
		return period;
	}
	return (uint16_t)((uint32_t)percent * period / 100u);
}

#endif
