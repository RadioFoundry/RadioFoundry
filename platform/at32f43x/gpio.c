// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT32F43x pin set-up: the STM32F4's GPIO layout, two bits a pin for the mode and the pull, one for the output type,
// four for the multiplexed function (at32f43x.h)
#include "platform.h"

#define MODE_IN		0u
#define MODE_OUT	1u
#define MODE_MUX	2u
#define MODE_ANALOG	3u

void at32_gpio_clock(at32_gpio_t *port)
{
	CRM_AHBEN1 |= CRM_AHBEN1_GPIOAEN << GPIO_PORT_INDEX(port);
	(void)CRM_AHBEN1;
}

// Each pin's two-bit field in a register that has one a pin (CFGR, PULL)
static uint32_t put2(uint32_t reg, uint16_t pins, uint32_t v)
{
	for (uint32_t n = 0; n < 16u; n++) {
		if (pins & (1u << n)) {
			reg = (reg & ~(3u << (2u * n))) | (v << (2u * n));
		}
	}
	return reg;
}

// Read-modify-writes of the port's words: interrupts masked, so a task or handler that changes another pin of the port
// in between (the RF bus and the receiver's I2C share port B) isn't undone
static void pin_cfg(at32_gpio_t *port, uint16_t pins, uint32_t mode, bool open_drain, at32_pull_t pull)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	port->OMODE = open_drain ? (port->OMODE | pins) : (port->OMODE & ~(uint32_t)pins);
	port->PULL = put2(port->PULL, pins, pull == AT32_PULL_UP ? 1u : pull == AT32_PULL_DOWN ? 2u : 0u);
	port->CFGR = put2(port->CFGR, pins, mode);
	__set_PRIMASK(primask);
}

void at32_pin_force(at32_gpio_t *port, uint16_t pins, bool level)
{
	at32_gpio_clock(port);
	at32_pin_write(port, pins, level);	// level first, so the pin never glitches
	pin_cfg(port, pins, MODE_OUT, false, AT32_PULL_NONE);
}

void at32_pin_out(at32_gpio_t *port, uint16_t pins, bool level, bool open_drain)
{
	at32_gpio_clock(port);
	at32_pin_write(port, pins, level);
	pin_cfg(port, pins, MODE_OUT, open_drain, AT32_PULL_NONE);
}

void at32_pin_in(at32_gpio_t *port, uint16_t pins, at32_pull_t pull)
{
	at32_gpio_clock(port);
	pin_cfg(port, pins, MODE_IN, false, pull);
}

void at32_pin_af(at32_gpio_t *port, uint16_t pins, uint8_t af, bool open_drain)
{
	at32_gpio_clock(port);
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	uint32_t lo = port->MUXL;
	uint32_t hi = port->MUXH;
	for (uint32_t n = 0; n < 16u; n++) {
		if ((pins & (1u << n)) == 0) {
			continue;
		}
		if (n < 8u) {
			lo = (lo & ~(0xFu << (4u * n))) | ((uint32_t)(af & 0xFu) << (4u * n));
		} else {
			hi = (hi & ~(0xFu << (4u * (n - 8u)))) | ((uint32_t)(af & 0xFu) << (4u * (n - 8u)));
		}
	}
	port->MUXL = lo;
	port->MUXH = hi;
	__set_PRIMASK(primask);
	pin_cfg(port, pins, MODE_MUX, open_drain, AT32_PULL_NONE);
}

void at32_pin_analog(at32_gpio_t *port, uint16_t pins)
{
	at32_gpio_clock(port);
	pin_cfg(port, pins, MODE_ANALOG, false, AT32_PULL_NONE);
}

void at32_exint_route(at32_gpio_t *port, uint8_t line)
{
	CRM_APB2EN |= CRM_APB2EN_SCFGEN;
	(void)CRM_APB2EN;
	uint32_t shift = 4u * (line & 3u);
	SCFG_EXINTC(line >> 2) = (SCFG_EXINTC(line >> 2) & ~(0xFu << shift)) | (GPIO_PORT_INDEX(port) << shift);
}
