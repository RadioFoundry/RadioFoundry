// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AT-D578UV's microphone key lines, backlight levels and speaker amplifier (front_io.h, hardware-d578uv.md 3.1,
// 3.2, 4.4): every line combination, the percent to register byte map at its edges, every amplifier input
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

static void test_amp(void)
{
	CHECK(!spk_amp_off(false, true, true));		// receiving, something plays, knob up: on
	CHECK(spk_amp_off(false, false, true));		// nothing plays: off, so no hiss with the squelch shut
	CHECK(spk_amp_off(false, true, false));		// the knob at 0
	CHECK(spk_amp_off(true, true, true));		// transmitting
	CHECK(spk_amp_off(true, false, false));
}

static void test_hang(void)
{
	spk_hang_t h = { 0 };
	CHECK(!spk_hang(&h, false, false, true, 1000));		// never on: stays off
	CHECK(spk_hang(&h, true, true, true, 1000));		// a beep
	CHECK(spk_hang(&h, false, false, true, 1100));		// beep over: the hang starts
	CHECK(spk_hang(&h, false, false, true, 1100 + SPK_HANG_BEEP_MS - 1));
	CHECK(!spk_hang(&h, false, false, true, 1100 + SPK_HANG_BEEP_MS));	// run out
	CHECK(!h.on && !h.hanging);
	// A second beep inside the hang keeps it on and starts the hang again from its end
	CHECK(spk_hang(&h, true, true, true, 2000));
	CHECK(spk_hang(&h, false, false, true, 2050));
	CHECK(spk_hang(&h, true, true, true, 2300));
	CHECK(spk_hang(&h, false, false, true, 2350));
	CHECK(spk_hang(&h, false, false, true, 2350 + SPK_HANG_BEEP_MS - 1));
	CHECK(!spk_hang(&h, false, false, true, 2350 + SPK_HANG_BEEP_MS));
	// Received audio: the shorter hang
	CHECK(spk_hang(&h, true, false, true, 3000));
	CHECK(spk_hang(&h, false, false, true, 3500));
	CHECK(!spk_hang(&h, false, false, true, 3500 + SPK_HANG_RX_MS));
	// Transmitting or the volume at 0: off at once, no hang
	CHECK(spk_hang(&h, true, true, true, 4000));
	CHECK(!spk_hang(&h, false, false, false, 4001));
	CHECK(!h.on);
	// The clock wrapping inside a hang
	CHECK(spk_hang(&h, true, false, true, 0xFFFFFF00u));
	CHECK(spk_hang(&h, false, false, true, 0xFFFFFF80u));
	CHECK(spk_hang(&h, false, false, true, 0x00000010u));
	CHECK(!spk_hang(&h, false, false, true, 0xFFFFFF80u + SPK_HANG_RX_MS + 1u));
}

int main(void)
{
	test_mic();
	test_backlight();
	test_amp();
	test_hang();
	if (failures) {
		printf("d578uvFrontIoTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("d578uvFrontIoTest: all passed\n");
	return 0;
}
