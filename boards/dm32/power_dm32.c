// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-32UV power latch and switch, battery on ADC channel C, status LEDs (docs/spec/hardware-dm32.md 3.7, 6.4, 6.5).
// The volume knob isn't read by any firmware seen (it is on the audio amplifier), so volume() is -1.
#include "dm32.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#define ADC_CTRL		REG32(ADC_BASE + 0x00)
#define ADC_INTR		REG32(ADC_BASE + 0x08)
#define ADC_INTR_STA		REG32(ADC_BASE + 0x0C)
#define ADC_START		REG32(ADC_BASE + 0x14)
#define ADC_CH_VLD		REG32(ADC_BASE + 0x24)
#define ADC_PD_SEOC_TIME	REG32(ADC_BASE + 0x28)
#define ADC_DATA_AB		REG32(ADC_BASE + 0x30)
#define ADC_DATA_CD		REG32(ADC_BASE + 0x34)
#define ADC_SPINS		200000u

// 2S Li-ion: raw / 8.2 = volts x 10 (raw 672 = 8.2 V full); 6.4 V is where the old port warned
#define BAT_FULL_MV		8200u
#define BAT_EMPTY_MV		6400u
#define BAT_CACHE_MS		500u
#define FILTER_SHIFT		3		// battery_mv() is an average over about 8 readings
#define SWITCH_SAMPLES		3

static SemaphoreHandle_t adc_lock;
static uint16_t raw_last[3];
static uint32_t bat_mv_f, bat_ms;
static bool bat_valid;
static uint8_t off_count;

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

// One single-scan conversion of channel 1 (B) or 2 (C); single scan converts the lowest enabled channel only (3.7)
static uint16_t adc_read(uint8_t ch)
{
	if (ch < 1 || ch > 2) {
		return 0;
	}
	take();
	uint32_t bit = 1u << ch;
	ADC_CH_VLD = bit;
	ADC_INTR = 0x10000u | bit;
	ADC_PD_SEOC_TIME = 0xA20u;
	ADC_INTR_STA = bit;	// clear a stale flag (write one to clear assumed; harmless otherwise)
	ADC_START = 1;
	uint16_t v = 0;
	for (uint32_t n = ADC_SPINS; n != 0; n--) {
		if (ADC_INTR_STA & bit) {
			v = (uint16_t)((ch == 1 ? ADC_DATA_AB >> 16 : ADC_DATA_CD) & 0x3FFu);
			break;
		}
	}
	ADC_INTR_STA = bit;
	give();
	raw_last[ch] = v;
	return v;
}

uint16_t dm32_adc_raw(uint8_t ch)
{
	return ch < 3 ? raw_last[ch] : 0;
}

static void init(void)
{
	if (adc_lock == NULL) {
		adc_lock = xSemaphoreCreateMutex();
	}
	hrc_pin_out(PIN_POWER_LATCH, true);
	hrc_pin_in(PIN_POWER_SWITCH);
	ADC_CTRL = 0;		// single scan, no glitch filter, not low power
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
		uint32_t mv = (uint32_t)adc_read(2) * 1000u / 82u;
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
};
