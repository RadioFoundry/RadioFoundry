// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-32UV power latch and switch, battery on ADC channel C, status LEDs (docs/spec/hardware-dm32.md 3.7, 6.4, 6.5).
// The volume knob isn't read by any firmware seen (it is on the audio amplifier), so volume() is -1.
#include "dm32.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "power/battery/battery.h"

// 2S Li-ion: raw / 8.32 = volts x 10. The port's raw / 8.2 read 8.21 V where a meter on the pack read 8.09 V (radio
// on vs. off, about 30 mV of load), so 8.32 (hardware-dm32.md 3.7). 6.4 V is where the old port warned
#define BAT_RAW_PER_10V		832u		// ADC counts per 10 V
#define BAT_FULL_MV		8200u
#define BAT_EMPTY_MV		6400u
#define SWITCH_SAMPLES		3

static battery_avg_t bat;
static uint8_t off_count;

static void init(void)
{
	hrc_adc_init();
	hrc_pin_out(PIN_POWER_LATCH, true);
	hrc_pin_in(PIN_POWER_SWITCH);
	bat.valid = false;
	off_count = 0;
}

static void latch(bool on)
{
	hrc_pin_write(PIN_POWER_LATCH, on);
}

static uint32_t battery_read_mv(void)
{
	return (uint32_t)hrc_adc_read(2) * 10000u / BAT_RAW_PER_10V;
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

// GPIO 0x10 reads 1 once the knob is turned off; three readings in a row before believing it
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

const hal_power_t dm32_power = {
	.init = init,
	.latch = latch,
	.battery_mv = battery_mv,
	.battery_pct = battery_pct,
	.volume = volume,
	.switch_on = switch_on,
	.led = led,
};
