// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// STM32F4 platform services for boards built on it
#ifndef PLATFORM_STM32F4_H
#define PLATFORM_STM32F4_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx_hal.h"
#include "hal_system.h"

typedef struct {
	uint32_t hse_hz;
	bool hse_bypass;	// external clock fed in, no crystal
	uint8_t pll_m, pll_p, pll_q;
	uint16_t pll_n;
	uint32_t ahb_div, apb1_div, apb2_div;	// RCC_SYSCLK_DIVx / RCC_HCLK_DIVx values
	uint32_t flash_latency;			// FLASH_LATENCY_x
	bool lse;				// 32.768 kHz crystal fitted
} stm32_clock_t;

// Switch to HSI first (the bootloader may leave HSE/PLL running, and HSE can't be changed while it is in use), then
// bring up HSE and the PLL from the table. Returns false and stays on HSI if the HSE or PLL never locks
bool stm32_clock_init(const stm32_clock_t *c);

// System services for board.system (sysboot.c): chip info, memory reads, reboot incl. the ROM DFU entry
extern const hal_system_t stm32_system;
// First thing in main(): enters the ROM bootloader when a REBOOT ROM-DFU request armed it (does not return then)
void stm32_sysboot_check(void);
// Optional board hook, called before the ROM bootloader entry: the board's reset-time pin levels (power latch high,
// transmitter off). Weak default does nothing
void board_sysboot_prepare(void);

// Enable the clock of a GPIO port by its base address
void stm32_gpio_clock(GPIO_TypeDef *port);

// Simple pin set-up helpers
void stm32_pin_out(GPIO_TypeDef *port, uint16_t pins, bool level, bool open_drain);
void stm32_pin_in(GPIO_TypeDef *port, uint16_t pins, uint32_t pull);
void stm32_pin_af(GPIO_TypeDef *port, uint16_t pins, uint8_t af, bool open_drain, uint32_t pull);
void stm32_pin_analog(GPIO_TypeDef *port, uint16_t pins);

static inline void stm32_pin_write(GPIO_TypeDef *port, uint16_t pins, bool level)
{
	port->BSRR = level ? pins : ((uint32_t)pins << 16);
}

static inline bool stm32_pin_read(GPIO_TypeDef *port, uint16_t pin)
{
	return (port->IDR & pin) != 0;
}

#endif
