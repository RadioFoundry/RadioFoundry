// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-88 SoC A power key and latch, battery and volume knob on the ADC, status LEDs (docs/spec/hardware-gd88.md 6.1).
//
// PTB12 reads high while the power key is pressed; the latch (PTB13) keeps the radio on. The radio is switched on with
// the volume knob, so PTB12 is most likely the knob's switch contact; it is handled as a key all the same: once seen
// low, a high reading switches the radio off. A knob that holds the line high while on never does that. ADC channel 1
// is the knob's position and channel 2 the battery (2S Li-ion): V = (raw + 5.7818) / 7.8551 / 10. A battery reading
// below 50 means the supply is going: the radio switches off. The LEDs are on SoC B (gd88_rx2_led).
#include "gd88.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"

// TODO(gd88): unknown, needs a radio: whether PTB12 is the knob's contact or a key
#define KEY_OFF_POLLS		3u	// pressed this many polls in a row (100 ms each) before believing it
#define BAT_GONE_RAW		50u
#define BAT_GONE_SAMPLES	10u	// a second of readings below it
#define BAT_FULL_MV		8400u
#define BAT_EMPTY_MV		6000u
#define BAT_CACHE_MS		500u
#define FILTER_SHIFT		3
// The knob: 21 positions over the ADC's range, with a little hysteresis so a knob resting on a boundary doesn't
// flicker between two. TODO(gd88): unknown, needs a radio: the knob's end readings (taken as 0 and 1023 here)
#define VOL_LEVELS		21u
#define VOL_FULL_RAW		1023u
#define VOL_HYST_RAW		12u

static uint32_t bat_mv_f, bat_ms;
static bool bat_valid;
static uint8_t off_count, gone_count;
static bool seen_on;		// the key was seen released
static uint8_t vol_level;

static uint32_t raw_to_mv(uint16_t raw)
{
	return ((uint32_t)raw * 10000u + 57818u) * 100u / 78551u;	// (raw + 5.7818) / 7.8551 x 100
}

static void init(void)
{
	hrc_adc_init();
	hrc_pin_out(PIN_POWER_LATCH, true);
	hrc_pin_in(PIN_POWER_KEY);
	bat_valid = false;
	off_count = 0;
	gone_count = 0;
	seen_on = false;
	vol_level = 0;
}

static void latch(bool on)
{
	hrc_pin_write(PIN_POWER_LATCH, on);
}

static uint16_t battery_mv(void)
{
	uint32_t now = hal_millis();
	if (!bat_valid || now - bat_ms >= BAT_CACHE_MS) {
		uint32_t mv = raw_to_mv(hrc_adc_read(GD88_ADC_BATTERY));
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

// The level whose band holds raw; a move of less than VOL_HYST_RAW past the current band's edge keeps it
static int8_t volume(void)
{
	uint32_t raw = hrc_adc_read(GD88_ADC_VOLUME);
	uint32_t band = (VOL_FULL_RAW + VOL_LEVELS - 1u) / VOL_LEVELS;
	uint32_t lo = vol_level * band;
	uint32_t hi = lo + band;
	if (raw + VOL_HYST_RAW < lo || raw >= hi + VOL_HYST_RAW) {
		uint32_t l = raw / band;
		vol_level = (uint8_t)(l >= VOL_LEVELS ? VOL_LEVELS - 1u : l);
	}
	return (int8_t)(vol_level * 100u / (VOL_LEVELS - 1u));
}

static bool switch_on(void)
{
	if (hrc_adc_read(GD88_ADC_BATTERY) < BAT_GONE_RAW) {
		if (gone_count < BAT_GONE_SAMPLES) {
			gone_count++;
		}
	} else {
		gone_count = 0;
	}
	if (gone_count >= BAT_GONE_SAMPLES) {
		return false;
	}
	// The key only counts once it has been seen released (it is held to switch on)
	if (!hrc_pin_read(PIN_POWER_KEY)) {
		seen_on = true;		// released
		off_count = 0;
	} else if (seen_on && off_count < KEY_OFF_POLLS) {
		off_count++;
	}
	return off_count < KEY_OFF_POLLS;
}

static void led(uint8_t which, bool on)
{
	gd88_rx2_led(which, on);
}

const hal_power_t gd88_power = {
	.init = init,
	.latch = latch,
	.battery_mv = battery_mv,
	.battery_pct = battery_pct,
	.volume = volume,
	.switch_on = switch_on,
	.led = led,
};
