// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-380 backlight PWM (backlight_md380.h): the standard rate stays 40 kHz, the rates run fastest first, and the
// core's percent maps onto the compare value with 0 off and 100 full
#include <stdio.h>
#include "backlight_md380.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
	CHECK(md380_bl_rate_hz(MD380_BL_RATE_STD) == 40000u);
	CHECK(md380_bl_rate_hz(MD380_BL_RATES - 1u) == 100u);
	CHECK(md380_bl_rate_hz(200) == 100u);
	CHECK(MD380_BL_RATE_STD < MD380_BL_RATES);
	for (uint8_t i = 0; i < MD380_BL_RATES; i++) {
		CHECK(md380_bl_div[i] >= 1u && md380_bl_div[i] - 1u <= 0xFFFFu);	// a 16-bit prescaler
		if (i > 0) {
			CHECK(md380_bl_rate_hz(i) < md380_bl_rate_hz((uint8_t)(i - 1)));
		}
	}
	CHECK(MD380_BL_PERIOD - 1u <= 0xFFFFu);
	CHECK(md380_bl_cv(0) == 0);
	CHECK(md380_bl_cv(1) == 18);
	CHECK(md380_bl_cv(50) == MD380_BL_PERIOD / 2u);
	CHECK(md380_bl_cv(99) < MD380_BL_PERIOD);
	CHECK(md380_bl_cv(100) == MD380_BL_PERIOD);	// above ARR: on through the whole period
	CHECK(md380_bl_cv(255) == MD380_BL_PERIOD);
	if (failures) {
		printf("md380BacklightTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("md380BacklightTest: all passed\n");
	return 0;
}
