// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-32UV power latch and switch, battery on ADC channel C, status LEDs (docs/spec/hardware-dm32.md 3.7, 6.4, 6.5).
// The volume knob isn't read by any firmware seen (it is on the audio amplifier), so volume() is -1.
#include "dm32.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"

// 2S Li-ion: raw / 8.2 = volts x 10 (raw 672 = 8.2 V full); 6.4 V is where the old port warned
#define BAT_FULL_MV		8200u
#define BAT_EMPTY_MV		6400u
#define BAT_CACHE_MS		500u
#define FILTER_SHIFT		3		// battery_mv() is an average over about 8 readings
#define SWITCH_SAMPLES		3

static uint32_t bat_mv_f, bat_ms;
static bool bat_valid;
static uint8_t off_count;

static void init(void)
{
	hrc_adc_init();
	hrc_pin_out(PIN_POWER_LATCH, true);
	hrc_pin_in(PIN_POWER_SWITCH);
	bat_valid = false;
	off_count = 0;
}

static void latch(bool on)
{
	hrc_pin_write(PIN_POWER_LATCH, on);
}

static uint16_t battery_mv(void)
{
	uint32_t now = hal_millis();
	if (!bat_valid || now - bat_ms >= BAT_CACHE_MS) {
		uint32_t mv = (uint32_t)hrc_adc_read(2) * 1000u / 82u;
		if (!bat_valid) {
			bat_mv_f = mv << FILTER_SHIFT;
			bat_valid = true;
		} else {
			bat_mv_f += mv - (bat_mv_f >> FILTER_SHIFT);
		}
		bat_ms = now;
	}
	return (uint16_t)(bat_mv_f >> FILTER_SHIFT);
}

static uint8_t battery_pct(void)
{
	uint32_t mv = battery_mv();
	if (mv <= BAT_EMPTY_MV) {
		return 0;
	}
	if (mv >= BAT_FULL_MV) {
		return 100;
	}
	return (uint8_t)((mv - BAT_EMPTY_MV) * 100u / (BAT_FULL_MV - BAT_EMPTY_MV));
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
	.mic_level = HRC_MIC_LEVEL,	// from the mic PCM: the ADC channel D read is suspect (hardware-dm32.md 3.7)
};
