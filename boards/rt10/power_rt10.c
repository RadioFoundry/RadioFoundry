// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RT10 power latch and switch, battery on ADC channel 2, status LEDs (docs/spec/hardware-rt10.md 3, 5). The volume
// knob is on the audio path, not read by the MCU, so volume() is -1. The power switch reads high when off (measured);
// switch_on() turns false after SWITCH_SAMPLES polls in a row read off (app.c polls every 100 ms: the original
// firmware's 500 ms), and app.c then lets go of the latch. battery_mv() is the ADC count times BAT_MV_PER_COUNT
#include "rt10.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "power/battery/battery.h"

#define BAT_MV_PER_COUNT	10u
#define BAT_FULL_MV		8200u
#define BAT_EMPTY_MV		6400u
#define SWITCH_SAMPLES		5

static battery_avg_t bat;
static uint8_t off_count;

static void init(void)
{
	hrc_adc_init();
	hrc_pin_out(PIN_POWER_LATCH, true);
	hrc_pin_in(PIN_POWER_SWITCH);
	off_count = 0;
	bat.valid = false;
}

static void latch(bool on)
{
	hrc_pin_write(PIN_POWER_LATCH, on);
}

static uint32_t battery_read_mv(void)
{
	return (uint32_t)hrc_adc_read(2) * BAT_MV_PER_COUNT;
}

static uint16_t battery_mv(void)
{
	return battery_avg_mv(&bat, hal_millis(), battery_read_mv);
}

static uint8_t battery_pct(void)
{
	return battery_linear_pct(battery_mv(), BAT_EMPTY_MV, BAT_FULL_MV);
}

static int8_t volume(void)
{
	return -1;
}

static bool switch_on(void)
{
	if (hrc_pin_read(PIN_POWER_SWITCH)) {
		if (off_count < SWITCH_SAMPLES) {
			off_count++;
		}
	} else {
		off_count = 0;
	}
	return off_count < SWITCH_SAMPLES;
}

static void led(uint8_t which, bool on)
{
	hrc_pin_write(which == LED_RED ? PIN_LED_RED : PIN_LED_GREEN, on);
}

const hal_power_t rt10_power = {
	.init = init,
	.latch = latch,
	.battery_mv = battery_mv,
	.battery_pct = battery_pct,
	.volume = volume,
	.switch_on = switch_on,
	.led = led,
};
