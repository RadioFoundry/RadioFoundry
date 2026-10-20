// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 power hold (PTE26) and power knob switch (PTA13), battery on ADC0, LEDs (docs/spec/hardware-gd77.md 9). The
// battery channel and its scale are unverified: the reading is the ADC pin voltage times a provisional divider, and the
// board sets no low-battery thresholds until a radio has been measured. The LED pins are the likelier pair of two
#include "gd77.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"

#define ADC_REF_MV		3300u	// VDDA (unverified)
#define BAT_DIVIDER_X10		30u	// provisional 3:1 divider: replace with the radio's measured scale
#define BAT_FULL_MV		8200u	// two Li-ion cells
#define BAT_EMPTY_MV		6600u
#define BAT_CACHE_MS		500u
#define SWITCH_SAMPLES		3

static bool adc_ok;
static uint16_t bat_raw;
static uint32_t bat_ms;
static bool bat_valid;
static uint8_t off_count;

static void init(void)
{
	k22f_pin_write(PIN_POWER_HOLD, true);
	adc_ok = k22f_adc_init(0) == K22F_OK;
	bat_valid = false;
	off_count = 0;
}

static void latch(bool on)
{
	k22f_pin_write(PIN_POWER_HOLD, on);
}

uint16_t gd77_battery_raw(void)
{
	return bat_raw;
}

static uint16_t battery_mv(void)
{
	uint32_t now = hal_millis();
	if (adc_ok && (!bat_valid || now - bat_ms >= BAT_CACHE_MS)) {
		uint16_t raw = k22f_adc_read(0, ADC_CH_BATTERY);
		if (raw != 0xFFFFu) {
			bat_raw = raw;
			bat_valid = true;
		}
		bat_ms = now;
	}
	if (!bat_valid) {
		return 0;
	}
	return (uint16_t)((uint32_t)bat_raw * ADC_REF_MV * BAT_DIVIDER_X10 / (4095u * 10u));
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
	return -1;	// the volume knob is analog, on the audio path
}

// The knob switch reads low for three polls in a row: the radio is being switched off
static bool switch_on(void)
{
	if (!k22f_pin_read(PIN_POWER_SWITCH)) {
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
	k22f_pin_write(which == LED_RED ? PIN_LED_B : PIN_LED_A, on);
}

const hal_power_t gd77_power = {
	.init = init,
	.latch = latch,
	.battery_mv = battery_mv,
	.battery_pct = battery_pct,
	.volume = volume,
	.switch_on = switch_on,
	.led = led,
};
