// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Power, battery, volume knob and status LEDs
#ifndef HAL_POWER_H
#define HAL_POWER_H

#include <stdint.h>
#include <stdbool.h>

enum { LED_GREEN, LED_RED };

typedef struct {
	void (*init)(void);
	// Keep the radio powered (true) or let it turn off (false)
	void (*latch)(bool on);
	uint16_t (*battery_mv)(void);
	// Battery percentage 0..100 from the board's own curve
	uint8_t (*battery_pct)(void);
	// Volume knob position 0..100, or -1 if volume is not a knob the MCU reads
	int8_t (*volume)(void);
	// False once the user has switched the radio off (the core then saves and calls latch(false))
	bool (*switch_on)(void);
	void (*led)(uint8_t which, bool on);
	// Optional: microphone level 0..100, logarithmic (about 0.7 dB per step, 0 = silence), for VOX
	uint8_t (*mic_level)(void);
	// Optional: run the vibration motor for ms milliseconds and return at once (a call while it runs restarts the time);
	// NULL when the radio has no motor. Nothing in the core calls it yet
	void (*vibrate)(uint16_t ms);
} hal_power_t;

#endif
