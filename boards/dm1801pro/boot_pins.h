// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DM-1801 Pro outputs and their levels at start-up (hardware-dm1801pro.md 3.1), applied in order by board.c io_init()
// after the power latch and the IO mux. A table so the host test (tests/dm1801proPinsTest.c) can check it
#ifndef DM1801PRO_BOOT_PINS_H
#define DM1801PRO_BOOT_PINS_H

#include <stdbool.h>
#include <stdint.h>
#include "pins.h"

typedef struct {
	uint8_t pin;
	bool level;
} dm1801pro_boot_pin_t;

// Safe levels: transmitter and receiver paths off, speaker muted, chip selects high
static const dm1801pro_boot_pin_t dm1801pro_boot_pins[] = {
	{ PIN_FLASH_CS, true },
	{ PIN_RF_CS, true },
	{ PIN_RF_CLK, false },
	{ PIN_TX_VHF, false },
	{ PIN_TX_UHF, false },
	{ PIN_RX_VHF, false },
	{ PIN_RX_UHF, false },
	{ PIN_MIC_PATH, true },
	{ PIN_SPK_SOURCE, true },
	{ PIN_AMP_EN, false },
	{ PIN_BCAST_EN, true },
	{ PIN_LED_RED, false },
	{ PIN_LED_GREEN, false },
	{ PIN_LCD_BACKLIGHT, false },
	{ PIN_VIBRATION, false },
	{ PIN_LCD_CS, true },
	{ PIN_LCD_RESET, true },
	{ PIN_PTB16, false },	// the stock firmware's only write to it; the boot loader leaves it an input
};

#define DM1801PRO_BOOT_PIN_COUNT	(sizeof(dm1801pro_boot_pins) / sizeof(dm1801pro_boot_pins[0]))

#endif
