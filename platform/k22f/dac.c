// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DAC0 (reference manual chapter 37): 12-bit, VDDA reference, the output follows each write (buffer off)
#include "platform.h"

void k22f_dac_init(void)
{
	SIM_SCGC6 |= SIM_SCGC6_DAC0;
	DAC0_C1 = 0;
	DAC0_C0 = DAC_C0_DACEN | DAC_C0_DACRFS | DAC_C0_DACTRGSEL;
	k22f_dac_write(0);
}

void k22f_dac_write(uint16_t value)
{
	if (value > 0x0FFFu) {
		value = 0x0FFFu;
	}
	DAC0_DAT0L = (uint8_t)(value & 0xFFu);
	DAC0_DAT0H = (uint8_t)(value >> 8);
}
