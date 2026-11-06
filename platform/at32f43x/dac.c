// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DAC: the two 12-bit outputs (DAC1 on PA4, DAC2 on PA5; channels 0 and 1 here), output buffer on, written by software.
// Writes are single stores, so any task (and a fault handler) may write
#include "platform.h"

void at32_dac_init(uint8_t ch)
{
	CRM_APB1EN |= CRM_APB1EN_DACEN;
	(void)CRM_APB1EN;
	if (ch == 0) {
		at32_pin_analog(GPIOA, AT32_PIN(4));
		DAC_D1DTH12R = 0;
		DAC_CTRL |= DAC_CTRL_D1EN;
	} else {
		at32_pin_analog(GPIOA, AT32_PIN(5));
		DAC_D2DTH12R = 0;
		DAC_CTRL |= DAC_CTRL_D2EN;
	}
}

void at32_dac_write(uint8_t ch, uint16_t value)
{
	if (ch == 0) {
		DAC_D1DTH12R = value & 0xFFFu;
	} else {
		DAC_D2DTH12R = value & 0xFFFu;
	}
}
