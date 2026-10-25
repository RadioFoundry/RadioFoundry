// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DB25-D power latch and power key, status LEDs (docs/spec/hardware-db25d.md 3.3).
//
// A mobile on a fixed 13.8 V supply: no battery (CAP_NO_BATTERY), so battery_mv and battery_pct stay NULL and the core
// shows no battery UI. Which ADC channel sees the supply, and at what scale, isn't known yet (10 #10), so there is
// no supply reading either; the diagnostics page shows the raw channels. Volume is digital (VOL key and encoder),
// so volume() is -1.
#include "db25d.h"
#include "pins.h"
#include "platform.h"

// The power key is held to switch on, so it only counts once it has been seen released. Held for this many polls
// (switch_on runs every 100 ms in the start-up task's housekeeping loop: 2 s) it switches the radio off
#define POWER_OFF_POLLS		20u

static bool key_released;
static uint16_t held;

static void init(void)
{
	hrc_adc_init();
	hrc_pin_out(PIN_POWER_LATCH, true);
	hrc_pin_in(PIN_POWER_KEY);
	key_released = false;
	held = 0;
}

static void latch(bool on)
{
	hrc_pin_write(PIN_POWER_LATCH, on);
}

static int8_t volume(void)
{
	return -1;
}

static bool switch_on(void)
{
	bool down = !hrc_pin_read(PIN_POWER_KEY);
	if (!down) {
		key_released = true;
		held = 0;
	} else if (key_released && held < POWER_OFF_POLLS) {
		held++;
	}
	return held < POWER_OFF_POLLS;
}

static void led(uint8_t which, bool on)
{
	hrc_pin_write(which == LED_RED ? PIN_LED_RED : PIN_LED_GREEN, on);
}

const hal_power_t db25d_power = {
	.init = init,
	.latch = latch,
	.volume = volume,
	.switch_on = switch_on,
	.led = led,
};
