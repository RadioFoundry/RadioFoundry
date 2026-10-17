// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DB25-D power latch and power key, status LEDs, raw ADC readings (docs/spec/hardware-db25d.md 3.3).
//
// A mobile on a fixed 13.8 V supply: no battery (CAP_NO_BATTERY), so battery_mv and battery_pct stay NULL and the core
// shows no battery UI. Which ADC channel sees the supply, and at what scale, isn't known yet (10 #10), so there is
// no supply reading either; the diagnostics page shows the raw channels. Volume is digital (VOL key and encoder),
// so volume() is -1.
#include "db25d.h"
#include "pins.h"
#include "platform.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#define ADC_CTRL		REG32(ADC_BASE + 0x00)
#define ADC_INTR		REG32(ADC_BASE + 0x08)
#define ADC_INTR_STA		REG32(ADC_BASE + 0x0C)
#define ADC_START		REG32(ADC_BASE + 0x14)
#define ADC_CH_VLD		REG32(ADC_BASE + 0x24)
#define ADC_PD_SEOC_TIME	REG32(ADC_BASE + 0x28)
// Channel pairs: A/B at 0x30, C/D at 0x34 (hardware-dm32.md 3.7); E/F and G/H are taken to follow at 0x38 / 0x3C (INF)
#define ADC_DATA(ch)		REG32(ADC_BASE + 0x30u + 4u * ((ch) >> 1))
#define ADC_SPINS		200000u
#define ADC_CHANNELS		8u

// The power key is held to switch on, so it only counts once it has been seen released. Held for this many polls
// (switch_on runs every 100 ms in the start-up task's housekeeping loop: 2 s) it switches the radio off
#define POWER_OFF_POLLS		20u

static SemaphoreHandle_t adc_lock;
static bool key_released;
static uint16_t held;

static void take(void)
{
	if (adc_lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreTake(adc_lock, portMAX_DELAY);
	}
}

static void give(void)
{
	if (adc_lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreGive(adc_lock);
	}
}

// One single-scan conversion of one channel (single scan converts the lowest enabled channel only)
static uint16_t adc_read(uint8_t ch)
{
	if (ch >= ADC_CHANNELS) {
		return 0;
	}
	take();
	uint32_t bit = 1u << ch;
	ADC_CH_VLD = bit;
	ADC_INTR = 0x10000u | bit;
	ADC_PD_SEOC_TIME = 0xA20u;
	ADC_INTR_STA = bit;
	ADC_START = 1;
	uint16_t v = 0;
	for (uint32_t n = ADC_SPINS; n != 0; n--) {
		if (ADC_INTR_STA & bit) {
			v = (uint16_t)((ADC_DATA(ch) >> ((ch & 1u) * 16u)) & 0x3FFu);
			break;
		}
	}
	ADC_INTR_STA = bit;
	give();
	return v;
}

uint16_t db25d_adc_raw(uint8_t ch)
{
	return ch < ADC_CHANNELS ? adc_read(ch) : 0;
}

static void init(void)
{
	if (adc_lock == NULL) {
		adc_lock = xSemaphoreCreateMutex();
	}
	hrc_pin_out(PIN_POWER_LATCH, true);
	hrc_pin_in(PIN_POWER_KEY);
	ADC_CTRL = 0;		// single scan, no glitch filter, not low power
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
