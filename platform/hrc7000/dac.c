// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DAC (3 channels, 12 bits; docs/spec/hardware-dm32.md 3.8) as the radios use it: channel B is the PA drive, C a
// bias the board picks, A unused. The modem's analog set-up is modem-hrc7000.md section 4 steps 1-3.
#include "platform.h"
#include "hal_time.h"

#define ADC_CTRL		REG32(ADC_BASE + 0x00)
#define ADC_CTRL_RESET		(1u << 3)

void hrc_modem_analog_init(uint16_t dac_c)
{
	// 1: the ADC controller reset and idle (the baseband shares the analog front end)
	ADC_CTRL = ADC_CTRL_RESET;
	hal_delay_us(2);
	ADC_CTRL = 0;
	// 2: DAC B to its pin
	uint32_t s = hrc_irq_save();
	IO_DIPLEX2 &= ~IO_DIPLEX2_DAC_B;
	hrc_irq_restore(s);
	// 3: DACs B and C on, A off, the PA drive at 0
	DAC_PD_CTRL = DAC_A_OFF;
	DAC_PD_MODE_EN = 0x01u;
	DAC_DATA_A = 0;
	DAC_DATA_B = 0;
	DAC_DATA_C = dac_c;
}

// Interrupts off: DAC_PD_CTRL is read-modify-write, and the DMR interrupts change the PA drive
void hrc_dac_b_power(bool on)
{
	uint32_t s = hrc_irq_save();
	if (on) {
		DAC_PD_CTRL &= ~DAC_B_OFF;
	} else {
		DAC_PD_CTRL |= DAC_B_OFF;
	}
	hrc_irq_restore(s);
}
