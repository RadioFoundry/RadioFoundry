// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 start-up pin table (docs/spec/hardware-gd77.md 3.2): every pin the firmware uses, with its function and the
// level it starts at. Chip selects idle inactive, the HR_C6000 stays powered down, and every output whose role is
// still open starts inactive, so nothing keys up before the radio code chooses. The power hold is driven high before
// this table, on its own (board.c). Pure data: tests/host/gd77PinsTest.c checks it.
#ifndef GD77_PIN_TABLE_H
#define GD77_PIN_TABLE_H

#include "pins.h"

#define GPIO_OUT_LOW	(K22F_MUX_GPIO | K22F_OUT)
#define GPIO_OUT_HIGH	(K22F_MUX_GPIO | K22F_OUT | K22F_HIGH)
#define GPIO_IN		(K22F_MUX_GPIO)
#define GPIO_IN_PU	(K22F_MUX_GPIO | K22F_PULLUP)

static const k22f_pin_cfg_t gd77_pin_table[] = {
	// Display: deselected, held in reset until the display driver starts it, backlight off
	{ PIN_LCD_CS, GPIO_OUT_HIGH }, { PIN_LCD_RESET, GPIO_OUT_LOW }, { PIN_LCD_A0, GPIO_OUT_HIGH },
	{ PIN_LCD_SCK, GPIO_OUT_HIGH }, { PIN_LCD_SDA, GPIO_OUT_HIGH }, { PIN_BACKLIGHT, GPIO_OUT_LOW },
	// Keys: columns idle as inputs with pull-ups (the scan drives one low at a time), rows and direct keys read
	{ PIN_KEY_COL0, GPIO_IN_PU }, { PIN_KEY_COL0 + 1, GPIO_IN_PU }, { PIN_KEY_COL0 + 2, GPIO_IN_PU },
	{ PIN_KEY_COL0 + 3, GPIO_IN_PU },
	{ PIN_KEY_ROW0, GPIO_IN_PU }, { PIN_KEY_ROW0 + 1, GPIO_IN_PU }, { PIN_KEY_ROW0 + 2, GPIO_IN_PU },
	{ PIN_KEY_ROW0 + 3, GPIO_IN_PU }, { PIN_KEY_ROW0 + 4, GPIO_IN_PU },
	{ PIN_KEY_B9, GPIO_IN_PU }, { PIN_KEY_B1, GPIO_IN_PU }, { PIN_KEY_A2, GPIO_IN_PU }, { PIN_KEY_A1, GPIO_IN },
	{ PIN_POWER_SWITCH, GPIO_IN },
	// Data flash: deselected (released to its pull-up), clock low (mode 0)
	{ PIN_NOR_CS, GPIO_IN }, { PIN_NOR_MOSI, GPIO_OUT_HIGH }, { PIN_NOR_SCK, GPIO_OUT_LOW },
	{ PIN_NOR_MISO, GPIO_IN_PU },
	// The two I2C buses idle: clock high, data released
	{ PIN_I2CA_SCL, GPIO_OUT_HIGH }, { PIN_I2CA_SDA, GPIO_IN_PU },
	{ PIN_I2CB_SCL, GPIO_OUT_HIGH }, { PIN_I2CB_SDA, GPIO_IN_PU },
	// HR_C6000: deselected, both control lines high (powered down) until the radio code starts it
	{ PIN_C6K_CS, GPIO_OUT_HIGH },
	{ PIN_C6K_SCK, K22F_MUX(2) | K22F_DRIVE_HIGH }, { PIN_C6K_SOUT, K22F_MUX(2) | K22F_DRIVE_HIGH },
	{ PIN_C6K_SIN, K22F_MUX(2) | K22F_DRIVE_HIGH },
	{ PIN_C6K_E0, GPIO_OUT_HIGH }, { PIN_C6K_PWD, GPIO_OUT_HIGH },
	{ PIN_C6K_RF_RX, GPIO_IN }, { PIN_C6K_RF_TX, GPIO_IN }, { PIN_C6K_SYS, GPIO_IN }, { PIN_C6K_SLOT, GPIO_IN },
	// I2S0: the codec drives the clocks
	{ PIN_I2S_TXD0, K22F_MUX(6) }, { PIN_I2S_RX_BCLK, K22F_MUX(6) }, { PIN_I2S_RXD0, K22F_MUX(6) },
	{ PIN_I2S_RX_FS, K22F_MUX(6) },
	// Open roles, inactive: no transmit enable, RF switches off, speaker amplifier off, indicators off
	{ PIN_TX_EN, GPIO_OUT_LOW }, { PIN_AUDIO_EN, GPIO_OUT_LOW },
	{ PIN_RF_SW1, GPIO_OUT_LOW }, { PIN_RF_SW2, GPIO_OUT_LOW }, { PIN_RF_SW3, GPIO_OUT_LOW },
	{ PIN_RF_SW4, GPIO_OUT_LOW },
	{ PIN_LED_A, GPIO_OUT_LOW }, { PIN_LED_B, GPIO_OUT_LOW },
	{ PIN_IND_C5, GPIO_OUT_HIGH }, { PIN_IND_C6, GPIO_OUT_LOW },
};

#define GD77_PIN_COUNT	(sizeof(gd77_pin_table) / sizeof(gd77_pin_table[0]))

#endif
