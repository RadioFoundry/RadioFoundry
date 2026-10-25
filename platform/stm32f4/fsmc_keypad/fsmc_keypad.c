// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A key matrix on the FSMC data lines (fsmc_keypad.h; docs/spec/hardware-dm1701.md 10.1)
#include "fsmc_keypad.h"
#include "hal_time.h"

#define MODER_INPUT	0u
#define MODER_OUTPUT	1u
#define MODER_AF	2u
#define PUPDR_NONE	0u
#define PUPDR_DOWN	2u
#define FSMC_AF		12u

// Two-bit fields (MODER, PUPDR) of every pin in mask set to value
static uint32_t field2(uint32_t reg, uint16_t mask, uint32_t value)
{
	for (uint32_t i = 0; i < 16; i++) {
		if (mask & (1u << i)) {
			reg = (reg & ~(3u << (i * 2))) | (value << (i * 2));
		}
	}
	return reg;
}

// Four-bit alternate function fields of every pin in mask
static void set_af(GPIO_TypeDef *port, uint16_t mask, uint32_t af)
{
	for (uint32_t i = 0; i < 16; i++) {
		if (mask & (1u << i)) {
			uint32_t idx = i >> 3, shift = (i & 7u) * 4;
			port->AFR[idx] = (port->AFR[idx] & ~(0xFu << shift)) | (af << shift);
		}
	}
}

// Mode and pull of a group of pins; interrupts off so a read-modify-write of the same port elsewhere can't race
static void set_mode(GPIO_TypeDef *port, uint16_t mask, uint32_t mode, uint32_t pull)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	port->PUPDR = field2(port->PUPDR, mask, pull);
	port->MODER = field2(port->MODER, mask, mode);
	__set_PRIMASK(primask);
}

static void columns_to_gpio(void)
{
	// Discharge first (hardware-dm1701.md 10.1 step 1): leaving FSMC mode can leave charge that reads as phantom keys
	GPIOD->BSRR = (uint32_t)STM32_FSMC_D0_D3 << 16;
	GPIOE->BSRR = (uint32_t)STM32_FSMC_D4_D7 << 16;
	set_mode(GPIOD, STM32_FSMC_D0_D3, MODER_OUTPUT, PUPDR_NONE);
	set_mode(GPIOE, STM32_FSMC_D4_D7, MODER_OUTPUT, PUPDR_NONE);
	hal_delay_us(1);
	set_mode(GPIOD, STM32_FSMC_D0_D3, MODER_INPUT, PUPDR_DOWN);
	set_mode(GPIOE, STM32_FSMC_D4_D7, MODER_INPUT, PUPDR_DOWN);
}

static void columns_to_fsmc(void)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	set_af(GPIOD, STM32_FSMC_D0_D3, FSMC_AF);
	set_af(GPIOE, STM32_FSMC_D4_D7, FSMC_AF);
	__set_PRIMASK(primask);
	set_mode(GPIOD, STM32_FSMC_D0_D3, MODER_AF, PUPDR_NONE);
	set_mode(GPIOE, STM32_FSMC_D4_D7, MODER_AF, PUPDR_NONE);
}

// Columns as a byte, bit n = Dn
static uint8_t read_columns(void)
{
	uint32_t d = GPIOD->IDR, e = GPIOE->IDR;
	uint8_t c = 0;
	c |= (d & GPIO_PIN_14) ? 0x01 : 0;
	c |= (d & GPIO_PIN_15) ? 0x02 : 0;
	c |= (d & GPIO_PIN_0) ? 0x04 : 0;
	c |= (d & GPIO_PIN_1) ? 0x08 : 0;
	c |= (uint8_t)(((e >> 7) & 0xFu) << 4);	// PE7..PE10 -> D4..D7
	return c;
}

void stm32_fsmc_keypad_init(const stm32_fsmc_keypad_cfg_t *cfg)
{
	for (int r = 0; r < cfg->rows; r++) {
		stm32_pin_in(cfg->row_port[r], cfg->row_pin[r], GPIO_PULLDOWN);
	}
}

uint32_t stm32_fsmc_keypad_scan(const stm32_fsmc_keypad_cfg_t *cfg, uint8_t *raw)
{
	uint32_t keys = 0;
	cfg->lock();
	columns_to_gpio();
	for (int r = 0; r < cfg->rows; r++) {
		GPIO_TypeDef *port = cfg->row_port[r];
		uint16_t pin = cfg->row_pin[r];
		port->BSRR = pin;
		set_mode(port, pin, MODER_OUTPUT, PUPDR_NONE);
		hal_delay_us(5);
		uint8_t cols = read_columns();
		// Drive the row low before letting go: a row released while high stays charged, and through a pressed key
		// the next row's scan saw it (every key also read as the one below it, seen on the bench 2026-10-09)
		port->BSRR = (uint32_t)pin << 16;
		hal_delay_us(2);
		set_mode(port, pin, MODER_INPUT, PUPDR_DOWN);
		if (raw != NULL) {
			raw[r] = cols;
		}
		for (int c = 0; c < 8; c++) {
			if ((cols & (1u << c)) && cfg->matrix[r][c] != STM32_KEYPAD_NO_KEY) {
				keys |= 1u << cfg->matrix[r][c];
			}
		}
	}
	columns_to_fsmc();
	cfg->unlock();
	return keys;
}
