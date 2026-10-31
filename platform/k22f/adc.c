// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// ADC0 / ADC1 (reference manual chapter 35): 12-bit single-ended conversions, software triggered and polled, with the
// hardware average of 8 samples. The calibration at init is the manual's: run CAL, then the plus and minus side gains
// are the sums of the CLPS, CLP4..CLP0 (CLMS, CLM4..CLM0) results, halved, with bit 15 set
#include "platform.h"

#define CONV_WAIT_LOOPS	100000u
#define CLK_DIV4	2u	// ADIV: bus / 4 keeps the ADC clock under its 18 MHz (12-bit) limit at a 60 MHz bus

static bool ready[2];

static uint32_t cal_sum(uint8_t inst, uint32_t first, uint32_t last)
{
	uint32_t sum = 0;
	for (uint32_t off = first; off <= last; off += 4u) {
		sum += ADC_CLP(inst, off);
	}
	return sum;
}

k22f_status_t k22f_adc_init(uint8_t inst)
{
	if (inst > 1u) {
		return K22F_ERR_ARG;
	}
	SIM_SCGC6 |= inst == 0 ? SIM_SCGC6_ADC0 : SIM_SCGC6_ADC1;
	ADC_CFG1(inst) = ADC_CFG1_ADIV(CLK_DIV4) | ADC_CFG1_ADLSMP | ADC_CFG1_MODE_12;
	ADC_CFG2(inst) = 0;
	ADC_SC2(inst) = 0;			// software trigger, default reference
	ADC_SC3(inst) = ADC_SC3_CAL | ADC_SC3_AVGE | ADC_SC3_AVGS(3);	// calibrate with the 32-sample average
	uint32_t w = 0;
	while ((ADC_SC3(inst) & ADC_SC3_CAL) != 0) {
		if (++w > 20u * CONV_WAIT_LOOPS) {
			return K22F_ERR_TIMEOUT;
		}
	}
	if (ADC_SC3(inst) & ADC_SC3_CALF) {
		return K22F_ERR_TIMEOUT;
	}
	ADC_PG(inst) = ((cal_sum(inst, 0x38u, 0x4Cu) >> 1) | 0x8000u) & 0xFFFFu;	// CLPS, CLP4 .. CLP0
	ADC_MG(inst) = ((cal_sum(inst, 0x58u, 0x6Cu) >> 1) | 0x8000u) & 0xFFFFu;	// CLMS, CLM4 .. CLM0
	ADC_SC3(inst) = ADC_SC3_AVGE | ADC_SC3_AVGS(1);		// 8-sample average for readings
	ready[inst] = true;
	return K22F_OK;
}

uint16_t k22f_adc_read(uint8_t inst, uint8_t channel)
{
	if (inst > 1u || !ready[inst]) {
		return 0xFFFFu;
	}
	ADC_SC1A(inst) = ADC_SC1_ADCH(channel);	// starts the conversion
	uint32_t w = 0;
	while ((ADC_SC1A(inst) & ADC_SC1_COCO) == 0) {
		if (++w > CONV_WAIT_LOOPS) {
			return 0xFFFFu;
		}
	}
	return (uint16_t)(ADC_RA(inst) & 0x0FFFu);
}
