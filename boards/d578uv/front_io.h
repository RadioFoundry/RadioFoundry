// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV: the plain hand microphone's two key lines and the backlight level (docs/spec/hardware-d578uv.md 3.2,
// 3.1). Pure, so the host tests check them; input_d578uv.c and radio_d578uv.c apply them.
#ifndef D578UV_FRONT_IO_H
#define D578UV_FRONT_IO_H

#include <stdbool.h>
#include <stdint.h>

// --- Plain microphone (PA9, PA10; pull-ups, low = pulled by the microphone) ---
// The stock decoding: PA10 low alone is PTT, PA9 low alone the microphone's Up key, both low its Down key. Up and Down
// are inferred from the stock key codes (0x10, 0x11), not seen on a microphone. A key that pulls both lines can show
// PTT for an instant while its lines settle; the core's debounce (two agreeing scans) drops that.
typedef enum {
	MIC_KEY_NONE,
	MIC_KEY_PTT,
	MIC_KEY_UP,
	MIC_KEY_DOWN,
} mic_key_t;

static inline mic_key_t mic_key_decode(bool pa9_low, bool pa10_low)
{
	if (pa10_low) {
		return pa9_low ? MIC_KEY_DOWN : MIC_KEY_PTT;
	}
	return pa9_low ? MIC_KEY_UP : MIC_KEY_NONE;
}

// --- Backlight: register 8 of the PA5 device ---
// The stock "Back Light" setting has five levels (codes, dimmest first); 0 is off, as at the stock power-off and
// standby. Four of the five bytes sit close together, so brightness is likely far from linear in the byte (inferred),
// and no other byte is known to look right: the core's percent picks one of the five. 0 % is off, 1-20 % the first
// level, 21-40 % the second and so on, 81-100 % the brightest (two of the core's 10 % brightness steps per level)
static inline uint8_t backlight_code(uint8_t percent)
{
	static const uint8_t codes[5] = { 0x38, 0x3D, 0x42, 0x4B, 0xFF };
	if (percent == 0) {
		return 0;
	}
	if (percent > 100u) {
		percent = 100u;
	}
	return codes[(percent - 1u) / 20u];
}

#endif
