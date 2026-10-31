// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD32F30x pin set-up: four bits a pin in CTL0 / CTL1 (gd32f30x.h)
#include "platform.h"

void gd32_gpio_clock(gd32_gpio_t *port)
{
	RCU_APB2EN |= (RCU_APB2EN_PAEN << GPIO_PORT_INDEX(port)) | RCU_APB2EN_AFEN;
	(void)RCU_APB2EN;
}

// A read-modify-write of both control words: interrupts masked, so a task or handler that changes another pin of the
// port in between (the RF bus and the receiver's I2C share port B) isn't undone
static void pin_cfg(gd32_gpio_t *port, uint16_t pins, uint32_t nibble)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	uint32_t ctl0 = port->CTL0;
	uint32_t ctl1 = port->CTL1;
	for (uint32_t n = 0; n < 16u; n++) {
		if ((pins & (1u << n)) == 0) {
			continue;
		}
		if (n < 8u) {
			ctl0 = (ctl0 & ~(0xFu << (4u * n))) | (nibble << (4u * n));
		} else {
			ctl1 = (ctl1 & ~(0xFu << (4u * (n - 8u)))) | (nibble << (4u * (n - 8u)));
		}
	}
	port->CTL0 = ctl0;
	port->CTL1 = ctl1;
	__set_PRIMASK(primask);
}

void gd32_pin_force(gd32_gpio_t *port, uint16_t pins, bool level)
{
	gd32_gpio_clock(port);
	gd32_pin_write(port, pins, level);	// level first, so the pin never glitches
	pin_cfg(port, pins, GD32_PIN_OUT_PP);
}

void gd32_pin_out(gd32_gpio_t *port, uint16_t pins, bool level, bool open_drain)
{
	gd32_gpio_clock(port);
	gd32_pin_write(port, pins, level);
	pin_cfg(port, pins, open_drain ? GD32_PIN_OUT_OD : GD32_PIN_OUT_PP);
}

void gd32_pin_in(gd32_gpio_t *port, uint16_t pins, gd32_pull_t pull)
{
	gd32_gpio_clock(port);
	if (pull == GD32_PULL_NONE) {
		pin_cfg(port, pins, GD32_PIN_IN_FLOAT);
		return;
	}
	gd32_pin_write(port, pins, pull == GD32_PULL_UP);	// OCTL picks up or down
	pin_cfg(port, pins, GD32_PIN_IN_PULL);
}

void gd32_pin_af(gd32_gpio_t *port, uint16_t pins, bool open_drain)
{
	gd32_gpio_clock(port);
	pin_cfg(port, pins, open_drain ? GD32_PIN_AF_OD : GD32_PIN_AF_PP);
}

void gd32_pin_analog(gd32_gpio_t *port, uint16_t pins)
{
	gd32_gpio_clock(port);
	pin_cfg(port, pins, GD32_PIN_ANALOG);
}

void gd32_swj(uint32_t mode)
{
	RCU_APB2EN |= RCU_APB2EN_AFEN;
	(void)RCU_APB2EN;
	AFIO_PCF0 = (AFIO_PCF0 & ~AFIO_PCF0_SWJ_MASK) | mode;
}

void gd32_exti_route(gd32_gpio_t *port, uint8_t line)
{
	RCU_APB2EN |= RCU_APB2EN_AFEN;
	(void)RCU_APB2EN;
	uint32_t shift = 4u * (line & 3u);
	AFIO_EXTISS(line >> 2) = (AFIO_EXTISS(line >> 2) & ~(0xFu << shift)) | (GPIO_PORT_INDEX(port) << shift);
}
