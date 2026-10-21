// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 TIM8 PWM outputs besides the backlight (docs/spec/hardware-md9600.md 3, 19.9): CH2 on PC7 is the sub-audio
// tone into the modulator, CH3 on PC8 the beep. TIM8 runs for the backlight (display_md9600.c: / 10 / 256, about
// 28 kHz); these only add a compare channel. A tone or beep generator changes the duty at its sample rate. Duty 0 hands
// the pin back as a GPIO driven low, the reset level.
#include "md9600.h"
#include "pins.h"

void md9600_pwm_out(uint8_t ch, uint8_t duty)
{
	if (ch != MD9600_PWM_TONE && ch != MD9600_PWM_BEEP) {
		return;
	}
	bool tone = ch == MD9600_PWM_TONE;
	GPIO_TypeDef *port = tone ? PIN_PORT(PIN_TONE_PWM) : PIN_PORT(PIN_BEEP_PWM);
	uint16_t pin = tone ? PIN_MASK(PIN_TONE_PWM) : PIN_MASK(PIN_BEEP_PWM);
	if (duty == 0 || !(RCC->APB2ENR & RCC_APB2ENR_TIM8EN)) {
		TIM8->CCER &= tone ? ~TIM_CCER_CC2E : ~TIM_CCER_CC3E;
		stm32_pin_out(port, pin, false, false);
		return;
	}
	if (tone) {
		TIM8->CCR2 = duty;
		if (!(TIM8->CCER & TIM_CCER_CC2E)) {
			TIM8->CCMR1 = (TIM8->CCMR1 & ~(TIM_CCMR1_OC2M | TIM_CCMR1_CC2S)) | (6u << TIM_CCMR1_OC2M_Pos) |
				      TIM_CCMR1_OC2PE;	// PWM mode 1, preload: a new duty starts with the next period
			TIM8->CCER |= TIM_CCER_CC2E;
		}
	} else {
		TIM8->CCR3 = duty;
		if (!(TIM8->CCER & TIM_CCER_CC3E)) {
			TIM8->CCMR2 = (TIM8->CCMR2 & ~(TIM_CCMR2_OC3M | TIM_CCMR2_CC3S)) | (6u << TIM_CCMR2_OC3M_Pos) |
				      TIM_CCMR2_OC3PE;
			TIM8->CCER |= TIM_CCER_CC3E;
		}
	}
	stm32_pin_af(port, pin, GPIO_AF3_TIM8, false, GPIO_NOPULL);
}
