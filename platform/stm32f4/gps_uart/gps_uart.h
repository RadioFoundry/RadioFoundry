// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A GPS module's NMEA output on a receive-only STM32F4 USART, shared by the radios with an optional module: the
// receive interrupt, the task that drains it through drivers/gps/gps_rx, run-time detection and the
// rate hunt (gps_baud.h). The same image runs with and without a module: one checksum-valid sentence makes it
// present and every sentence then goes to the APRS position source (CONFIG_APRS); a silent line, or no valid sentence
// at any rate of the board's list, switches the USART off again and ends the task. A board adds platform/stm32f4/gps_uart,
// drivers/gps/nmea_line and drivers/gps/gps_rx to BOARD_DIRS and calls stm32_gps_irq() from its USART's handler.
#ifndef STM32_GPS_UART_H
#define STM32_GPS_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "platform.h"

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
} stm32_gps_cfg_t;

// Start listening (from board_init). cfg stays valid
void stm32_gps_init(const stm32_gps_cfg_t *cfg);
// From the board's USART interrupt handler
void stm32_gps_irq(void);
// The module has been heard (board_t.gps_present)
bool stm32_gps_present(void);
// Settings > Location > GPS: sentences go to the position source only while on (on at start-up). The module keeps
// its supply: the boards that use this can't switch it alone
void stm32_gps_enable(bool on);
// For diagnostics: valid sentences, bad ones, bytes received; the rate in use (0 once given up)
void stm32_gps_counts(uint32_t *good, uint32_t *bad, uint32_t *bytes);
uint32_t stm32_gps_baud(void);

#endif
