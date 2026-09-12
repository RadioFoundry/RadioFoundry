// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 power latch, battery, volume knob, LEDs (docs/spec/hardware-dm1701.md sections 10.4 and 11).
// ADC1 scans volume, battery, mic level and the temperature sensor continuously; DMA2 Stream4 keeps the last
// SCANS results in a circular buffer, so a reading is a short average with no interrupt and no locking.
#include "dm1701.h"
#include "FreeRTOS.h"
#include "pins.h"
#include "hal_time.h"

enum { CH_VOLUME, CH_BATTERY, CH_MIC, CH_TEMP, CH_COUNT };
#define SCANS			16		// about 220 us per scan at 9 MHz ADC clock and 480-cycle sampling: 3.5 ms window

#define SETTLE_MS		1500		// readings settle after power-up (spec 11.1)
#define SWITCH_OFF_MV		5500		// below this the volume knob switch is off (spec 10.4)
#define SWITCH_OFF_SAMPLES	3		// consecutive low readings, at least SWITCH_SAMPLE_MS apart
#define SWITCH_SAMPLE_MS	10
#define FILTER_MS		10000		// time constant of the displayed battery voltage

// Volume pot (spec 11.2, DM-1701): log taper, the breakpoint sits near the middle of the rotation
#define VOL_MIN			26	// measured on the bench: 26 at the bottom, 2063 at the top (spec 29..2075)
#define VOL_BREAK		318
#define VOL_MAX			2060
#define VOL_MUTE_ZONE		5		// counts above the minimum that still read 0
#define VOL_HYSTERESIS		5

static volatile uint16_t samples[SCANS * CH_COUNT];
static bool started;
static uint32_t start_ms;
static uint32_t filtered_mv, filter_ms;
static uint8_t low_count;
static uint32_t low_ms;
static bool vol_muted = true;

static void adc_start(void)
{
	DMA2_Stream4->CR = 0;
	while (DMA2_Stream4->CR & DMA_SxCR_EN) {
	}
	DMA2->HIFCR = DMA_HIFCR_CTCIF4 | DMA_HIFCR_CHTIF4 | DMA_HIFCR_CTEIF4 | DMA_HIFCR_CDMEIF4 | DMA_HIFCR_CFEIF4;
	DMA2_Stream4->PAR = (uint32_t)&ADC1->DR;
	DMA_RAM_OK(samples);
	DMA2_Stream4->M0AR = (uint32_t)samples;
	DMA2_Stream4->NDTR = SCANS * CH_COUNT;
	DMA2_Stream4->FCR = 0;	// direct mode
	// Channel 0 (ADC1), peripheral to memory, 16-bit both sides, memory increment, circular, low priority
	DMA2_Stream4->CR = DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC | DMA_SxCR_CIRC;
	DMA2_Stream4->CR |= DMA_SxCR_EN;

	ADC1->CR2 &= ~ADC_CR2_ADON;
	ADC1->SR = 0;
	ADC1->CR2 = ADC_CR2_CONT | ADC_CR2_DMA | ADC_CR2_DDS | ADC_CR2_ADON;
	hal_delay_us(5);	// ADC stabilization time
	ADC1->CR2 |= ADC_CR2_SWSTART;
}

static void init(void)
{
	stm32_pin_analog(PIN_PORT(PIN_VOLUME), PIN_MASK(PIN_VOLUME) | PIN_MASK(PIN_BATTERY) | PIN_MASK(PIN_MIC_LEVEL));
	RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
	(void)RCC->AHB1ENR;

	ADC->CCR = ADC_CCR_ADCPRE_0 | ADC_CCR_ADCPRE_1 | ADC_CCR_TSVREFE;	// PCLK2 / 8, temperature sensor on
	ADC1->CR1 = ADC_CR1_SCAN;		// 12-bit
	ADC1->SMPR2 = (7u << ADC_SMPR2_SMP0_Pos) | (7u << ADC_SMPR2_SMP1_Pos) | (7u << ADC_SMPR2_SMP3_Pos);
	ADC1->SMPR1 = 7u << ADC_SMPR1_SMP16_Pos;	// 480 cycles everywhere (the sensor needs 10 us or more)
	ADC1->SQR1 = (CH_COUNT - 1u) << ADC_SQR1_L_Pos;
	ADC1->SQR2 = 0;
	ADC1->SQR3 = (0u << 0) | (1u << 5) | (3u << 10) | (16u << 15);	// ranks: IN0, IN1, IN3, IN16
	adc_start();

	start_ms = hal_millis();
	started = true;
}

// Average of the buffered results of one channel. Restarts the converter if an overrun stopped it
static uint16_t adc_read(uint8_t ch)
{
	if (!started) {
		return 0;
	}
	if ((ADC1->SR & ADC_SR_OVR) || !(DMA2_Stream4->CR & DMA_SxCR_EN)) {
		adc_start();
		hal_delay_us(SCANS * 230);
	}
	uint32_t sum = 0;
	for (uint32_t i = ch; i < SCANS * CH_COUNT; i += CH_COUNT) {
		sum += samples[i];
	}
	return (uint16_t)(sum / SCANS);
}

static bool settled(void)
{
	return started && (uint32_t)(hal_millis() - start_ms) >= SETTLE_MS;
}

// Battery in mV from the latest readings: raw / 37.5 in 0.1 V units (spec 11.1) = raw * 8 / 3 mV
static uint32_t battery_now_mv(void)
{
	return (uint32_t)adc_read(CH_BATTERY) * 8u / 3u;
}

static void latch(bool on)
{
	PIN_WRITE(PIN_POWER_LATCH, on);
}

// Smoothed for display (TX load and noise would make it jump); first value once the readings have settled
static uint16_t battery_mv(void)
{
	uint32_t now_mv = battery_now_mv();
	if (!settled()) {
		return (uint16_t)now_mv;
	}
	uint32_t now = hal_millis();
	if (filter_ms == 0) {
		filtered_mv = now_mv;
	} else {
		uint32_t dt = now - filter_ms;
		if (dt > FILTER_MS) {
			dt = FILTER_MS;
		}
		int32_t diff = (int32_t)now_mv - (int32_t)filtered_mv;
		filtered_mv = (uint32_t)((int32_t)filtered_mv + diff * (int32_t)dt / FILTER_MS);
	}
	filter_ms = now ? now : 1;
	return (uint16_t)filtered_mv;
}

// 2S Li-ion, resting voltage to charge (per cell 4.10 V full .. 3.20 V empty, doubled)
static const struct { uint16_t mv; uint8_t pct; } curve[] = {
	{ 6400, 0 }, { 6800, 4 }, { 7000, 10 }, { 7200, 22 }, { 7400, 42 },
	{ 7600, 60 }, { 7800, 75 }, { 8000, 88 }, { 8200, 100 },
};

static uint8_t battery_pct(void)
{
	uint32_t mv = battery_mv();
	const uint32_t n = sizeof(curve) / sizeof(curve[0]);
	if (mv <= curve[0].mv) {
		return 0;
	}
	for (uint32_t i = 1; i < n; i++) {
		if (mv < curve[i].mv) {
			uint32_t span = curve[i].mv - curve[i - 1].mv;
			uint32_t rise = curve[i].pct - curve[i - 1].pct;
			return (uint8_t)(curve[i - 1].pct + (mv - curve[i - 1].mv) * rise / span);
		}
	}
	return 100;
}

// Two linear segments: minimum..breakpoint is 0..50 %, breakpoint..maximum is 50..100 %. The bottom reads 0 with
// a little hysteresis so a knob resting at the stop doesn't flicker
static int8_t volume(void)
{
	int32_t raw = adc_read(CH_VOLUME);
	int32_t mute_edge = VOL_MIN + VOL_MUTE_ZONE + (vol_muted ? VOL_HYSTERESIS : 0);
	vol_muted = raw <= mute_edge;
	if (vol_muted) {
		return 0;
	}
	int32_t pct;
	if (raw < VOL_BREAK) {
		pct = (raw - VOL_MIN) * 50 / (VOL_BREAK - VOL_MIN);
	} else {
		pct = 50 + (raw - VOL_BREAK) * 50 / (VOL_MAX - VOL_BREAK);
	}
	if (pct < 1) {
		pct = 1;	// out of the mute zone always means some volume
	} else if (pct > 100) {
		pct = 100;
	}
	return (int8_t)pct;
}

// The volume knob switch cuts the battery sense line: a few low readings in a row mean the user switched off
static bool switch_on(void)
{
	if (!settled()) {
		return true;
	}
	uint32_t now = hal_millis();
	if (battery_now_mv() >= SWITCH_OFF_MV) {
		low_count = 0;
		return true;
	}
	if (low_count == 0 || (uint32_t)(now - low_ms) >= SWITCH_SAMPLE_MS) {
		if (low_count < SWITCH_OFF_SAMPLES) {
			low_count++;
		}
		low_ms = now;
	}
	return low_count < SWITCH_OFF_SAMPLES;
}

static void led(uint8_t which, bool on)
{
	if (which == LED_GREEN) {
		PIN_WRITE(PIN_LED_GREEN, on);
	} else {
		PIN_WRITE(PIN_LED_RED, on);
	}
}

// Mic level (spec 11.3), raw 0..4095, for VOX in the core (dB conversion there)
uint16_t dm1701_mic_level_raw(void)
{
	return adc_read(CH_MIC);
}

// Internal temperature in 0.1 C, coarse and uncalibrated (spec 11.4: 943 counts at 25 C, 3.1 counts per C)
int16_t dm1701_temperature_c10(void)
{
	int32_t raw = adc_read(CH_TEMP);
	return (int16_t)(250 + (raw - 943) * 100 / 31);
}

// Mic level 0..100 for VOX (hal_power_t.mic_level): 20 log10 of the raw reading over the ADC's 72 dB, from an integer
// log2 (msb plus a linear mantissa, within 0.1 bit). Uncalibrated: the VOX thresholds want a bench check
static uint8_t mic_level(void)
{
	uint32_t x = adc_read(CH_MIC);
	if (x == 0) {
		return 0;
	}
	uint32_t msb = 31u - (uint32_t)__builtin_clz(x);
	uint32_t log2_q8 = msb * 256u + ((x << 8) >> msb) - 256u;	// log2(x) x 256
	uint32_t level = log2_q8 * 100u / (12u * 256u);			// 4095 -> 12 bits -> 100
	return (uint8_t)(level > 100u ? 100u : level);
}

const hal_power_t dm1701_power = {
	.init = init,
	.latch = latch,
	.battery_mv = battery_mv,
	.battery_pct = battery_pct,
	.volume = volume,
	.switch_on = switch_on,
	.led = led,
	.mic_level = mic_level,
};

uint16_t dm1701_adc_raw(uint8_t ch)
{
	return ch < CH_COUNT ? adc_read(ch) : 0;
}
