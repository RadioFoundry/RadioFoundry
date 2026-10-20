// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 keys (docs/spec/hardware-gd77.md 9.1): a matrix of 4 columns (PTC0-PTC3, driven low one at a time) by 5 rows
// (PTB19-PTB23, read with pull-ups, low = closed), and four direct inputs (PTB9, PTB1, PTA2, PTA1, low = pressed).
// The key positions are in keys.h; the roles of the six navigation keys and the four direct keys are provisional
#include "gd77.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"

static const struct {
	uint8_t pin, key;
} direct_keys[] = {
	{ PIN_KEY_B1, KEY_SIDE_B1 }, { PIN_KEY_B9, KEY_SIDE_B9 }, { PIN_KEY_A2, KEY_TOP }, { PIN_KEY_A1, KEY_PTT },
};

static uint32_t last_raw, stable, raw_keys;

static uint8_t rows_low(void)
{
	uint8_t low = 0;
	for (uint8_t r = 0; r < KEY_ROWS; r++) {
		if (!k22f_pin_read((uint8_t)(PIN_KEY_ROW0 + r))) {
			low |= (uint8_t)(1u << r);
		}
	}
	return low;
}

// A column is driven low only while it is scanned, and is an input with its pull-up the rest of the time, so two keys
// pressed in one row never short two driven columns together
static uint32_t scan_matrix(void)
{
	uint8_t col_low[KEY_COLS];
	raw_keys = 0;
	uint8_t idle = rows_low();
	for (uint8_t c = 0; c < KEY_COLS; c++) {
		uint8_t pin = (uint8_t)(PIN_KEY_COL0 + c);
		k22f_pin_write(pin, false);
		k22f_pin_dir(pin, true);
		hal_delay_us(5);
		col_low[c] = (uint8_t)(rows_low() & ~idle);
		k22f_pin_dir(pin, false);
		raw_keys |= (uint32_t)col_low[c] << (c * KEY_ROWS);
	}
	return keymatrix_decode(&gd77_keymatrix, 0, col_low);
}

static void init(void)
{
	// The pin table set every key pin up as an input (board.c); a column's output level is low whenever it is driven
	for (uint8_t c = 0; c < KEY_COLS; c++) {
		k22f_pin_write((uint8_t)(PIN_KEY_COL0 + c), false);
	}
}

// Called every 10 ms: a change is reported once two scans in a row agree
static uint32_t scan(void)
{
	uint32_t raw = scan_matrix();
	for (uint8_t i = 0; i < sizeof(direct_keys) / sizeof(direct_keys[0]); i++) {
		if (!k22f_pin_read(direct_keys[i].pin)) {
			raw |= 1u << direct_keys[i].key;
			raw_keys |= 1u << (20u + i);
		}
	}
	if (raw == last_raw) {
		stable = raw;
	}
	last_raw = raw;
	return stable;
}

uint32_t gd77_key_raw(void)
{
	return raw_keys;
}

const hal_input_t gd77_input = {
	.init = init,
	.scan = scan,
};
