// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// LCD backlight on PTA15 (GPIO 0x0F), which is also the PWM0 output (docs/spec/hardware-dm32.md 3.6). The DM-32UV and
// the DM-1801 Pro both wire their backlight there.
#include "platform.h"

#define PWM_CFG			REG32(PWM_BASE + 0x00)
#define PWM_PER			REG32(PWM_BASE + 0x08)
#define PWM_FP			REG32(PWM_BASE + 0x0C)
#define PWM_PERIOD		200u
#define IO_DIPLEX0_PWM0_GPIO	(1u << 17)		// 1: PTA15 is a GPIO, 0: PWM0
#define PIN_PTA15		0x0Fu

// 0 = off, 100 or more = fully on (GPIO), otherwise PWM0 duty in percent. FP is counted against PER (200), so the
// duty is percent x 2 (the old port wrote the percentage and topped out at about half: hardware-dm32.md 15)
void hrc_backlight(uint8_t percent)
{
	uint32_t s = hrc_irq_save();
	if (percent == 0 || percent >= 100) {
		IO_DIPLEX0 |= IO_DIPLEX0_PWM0_GPIO;
		hrc_irq_restore(s);
		hrc_pin_out(PIN_PTA15, percent > 0);
		return;
	}
	IO_DIPLEX0 &= ~IO_DIPLEX0_PWM0_GPIO;
	hrc_irq_restore(s);
	PWM_CFG = 0x328u;
	PWM_PER = PWM_PERIOD;
	PWM_FP = (uint32_t)percent * PWM_PERIOD / 100u;
	PWM_CFG = 0x32Cu;
	PWM_CFG = 0x32Du;
}
