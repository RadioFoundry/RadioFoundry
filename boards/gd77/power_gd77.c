// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 power hold (PTE26) and power knob switch (PTA13), battery on ADC0, LEDs (docs/spec/hardware-gd77.md 9). The
// battery channel and its scale are unverified: the reading is the ADC pin voltage times a provisional divider, and the
// board sets no low-battery thresholds until a radio has been measured. The LED pins are the likelier pair of two
#include "gd77.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "power/battery/battery.h"

#define ADC_REF_MV		3300u	// VDDA (unverified)
#define BAT_DIVIDER_X10		30u	// provisional 3:1 divider: replace with the radio's measured scale
#define BAT_FULL_MV		8200u	// two Li-ion cells
#define BAT_EMPTY_MV		6600u
#define BAT_CACHE_MS		500u
#define SWITCH_SAMPLES		3
#define OFF_RESET_MS		500u	// after the hold drops with the knob off: the supply should be gone by then
#define RCM_SRS1_SW		(1u << 10)	// k22f_reset_cause(): the last reset was a software one

static bool adc_ok;
static uint16_t bat_raw;
static uint16_t dp2_raw = 0xFFFFu, dp3_raw = 0xFFFFu;	// the two open analog roles, read with the battery
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

// Off: the hold line low. With the knob off that cuts the supply; if something still powers the radio after
// OFF_RESET_MS (unverified: a supply other than the battery), a reset brings it back to its start-up state instead of
// leaving it dark. Not twice in a row: when this start was already that reset, the radio stays down and dark until
// the knob turns it on, rather than restarting every few seconds. With the knob on, the core carries on (app.c)
static void latch(bool on)
{
	k22f_pin_write(PIN_POWER_HOLD, on);
	if (on || k22f_pin_read(PIN_POWER_SWITCH)) {
		return;
	}
	hal_delay_ms(OFF_RESET_MS);
	if (!k22f_pin_read(PIN_POWER_SWITCH) && (k22f_reset_cause() & RCM_SRS1_SW) == 0) {
		k22f_reset();
	}
}

uint16_t gd77_battery_raw(void)
{
	return bat_raw;
}

uint16_t gd77_adc_raw(uint8_t ch)
{
	return ch == ADC_CH_BATTERY ? bat_raw : (ch == ADC_CH_DP2 ? dp2_raw : (ch == ADC_CH_DP3 ? dp3_raw : 0xFFFFu));
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
		// Here, on the one task that reads the ADC, so no other conversion runs at the same time
		dp2_raw = k22f_adc_read(0, ADC_CH_DP2);
		dp3_raw = k22f_adc_read(0, ADC_CH_DP3);
		bat_ms = now;
	}
	if (!bat_valid) {
		return 0;
	}
	return (uint16_t)((uint32_t)bat_raw * ADC_REF_MV * BAT_DIVIDER_X10 / (4095u * 10u));
}

static uint8_t battery_pct(void)
{
	return battery_linear_pct(battery_mv(), BAT_EMPTY_MV, BAT_FULL_MV);
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
