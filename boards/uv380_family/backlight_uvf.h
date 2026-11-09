// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family backlight PWM timing (display_uvf.c, docs/spec/hardware-dm1701.md 9.4). Pure, so the host tests check it.
#ifndef BACKLIGHT_UVF_H
#define BACKLIGHT_UVF_H

#include <stdint.h>

// The backlight pin has no timer channel. TIM1's update sets it and its CC1 match clears it, each through a one-word
// DMA write to the port's BSRR: two writes per cycle at any duty. 40 kHz keeps the ripple's FM sidebands far below the
// carrier and three channels away; at 101 Hz it was an audible buzz in FM transmit, at 10 kHz sidebands about 33 dB
// down in the adjacent channel (#330).
#define UVF_BL_TIMER_HZ		72000000u
#define UVF_BL_PWM_HZ		40000u
#define UVF_BL_PERIOD		(UVF_BL_TIMER_HZ / UVF_BL_PWM_HZ)	// timer counts per cycle (1800)

// CC1 value for a brightness: the count at which the pin clears, percent of the period (1 % is 18 counts).
// 0 = off, UVF_BL_PERIOD = fully on (the caller drives the pin steady for both)
static inline uint16_t uvf_bl_compare(uint8_t percent)
{
	if (percent == 0) {
		return 0;
	}
	if (percent >= 100) {
		return (uint16_t)UVF_BL_PERIOD;
	}
	return (uint16_t)((uint32_t)percent * UVF_BL_PERIOD / 100u);
}

#endif
