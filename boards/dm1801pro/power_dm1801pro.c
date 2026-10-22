// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1801 Pro power latch and key, battery on ADC channel C, status LEDs (docs/spec/hardware-dm1801pro.md 3, 9.2, 9.4).
// The battery scale is measured on one radio (103 counts per volt, hardware-dm1801pro.md 9.2). The power key's polarity
// is provisional: the level it has at start-up counts as "on".
#include "dm1801pro.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "power/battery/battery.h"
#include "FreeRTOS.h"
#include "timers.h"

#define BAT_FULL_MV		8200u
#define BAT_EMPTY_MV		6400u
#define BAT_COUNTS_PER_V	103u	// ADC channel 2: 856 counts with 8.306 V across the pack, radio on (2026-10-16)
#define SWITCH_SAMPLES		3

static battery_avg_t bat;
static bool on_level;
static uint8_t off_count;

static TimerHandle_t vibe_timer;

static void vibe_off(TimerHandle_t t)
{
	(void)t;
	hrc_pin_write(PIN_VIBRATION, false);
}

// The motor runs for ms and a software timer stops it, so the caller never waits. A call while it runs restarts the time
static void vibrate(uint16_t ms)
{
	if (vibe_timer == NULL || ms == 0) {
		return;
	}
	hrc_pin_write(PIN_VIBRATION, true);
	xTimerChangePeriod(vibe_timer, pdMS_TO_TICKS(ms), portMAX_DELAY);
}

static void init(void)
{
	if (vibe_timer == NULL) {
		vibe_timer = xTimerCreate("vibe", 1, pdFALSE, NULL, vibe_off);
	}
	hrc_adc_init();
	hrc_pin_out(PIN_POWER_LATCH, true);
	hrc_pin_in(PIN_POWER_KEY);
	on_level = hrc_pin_read(PIN_POWER_KEY);
	bat.valid = false;
	off_count = 0;
}

static void latch(bool on)
{
	hrc_pin_write(PIN_POWER_LATCH, on);
}

uint16_t dm1801pro_battery_raw(void)
{
	return hrc_adc_last(2);
}

static uint32_t battery_read_mv(void)
{
	return (uint32_t)hrc_adc_read(2) * 1000u / BAT_COUNTS_PER_V;
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

// The key's level changed from its start-up level for three readings in a row
static bool switch_on(void)
{
	if (hrc_pin_read(PIN_POWER_KEY) != on_level) {
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

const hal_power_t dm1801pro_power = {
	.init = init,
	.latch = latch,
	.battery_mv = battery_mv,
	.battery_pct = battery_pct,
	.volume = volume,
	.switch_on = switch_on,
	.led = led,
	.mic_level = HRC_MIC_LEVEL,	// the mic PCM; no analog level tap on this radio
	.vibrate = vibrate,
};
