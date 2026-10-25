// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-380 power latch, battery, LEDs and the ADC (docs/spec/hardware-md380.md section 10).
// ADC1 scans the volume knob position, battery, mic level, RSSI, the noise level and the temperature sensor
// continuously; DMA2 Stream4 keeps the last SCANS results in a circular buffer, so a reading is a short average with
// no interrupt and no locking. The volume knob sets the loudness in analog hardware, so hal volume() is -1.
#include "md380.h"
#include "FreeRTOS.h"
#include "pins.h"
#include "hal_time.h"
#include "radio/power_switch.h"
#include "audio/miclevel/miclevel.h"
#include "config.h"

enum { CH_VOLUME = MD380_ADC_VOLUME, CH_BATTERY = MD380_ADC_BATTERY, CH_MIC = MD380_ADC_MIC, CH_RSSI = MD380_ADC_RSSI,
	CH_BUSY = MD380_ADC_BUSY, CH_TEMP = MD380_ADC_TEMP, CH_COUNT = MD380_ADC_COUNT };
#define ADC_CLK_DIV		8u		// PCLK2 / 8 (ADC->CCR below)
#define ADC_CYCLES		(480u + 12u)	// sampling time and conversion, per channel
#if CONFIG_SUBTONE
// The sub-audio input is one more rank of the scan (about 2.6 kHz per channel at 72 MHz), streamed from the DMA
// half and full transfer interrupts. A longer ring: a half is 8 scans (3 ms), and the averaged readings cover 6 ms
#define SCANS			16
#define QT_IRQ_PRIORITY		7	// numerically >= 5: no RTOS calls in the handler, but below the modem interrupts
#else
#define SCANS			8		// about 330 us per scan at 9 MHz ADC clock and 480-cycle sampling: 2.7 ms window
#endif

#define SETTLE_MS		1500		// readings settle after power-up (experience on the other STM32 radios)
#define FILTER_MS		10000		// time constant of the displayed battery voltage

// Battery divider (hardware-md380.md 10.2, ?): raw counts per 0.1 V, x 10
#ifndef MD380_BATT_COUNTS_X10
#define MD380_BATT_COUNTS_X10	403
#endif

static volatile uint16_t samples[SCANS * CH_COUNT];
static bool started;
static uint32_t start_ms;
static uint32_t filtered_mv, filter_ms;
static pwrsw_t pwrsw;

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
#if CONFIG_SUBTONE
	DMA2_Stream4->CR |= DMA_SxCR_HTIE | DMA_SxCR_TCIE;
#endif
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
	stm32_pin_analog(PIN_PORT(PIN_RSSI), PIN_MASK(PIN_RSSI) | PIN_MASK(PIN_BUSY));
#if CONFIG_SUBTONE
	stm32_pin_analog(PIN_PORT(PIN_QT_IN), PIN_MASK(PIN_QT_IN));
	HAL_NVIC_SetPriority(DMA2_Stream4_IRQn, QT_IRQ_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(DMA2_Stream4_IRQn);
#endif
	RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
	(void)RCC->AHB1ENR;

	ADC->CCR = ADC_CCR_ADCPRE_0 | ADC_CCR_ADCPRE_1 | ADC_CCR_TSVREFE;	// PCLK2 / 8, temperature sensor on
	ADC1->CR1 = ADC_CR1_SCAN;		// 12-bit
	ADC1->SMPR2 = (7u << ADC_SMPR2_SMP0_Pos) | (7u << ADC_SMPR2_SMP1_Pos) | (7u << ADC_SMPR2_SMP3_Pos) |
		      (7u << ADC_SMPR2_SMP8_Pos) | (7u << ADC_SMPR2_SMP9_Pos);
	ADC1->SMPR1 = 7u << ADC_SMPR1_SMP16_Pos;	// 480 cycles everywhere (the sensor needs 10 us or more)
	ADC1->SQR1 = (CH_COUNT - 1u) << ADC_SQR1_L_Pos;
	ADC1->SQR2 = 0;
#if CONFIG_SUBTONE
	ADC1->SMPR2 |= 7u << ADC_SMPR2_SMP2_Pos;
	ADC1->SQR2 = (uint32_t)ADC_CH_QT << 0;	// the seventh rank
#endif
	// Ranks in the order of the MD380_ADC_ enum: IN0, IN1, IN3, IN8, IN9, IN16 (and IN2 with CONFIG_SUBTONE)
	ADC1->SQR3 = ((uint32_t)ADC_CH_VOLUME << 0) | ((uint32_t)ADC_CH_BATTERY << 5) | ((uint32_t)ADC_CH_MIC << 10) |
		     ((uint32_t)ADC_CH_RSSI << 15) | ((uint32_t)ADC_CH_BUSY << 20) | (16u << 25);
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
		hal_delay_us(SCANS * 340);
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

// Battery in mV from the latest readings: raw / 40.3 in 0.1 V units (?)
static uint32_t battery_now_mv(void)
{
	return (uint32_t)adc_read(CH_BATTERY) * 1000u / MD380_BATT_COUNTS_X10;
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

// The knob sets the loudness in analog hardware (the amplifier's input level), not through the MCU
static int8_t volume(void)
{
	return -1;
}

// The volume knob switch breaks the battery sense line while the latch holds the supply (hardware-md380.md 10.1):
// a few low readings in a row mean the user switched off
static bool switch_on(void)
{
	if (!settled()) {
		return true;
	}
	return pwrsw_on(&pwrsw, battery_now_mv(), hal_millis());
}

static void led(uint8_t which, bool on)
{
	if (which == LED_GREEN) {
		PIN_WRITE(PIN_LED_GREEN, on);
	} else {
		PIN_WRITE(PIN_LED_RED, on);
	}
}

// Internal temperature in 0.1 C, coarse and uncalibrated (STM32F405 data sheet typical values: 0.76 V at 25 C, 2.5 mV per C)
int16_t md380_temperature_c10(void)
{
	int32_t raw = adc_read(CH_TEMP);
	return (int16_t)(250 + (raw - 943) * 100 / 31);
}

// Mic level 0..100 for VOX (hal_power_t.mic_level): the raw reading on the 12-bit ADC's 72 dB log scale.
// Uncalibrated: the VOX thresholds want a bench check
static uint8_t mic_level(void)
{
	return miclevel_log(adc_read(CH_MIC), 12);
}

const hal_power_t md380_power = {
	.init = init,
	.latch = latch,
	.battery_mv = battery_mv,
	.battery_pct = battery_pct,
	.volume = volume,
	.switch_on = switch_on,
	.led = led,
	.mic_level = mic_level,
};

uint16_t md380_adc_raw(uint8_t ch)
{
	return ch < CH_COUNT ? adc_read(ch) : 0;
}

#if CONFIG_SUBTONE
// The scan rate of each channel: one pass over CH_COUNT ranks, each ADC_CYCLES long, on the ADC clock
uint32_t md380_adc_scan_rate_hz(void)
{
	return HAL_RCC_GetPCLK2Freq() / ADC_CLK_DIV / (CH_COUNT * ADC_CYCLES);
}

// Half or full transfer: the scans just written go to the sub-audio detector (the half the DMA is not writing)
void DMA2_Stream4_IRQHandler(void)
{
	uint32_t isr = DMA2->HISR;
	uint32_t first;
	if (isr & DMA_HISR_HTIF4) {
		DMA2->HIFCR = DMA_HIFCR_CHTIF4;
		first = 0;
	} else if (isr & DMA_HISR_TCIF4) {
		DMA2->HIFCR = DMA_HIFCR_CTCIF4;
		first = SCANS / 2;
	} else {
		DMA2->HIFCR = DMA_HIFCR_CTEIF4 | DMA_HIFCR_CDMEIF4 | DMA_HIFCR_CFEIF4;
		return;
	}
	for (uint32_t s = first; s < first + SCANS / 2; s++) {
		md380_subtone_sample(samples[s * CH_COUNT + MD380_ADC_QT]);
	}
}
#endif
