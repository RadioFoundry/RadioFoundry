// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-380 keypad, side keys, PTT and the 16-position channel switch (docs/spec/hardware-md380.md 9).
//
// The keypad is the shared scan (platform/stm32f4/fsmc_keypad): rows driven high one at a time, the LCD data lines
// D0-D7 read back as columns under the LCD bus lock. The matrix positions are the MD-UV380's (the nearest documented relative); they are
// unconfirmed on the MD-380, and md380_input_raw() shows the raw row bits on the diagnostics page so a bench session
// can correct the table.
//
// The channel knob is not a quadrature encoder but a 16-position switch with four code lines. Each scan reads the code,
// turns it into a position and reports the change as encoder steps, so the core sees the same thing as on the other
// radios (a step per detent). The code is a Gray code with its own line order, measured on the radio (hardware-md380.md
// 9.2): one line changes per detent, so a contact that bounces flickers between two neighbors, never far away.
#include "md380.h"
#include "pins.h"
#include "hal_time.h"
#include "fsmc_keypad/fsmc_keypad.h"

#ifndef MD380_CH_ACTIVE_HIGH
#define MD380_CH_ACTIVE_HIGH	0	// 0: lines pulled up, a closed contact reads low
#endif

#define ROW_COUNT	3
#define NO_KEY		STM32_KEYPAD_NO_KEY

// [row][column D0..D7] (hardware-md380.md 9.1, MD-UV380 positions: unconfirmed on the MD-380)
static const uint8_t matrix[ROW_COUNT][8] = {
	{ KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_0, KEY_STAR },
	{ KEY_GREEN, KEY_UP, KEY_DOWN, KEY_7, KEY_8, KEY_9, KEY_HASH, KEY_RED },
	{ NO_KEY, NO_KEY, NO_KEY, NO_KEY, NO_KEY, NO_KEY, KEY_SK2, KEY_SK1 },
};

static const stm32_fsmc_keypad_cfg_t keypad = {
	.row_port = { PIN_PORT(PIN_KEY_ROW0), PIN_PORT(PIN_KEY_ROW1), PIN_PORT(PIN_KEY_ROW2) },
	.row_pin = { PIN_MASK(PIN_KEY_ROW0), PIN_MASK(PIN_KEY_ROW1), PIN_MASK(PIN_KEY_ROW2) },
	.rows = ROW_COUNT,
	.matrix = matrix,
	.lock = md380_lcdbus_lock,
	.unlock = md380_lcdbus_unlock,
};

static uint32_t last_raw;
static uint32_t stable;
static uint8_t raw_rows[ROW_COUNT];
static uint8_t ch_code;
static int8_t ch_pos = -1;	// -1 until the first read
static int32_t enc_count;

// The four code lines as a number 0..15 (bit n = line n), contacts as ones
static uint8_t read_channel_code(void)
{
	uint8_t c = 0;
	c |= PIN_READ(PIN_CH_B0) ? 1u : 0u;
	c |= PIN_READ(PIN_CH_B1) ? 2u : 0u;
	c |= PIN_READ(PIN_CH_B2) ? 4u : 0u;
	c |= PIN_READ(PIN_CH_B3) ? 8u : 0u;
	return MD380_CH_ACTIVE_HIGH ? c : (uint8_t)(~c & 0xFu);
}

// Position by code, clockwise: the codes read one detent at a time were 0 8 A 2 3 B 9 1 5 D F 7 6 E C 4 (bench,
// hardware-md380.md 9.2)
static const int8_t code_position[16] = {
	[0x0] = 0, [0x8] = 1, [0xA] = 2, [0x2] = 3, [0x3] = 4, [0xB] = 5, [0x9] = 6, [0x1] = 7,
	[0x5] = 8, [0xD] = 9, [0xF] = 10, [0x7] = 11, [0x6] = 12, [0xE] = 13, [0xC] = 14, [0x4] = 15,
};

// A code counts once it reads the same CH_STABLE scans in a row (30 ms): the contacts bounce between detents
#define CH_STABLE	3

static void channel_poll(void)
{
	static uint8_t cand, same;
	uint8_t code = read_channel_code();
	if (code != cand) {
		cand = code;
		same = 1;
		return;
	}
	if (same < CH_STABLE && ++same < CH_STABLE) {
		return;
	}
	ch_code = code;
	int8_t pos = code_position[code];
	if (ch_pos >= 0 && pos != ch_pos) {
		int d = pos - ch_pos;
		if (d > 8) {		// the shorter way round, should the switch turn past its stop
			d -= 16;
		} else if (d < -8) {
			d += 16;
		}
		enc_count += d;
	}
	ch_pos = pos;
}

static void init(void)
{
	stm32_fsmc_keypad_init(&keypad);
	stm32_pin_in(PIN_PORT(PIN_PTT), PIN_MASK(PIN_PTT) | PIN_MASK(PIN_PTT_EXT), GPIO_PULLUP);
	uint32_t pull = MD380_CH_ACTIVE_HIGH ? GPIO_PULLDOWN : GPIO_PULLUP;
	stm32_pin_in(PIN_PORT(PIN_CH_B0), PIN_MASK(PIN_CH_B0) | PIN_MASK(PIN_CH_B1), pull);
	stm32_pin_in(PIN_PORT(PIN_CH_B2), PIN_MASK(PIN_CH_B2) | PIN_MASK(PIN_CH_B3), pull);
	enc_count = 0;
	ch_pos = -1;
}

// Called every 10 ms: a change is reported once two scans in a row agree
static uint32_t scan(void)
{
	uint32_t raw = stm32_fsmc_keypad_scan(&keypad, raw_rows);
	if (!PIN_READ(PIN_PTT) || !PIN_READ(PIN_PTT_EXT)) {
		raw |= 1u << KEY_PTT;
	}
	if (raw == last_raw) {
		stable = raw;
	}
	last_raw = raw;
	channel_poll();
	return stable;
}

static int8_t encoder(void)
{
	int32_t n = enc_count;
	if (n > 127) {
		n = 127;
	} else if (n < -127) {
		n = -127;
	}
	enc_count -= n;
	return (int8_t)n;
}

void md380_input_raw(uint8_t rows[3], uint8_t *channel_code)
{
	for (int r = 0; r < ROW_COUNT; r++) {
		rows[r] = raw_rows[r];
	}
	*channel_code = ch_code;
}

const hal_input_t md380_input = { .init = init, .scan = scan, .encoder = encoder };
