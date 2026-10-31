// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// ADC0, one channel at a time by software trigger: a board's key ladders, knobs and supply sense. A conversion takes
// a few microseconds, so callers from any task share it inside a short critical section.
//
// The sampling channel: one more channel on the inserted group, which has its own trigger and data register, so an
// interrupt can sample audio at a steady rate while tasks go on reading. An inserted conversion started during a
// regular one interrupts it, and the regular one restarts after it (the STM32F1's ADC behavior the GD32 keeps); the
// regular data register is never touched
#include "platform.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"

static bool ready;

void gd32_adc_init(void)
{
	if (ready) {
		return;
	}
	RCU_APB2EN |= RCU_APB2EN_ADC0EN;
	ADC_CTL1(ADC0_BASE) = ADC_CTL1_ETERC | ADC_CTL1_ETSRC_SW;
	// The longest sample time on every channel (239.5 cycles): the ladders and dividers have high source impedance
	ADC_SAMPT0(ADC0_BASE) = 0x00FFFFFFu;
	ADC_SAMPT1(ADC0_BASE) = 0x3FFFFFFFu;
	ADC_RSQ0(ADC0_BASE) = 0;	// one conversion in the regular sequence
	ADC_CTL1(ADC0_BASE) |= ADC_CTL1_ADCON;
	hal_delay_ms(1);
	ADC_CTL1(ADC0_BASE) |= ADC_CTL1_RSTCLB;
	while (ADC_CTL1(ADC0_BASE) & ADC_CTL1_RSTCLB) {
	}
	ADC_CTL1(ADC0_BASE) |= ADC_CTL1_CLB;
	while (ADC_CTL1(ADC0_BASE) & ADC_CTL1_CLB) {
	}
	ready = true;
}

uint16_t gd32_adc_read(uint8_t ch)
{
	if (!ready) {
		return 0;
	}
	bool rtos = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
	if (rtos) {
		taskENTER_CRITICAL();
	}
	ADC_RSQ2(ADC0_BASE) = ch & 0x1Fu;
	ADC_STAT(ADC0_BASE) = 0;
	ADC_CTL1(ADC0_BASE) |= ADC_CTL1_SWRCST;
	uint32_t n = 0;
	while (!(ADC_STAT(ADC0_BASE) & ADC_STAT_EOC) && ++n < 100000u) {
	}
	uint16_t v = (uint16_t)(ADC_RDATA(ADC0_BASE) & 0xFFFu);
	if (rtos) {
		taskEXIT_CRITICAL();
	}
	return v;
}

void gd32_adc_sample_init(uint8_t ch)
{
	gd32_adc_init();
	bool rtos = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
	if (rtos) {
		taskENTER_CRITICAL();
	}
	ADC_ISQ(ADC0_BASE) = (uint32_t)(ch & 0x1Fu) << 15;	// IL 0: one conversion
	ADC_CTL1(ADC0_BASE) |= ADC_CTL1_ETEIC | ADC_CTL1_ETSIC_SW;
	if (rtos) {
		taskEXIT_CRITICAL();
	}
}
