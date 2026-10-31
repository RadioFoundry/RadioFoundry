// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AT-D578UV's microphone key lines and backlight levels (front_io.h, hardware-d578uv.md 3.1, 3.2): every line
// combination, and the percent to register byte map at its edges
#include <stdio.h>
#include "front_io.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_mic(void)
{
	CHECK(mic_key_decode(false, false) == MIC_KEY_NONE);	// both lines high: no microphone, or nothing pressed
	CHECK(mic_key_decode(false, true) == MIC_KEY_PTT);
	CHECK(mic_key_decode(true, false) == MIC_KEY_UP);
	CHECK(mic_key_decode(true, true) == MIC_KEY_DOWN);
}

static void test_backlight(void)
{
	CHECK(backlight_code(0) == 0);
	CHECK(backlight_code(1) == 0x38);
	CHECK(backlight_code(10) == 0x38);
	CHECK(backlight_code(20) == 0x38);
	CHECK(backlight_code(21) == 0x3D);
	CHECK(backlight_code(40) == 0x3D);
	CHECK(backlight_code(50) == 0x42);
	CHECK(backlight_code(70) == 0x4B);
	CHECK(backlight_code(80) == 0x4B);
	CHECK(backlight_code(81) == 0xFF);
	CHECK(backlight_code(100) == 0xFF);
	CHECK(backlight_code(255) == 0xFF);
	// Never darker as the percent rises, and never off above 0
	for (unsigned p = 1; p <= 100; p++) {
		CHECK(backlight_code((uint8_t)p) >= backlight_code((uint8_t)(p - 1)));
		CHECK(backlight_code((uint8_t)p) != 0);
	}
}

int main(void)
{
	test_mic();
	test_backlight();
	if (failures) {
		printf("d578uvFrontIoTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("d578uvFrontIoTest: all passed\n");
	return 0;
}
