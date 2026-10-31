// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// ADC1, one channel at a time by software trigger: a board's key ladders, knobs and supply sense. A conversion takes
// a few microseconds, so callers from any task share it inside a short critical section.
//
// The sampling channel: one more channel on the preempted group, which has its own trigger and data register, so an
// interrupt can sample audio at a steady rate while tasks go on reading. A preempted conversion started during an
// ordinary one interrupts it, and the ordinary one resumes after it; the ordinary data register is never touched
#include "platform.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"

#define SPINS	100000u

static bool ready;

static void wait_clear(uint32_t bit)
{
	for (uint32_t n = 0; n < SPINS && (ADC_CTRL2(ADC1_BASE) & bit); n++) {
	}
}

void at32_adc_init(void)
{
	if (ready) {
		return;
	}
	CRM_APB2EN |= CRM_APB2EN_ADC1EN;
	(void)CRM_APB2EN;
	ADCCOM_CCTRL = (ADCCOM_CCTRL & ~ADCCOM_CCTRL_ADCDIV_MASK) | ADCCOM_CCTRL_ADCDIV_4;
	ADC_CTRL1(ADC1_BASE) = 0;	// 12 bits, no scan
	ADC_CTRL2(ADC1_BASE) = 0;	// right aligned, single conversion
	// The longest sample time on every channel (640.5 cycles): the ladders and dividers have high source impedance
	ADC_SPT1(ADC1_BASE) = 0x07FFFFFFu;
	ADC_SPT2(ADC1_BASE) = 0x3FFFFFFFu;
	ADC_OSQ1(ADC1_BASE) = 0;	// one conversion in the ordinary sequence
	ADC_CTRL2(ADC1_BASE) |= ADC_CTRL2_ADCEN;
	for (uint32_t n = 0; n < SPINS && !(ADC_STS(ADC1_BASE) & ADC_STS_RDY); n++) {
	}
	hal_delay_ms(1);
	ADC_CTRL2(ADC1_BASE) |= ADC_CTRL2_ADCALINIT;
	wait_clear(ADC_CTRL2_ADCALINIT);
	ADC_CTRL2(ADC1_BASE) |= ADC_CTRL2_ADCAL;
	wait_clear(ADC_CTRL2_ADCAL);
	ready = true;
}

uint16_t at32_adc_read(uint8_t ch)
{
	if (!ready) {
		return 0;
	}
	bool rtos = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
	if (rtos) {
		taskENTER_CRITICAL();
	}
	ADC_OSQ3(ADC1_BASE) = ch & 0x1Fu;
	ADC_STS(ADC1_BASE) = 0;
	ADC_CTRL2(ADC1_BASE) |= ADC_CTRL2_OCSWTRG;
	uint32_t n = 0;
	while (!(ADC_STS(ADC1_BASE) & ADC_STS_OCCE) && ++n < SPINS) {
	}
	uint16_t v = (uint16_t)(ADC_ODT(ADC1_BASE) & 0xFFFu);
	if (rtos) {
		taskEXIT_CRITICAL();
	}
	return v;
}

void at32_adc_sample_init(uint8_t ch)
{
	at32_adc_init();
	bool rtos = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
	if (rtos) {
		taskENTER_CRITICAL();
	}
	ADC_PSQ(ADC1_BASE) = (uint32_t)(ch & 0x1Fu) << 15;	// PCLEN 0: one conversion
	if (rtos) {
		taskEXIT_CRITICAL();
	}
}
