// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// LCD backlight on a PWM channel whose output pin doubles as a GPIO (docs/spec/hardware-dm32.md 3.6). Most boards
// wire it to PTA15 / PWM0; one with another channel passes its hrc_backlight_t (docs/spec/hardware-db25d.md 5).
#include "platform.h"

#define PWM_CH(ch, off)		REG32(PWM_BASE + (uint32_t)(ch) * 0x20u + (off))
#define PWM_CFG(ch)		PWM_CH(ch, 0x00)
#define PWM_RPT(ch)		PWM_CH(ch, 0x04)
#define PWM_PER(ch)		PWM_CH(ch, 0x08)
#define PWM_FP(ch)		PWM_CH(ch, 0x0C)
#define CFG_LOAD		(1u << 2)
#define CFG_EN			(1u << 0)

// PWMn comes out on PTA15 + n when IO_DIPLEX0 bit 17 + n is clear; set, the pin is a GPIO (HR_C7000 user guide 4.4.4.4)
#define PWM_PIN(ch)		(0x0Fu + (ch))
#define DIPLEX0_PWM_GPIO(ch)	(1u << (17u + (ch)))

// 0 = off, 100 or more = fully on (GPIO), otherwise the duty in percent. FP is the on-time in counts of PER
void hrc_backlight_pwm(const hrc_backlight_t *bl, uint8_t percent)
{
	uint32_t s = hrc_irq_save();
	if (percent == 0 || percent >= 100) {
		IO_DIPLEX0 |= DIPLEX0_PWM_GPIO(bl->ch);
		hrc_irq_restore(s);
		hrc_pin_out(PWM_PIN(bl->ch), percent > 0);
		return;
	}
	IO_DIPLEX0 &= ~DIPLEX0_PWM_GPIO(bl->ch);
	hrc_irq_restore(s);
	PWM_CFG(bl->ch) = bl->cfg;
	if (bl->rpt != 0) {
		PWM_RPT(bl->ch) = bl->rpt;
	}
	PWM_PER(bl->ch) = bl->period;
	PWM_FP(bl->ch) = (uint32_t)percent * bl->period / 100u;
	PWM_CFG(bl->ch) = bl->cfg | CFG_LOAD;
	PWM_CFG(bl->ch) = bl->cfg | CFG_LOAD | CFG_EN;
}

// The default: PWM0, PER 200 (hardware-dm32.md 3.6)
void hrc_backlight(uint8_t percent)
{
	static const hrc_backlight_t pwm0 = { .ch = 0, .cfg = 0x328u, .rpt = 0, .period = 200u };
	hrc_backlight_pwm(&pwm0, percent);
}
