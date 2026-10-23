// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A key matrix whose columns are the FSMC data lines D0-D7 (STM32_FSMC_D0_D3, STM32_FSMC_D4_D7 in platform.h), shared
// by the STM32F4 radios whose keypad shares the LCD bus: under the board's LCD bus lock the lines leave the FSMC,
// rows are driven high one at a time and the columns read back, then the lines go back to the FSMC. A board adds
// platform/stm32f4/fsmc_keypad to BOARD_DIRS and calls these from its hal_input_t.
#ifndef STM32_FSMC_KEYPAD_H
#define STM32_FSMC_KEYPAD_H

#include <stdint.h>
#include <stdbool.h>
#include "platform.h"

#define STM32_KEYPAD_MAX_ROWS	4
#define STM32_KEYPAD_NO_KEY	0xFF

typedef struct {
	GPIO_TypeDef *row_port[STM32_KEYPAD_MAX_ROWS];
	uint16_t row_pin[STM32_KEYPAD_MAX_ROWS];
	uint8_t rows;
	const uint8_t (*matrix)[8];	// [row][column D0..D7]: the key's bit in the scan, or STM32_KEYPAD_NO_KEY
	void (*lock)(void);		// the LCD bus lock, held for the whole scan
	void (*unlock)(void);
} stm32_fsmc_keypad_cfg_t;

// Rows as inputs with a weak pull-down between scans (a definite low that can't load the LCD bus)
void stm32_fsmc_keypad_init(const stm32_fsmc_keypad_cfg_t *cfg);
// Every key found, as bits (side keys may be held together; the core decides what a combination means). raw, if not
// NULL, gets each row's column bits (bit n = Dn) for a diagnostics page
uint32_t stm32_fsmc_keypad_scan(const stm32_fsmc_keypad_cfg_t *cfg, uint8_t *raw);

#endif
