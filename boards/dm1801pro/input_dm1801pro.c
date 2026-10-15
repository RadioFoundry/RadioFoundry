// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1801 Pro keys: a 5 x 4 matrix (strobes PTB18-PTB22 driven low one at a time, inputs PTB3-PTB6 low = closed) and
// four direct inputs (docs/spec/hardware-dm1801pro.md 9.1). The table was read off the radio, one key at a time; each
// strobe is one keypad column (9 3 6 #, 8 2 5 0, 7 1 4 *) or half of the navigation keys.
#include "dm1801pro.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "input/keymatrix/keymatrix.h"

#define N	KEYMATRIX_NONE

// [strobe][input]
static const uint8_t map[KEY_STROBES * KEY_INPUTS] = {
	KEY_9, KEY_3, KEY_6, KEY_HASH,
	KEY_DOWN, KEY_MENU, KEY_UP, KEY_BACK,
	KEY_RIGHT, KEY_AB, KEY_VFO_MR, KEY_LEFT,
	KEY_8, KEY_2, KEY_5, KEY_0,
	KEY_7, KEY_1, KEY_4, KEY_STAR,
};
static const uint8_t direct[KEY_INPUTS] = { N, N, N, N };
static const keymatrix_t matrix = { .rows = KEY_STROBES, .cols = KEY_INPUTS, .map = map, .direct = direct };

static const struct {
	uint8_t pin, key;
} direct_keys[] = {
	{ PIN_KEY_D0, KEY_PTT }, { PIN_KEY_D1, KEY_SOS }, { PIN_KEY_D2, KEY_SK1 }, { PIN_KEY_D3, KEY_SK2 },
};

static uint32_t last_raw, stable, raw_matrix;

static uint8_t read_inputs_low(void)
{
	uint8_t low = 0;
	for (uint8_t c = 0; c < KEY_INPUTS; c++) {
		if (!hrc_pin_read((uint8_t)(PIN_KEY_ROW0 + c))) {
			low |= (uint8_t)(1u << c);
		}
	}
	return low;
}

static uint32_t scan_matrix(void)
{
	uint8_t row_low[KEY_STROBES];
	raw_matrix = 0;
	for (uint8_t r = 0; r < KEY_STROBES; r++) {
		hrc_pin_write((uint8_t)(PIN_KEY_STROBE0 + r), false);
		hal_delay_us(5);
		row_low[r] = read_inputs_low();
		hrc_pin_write((uint8_t)(PIN_KEY_STROBE0 + r), true);
		raw_matrix |= (uint32_t)row_low[r] << (r * KEY_INPUTS);
	}
	return keymatrix_decode(&matrix, 0, row_low);
}

static void init(void)
{
	for (uint8_t r = 0; r < KEY_STROBES; r++) {
		hrc_pin_out((uint8_t)(PIN_KEY_STROBE0 + r), true);
	}
	for (uint8_t c = 0; c < KEY_INPUTS; c++) {
		hrc_pin_in((uint8_t)(PIN_KEY_ROW0 + c));
	}
	for (uint8_t i = 0; i < sizeof(direct_keys) / sizeof(direct_keys[0]); i++) {
		hrc_pin_in(direct_keys[i].pin);
	}
}

// Called every 10 ms: a change is reported once two scans in a row agree
static uint32_t scan(void)
{
	uint32_t raw = scan_matrix();
	uint32_t pins = 0;
	for (uint8_t i = 0; i < sizeof(direct_keys) / sizeof(direct_keys[0]); i++) {
		if (!hrc_pin_read(direct_keys[i].pin)) {
			raw |= 1u << direct_keys[i].key;
			pins |= 1u << i;
		}
	}
	raw_matrix |= pins << 20;
	if (raw == last_raw) {
		stable = raw;
	}
	last_raw = raw;
	return stable;
}

uint32_t dm1801pro_key_raw(void)
{
	return raw_matrix;
}

const hal_input_t dm1801pro_input = {
	.init = init,
	.scan = scan,
};
