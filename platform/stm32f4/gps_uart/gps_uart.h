// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A GPS module's NMEA output on a receive-only STM32F4 USART, shared by the radios with an optional module: the
// receive interrupt, the task that drains it through drivers/gps/gps_rx, run-time detection and the
// rate hunt (gps_baud.h). The same image runs with and without a module: one checksum-valid sentence makes it
// present and every sentence then goes to the position service (core/position); a silent line, or no valid sentence
// at any rate of the board's list, switches the USART off again and ends the task. Once heard, the module follows
// Settings > Location > GPS (hal_gps_t.enable, drivers/gps/gps_rx/gps_power): off stops the USART and, through the
// board's power hook, the module's supply. A board adds platform/stm32f4/gps_uart, drivers/gps/nmea_line and
// drivers/gps/gps_rx to BOARD_DIRS and calls stm32_gps_irq() from its USART's handler.
#ifndef STM32_GPS_UART_H
#define STM32_GPS_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "platform.h"
#include "hal_gps.h"

typedef struct {
	USART_TypeDef *uart;
	IRQn_Type irq;
	GPIO_TypeDef *rx_port;
	uint16_t rx_pin;
	uint8_t rx_af;
	const uint32_t *rates;		// the rates to listen at, in order (one entry: a fixed rate)
	uint8_t n_rates;
	uint32_t listen_ms;		// listening time per rate
	void (*found)(void);		// optional, task context: the module was just heard
	void (*absent)(void);		// optional, task context: nothing was heard, the USART is off again (a board
					// that powered the module up to listen powers it down)
	void (*power)(bool on);		// optional, task context, only after found: switch the module's supply for
					// the setting. NULL: the board can't, and off only stops the USART
} stm32_gps_cfg_t;

// Start listening (from board_init). cfg stays valid
void stm32_gps_init(const stm32_gps_cfg_t *cfg);
// From the board's USART interrupt handler
void stm32_gps_irq(void);
// board_t.gps. enable: on at start-up; once the module has been heard, off switches the USART and the module's supply
// (cfg->power) off within one poll, on switches them back at the rate found
extern const hal_gps_t stm32_gps;

#endif
