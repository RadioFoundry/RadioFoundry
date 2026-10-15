// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// ADC (0x140D0000): one single-scan conversion of channel B (1) or C (2), 10 bits (docs/spec/hardware-dm32.md 3.7).
// A mutex serializes the readers once the scheduler runs.
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
#define ADC_DATA_AB		REG32(ADC_BASE + 0x30)
#define ADC_DATA_CD		REG32(ADC_BASE + 0x34)
#define ADC_SPINS		200000u

static SemaphoreHandle_t lock;
static uint16_t raw_last[3];

void hrc_adc_init(void)
{
	if (lock == NULL) {
		lock = xSemaphoreCreateMutex();
	}
	ADC_CTRL = 0;		// single scan, no glitch filter, not low power
}

static void take(void)
{
	if (lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreTake(lock, portMAX_DELAY);
	}
}

static void give(void)
{
	if (lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreGive(lock);
	}
}

// Single scan converts the lowest enabled channel only (3.7)
uint16_t hrc_adc_read(uint8_t ch)
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

uint16_t hrc_adc_last(uint8_t ch)
{
	return ch < 3 ? raw_last[ch] : 0;
}
