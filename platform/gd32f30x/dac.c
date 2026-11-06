// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DAC: the two 12-bit outputs (DAC0 on PA4, DAC1 on PA5), output buffer on, written by software. Writes are single
// stores, so any task (and a fault handler) may write
#include "platform.h"

void gd32_dac_init(uint8_t ch)
{
	RCU_APB1EN |= RCU_APB1EN_DACEN;
	if (ch == 0) {
		gd32_pin_analog(GPIOA, GD32_PIN(4));
		DAC0_R12DH = 0;
		DAC_CTL |= DAC_CTL_DEN0;
	} else {
		gd32_pin_analog(GPIOA, GD32_PIN(5));
		DAC1_R12DH = 0;
		DAC_CTL |= DAC_CTL_DEN1;
	}
}

void gd32_dac_write(uint8_t ch, uint16_t value)
{
	if (ch == 0) {
		DAC0_R12DH = value & 0xFFFu;
	} else {
		DAC1_R12DH = value & 0xFFFu;
	}
}
