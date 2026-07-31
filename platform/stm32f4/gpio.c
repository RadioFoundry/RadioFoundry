// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
#include "platform.h"

void stm32_gpio_clock(GPIO_TypeDef *port)
{
	uint32_t index = ((uint32_t)port - GPIOA_BASE) / (GPIOB_BASE - GPIOA_BASE);
	RCC->AHB1ENR |= 1u << index;
	(void)RCC->AHB1ENR;
}

static void pin_init(GPIO_TypeDef *port, uint16_t pins, uint32_t mode, uint32_t pull, uint8_t af)
{
	GPIO_InitTypeDef init = {
		.Pin = pins,
		.Mode = mode,
		.Pull = pull,
		.Speed = GPIO_SPEED_FREQ_VERY_HIGH,
		.Alternate = af,
	};
	stm32_gpio_clock(port);
	HAL_GPIO_Init(port, &init);
}

void stm32_pin_out(GPIO_TypeDef *port, uint16_t pins, bool level, bool open_drain)
{
	stm32_gpio_clock(port);
	stm32_pin_write(port, pins, level);	// level first, so the pin never glitches
	pin_init(port, pins, open_drain ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, 0);
}

void stm32_pin_in(GPIO_TypeDef *port, uint16_t pins, uint32_t pull)
{
	pin_init(port, pins, GPIO_MODE_INPUT, pull, 0);
}

void stm32_pin_af(GPIO_TypeDef *port, uint16_t pins, uint8_t af, bool open_drain, uint32_t pull)
{
	pin_init(port, pins, open_drain ? GPIO_MODE_AF_OD : GPIO_MODE_AF_PP, pull, af);
}

void stm32_pin_analog(GPIO_TypeDef *port, uint16_t pins)
{
	pin_init(port, pins, GPIO_MODE_ANALOG, GPIO_NOPULL, 0);
}
