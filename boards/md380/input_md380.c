// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-380 keypad, side keys, PTT and the 16-position channel switch (docs/spec/hardware-md380.md 9).
//
// The keypad works as on the DM-1701: rows driven high one at a time, the LCD data lines D0-D7 read back as columns
// under the LCD bus lock. The matrix positions are the MD-UV380's (the nearest documented relative); they are
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

#ifndef MD380_CH_ACTIVE_HIGH
#define MD380_CH_ACTIVE_HIGH	0	// 0: lines pulled up, a closed contact reads low
#endif

#define MODER_INPUT	0u
#define MODER_OUTPUT	1u
#define MODER_AF	2u
#define PUPDR_NONE	0u
#define PUPDR_DOWN	2u
#define FSMC_AF		12u

#define ROW_COUNT	3
#define NO_KEY		0xFF

// [row][column D0..D7] (hardware-md380.md 9.1, MD-UV380 positions: unconfirmed on the MD-380)
static const uint8_t matrix[ROW_COUNT][8] = {
	{ KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_0, KEY_STAR },
	{ KEY_GREEN, KEY_UP, KEY_DOWN, KEY_7, KEY_8, KEY_9, KEY_HASH, KEY_RED },
	{ NO_KEY, NO_KEY, NO_KEY, NO_KEY, NO_KEY, NO_KEY, KEY_SK2, KEY_SK1 },
};

static GPIO_TypeDef *const row_port[ROW_COUNT] = { PIN_PORT(PIN_KEY_ROW0), PIN_PORT(PIN_KEY_ROW1), PIN_PORT(PIN_KEY_ROW2) };
static const uint16_t row_pin[ROW_COUNT] = { PIN_MASK(PIN_KEY_ROW0), PIN_MASK(PIN_KEY_ROW1), PIN_MASK(PIN_KEY_ROW2) };

static uint32_t last_raw;
static uint32_t stable;
static uint8_t raw_rows[ROW_COUNT];
static uint8_t ch_code;
static int8_t ch_pos = -1;	// -1 until the first read
static int32_t enc_count;

static uint32_t field2(uint32_t reg, uint16_t mask, uint32_t value)
{
	for (uint32_t i = 0; i < 16; i++) {
		if (mask & (1u << i)) {
			reg = (reg & ~(3u << (i * 2))) | (value << (i * 2));
		}
	}
	return reg;
}

static void set_af(GPIO_TypeDef *port, uint16_t mask, uint32_t af)
{
	for (uint32_t i = 0; i < 16; i++) {
		if (mask & (1u << i)) {
			uint32_t idx = i >> 3, shift = (i & 7u) * 4;
			port->AFR[idx] = (port->AFR[idx] & ~(0xFu << shift)) | (af << shift);
		}
	}
}

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
	GPIOD->BSRR = (uint32_t)KEY_COLS_PORTD << 16;
	GPIOE->BSRR = (uint32_t)KEY_COLS_PORTE << 16;
	set_mode(GPIOD, KEY_COLS_PORTD, MODER_OUTPUT, PUPDR_NONE);
	set_mode(GPIOE, KEY_COLS_PORTE, MODER_OUTPUT, PUPDR_NONE);
	hal_delay_us(1);
	set_mode(GPIOD, KEY_COLS_PORTD, MODER_INPUT, PUPDR_DOWN);
	set_mode(GPIOE, KEY_COLS_PORTE, MODER_INPUT, PUPDR_DOWN);
}

static void columns_to_fsmc(void)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	set_af(GPIOD, KEY_COLS_PORTD, FSMC_AF);
	set_af(GPIOE, KEY_COLS_PORTE, FSMC_AF);
	__set_PRIMASK(primask);
	set_mode(GPIOD, KEY_COLS_PORTD, MODER_AF, PUPDR_NONE);
	set_mode(GPIOE, KEY_COLS_PORTE, MODER_AF, PUPDR_NONE);
}

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

static uint32_t scan_matrix(void)
{
	uint32_t keys = 0;
	md380_lcdbus_lock();
	columns_to_gpio();
	for (int r = 0; r < ROW_COUNT; r++) {
		row_port[r]->BSRR = row_pin[r];
		set_mode(row_port[r], row_pin[r], MODER_OUTPUT, PUPDR_NONE);
		hal_delay_us(5);
		uint8_t cols = read_columns();
		// Drive the row low before letting go (the DM-1701's bench lesson: a row released high stays charged)
		row_port[r]->BSRR = (uint32_t)row_pin[r] << 16;
		hal_delay_us(2);
		set_mode(row_port[r], row_pin[r], MODER_INPUT, PUPDR_DOWN);
		raw_rows[r] = cols;
		for (int c = 0; c < 8; c++) {
			if ((cols & (1u << c)) && matrix[r][c] != NO_KEY) {
				keys |= 1u << matrix[r][c];
			}
		}
	}
	columns_to_fsmc();
	md380_lcdbus_unlock();
	return keys;
}

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
	for (int r = 0; r < ROW_COUNT; r++) {
		stm32_pin_in(row_port[r], row_pin[r], GPIO_PULLDOWN);
	}
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
	uint32_t raw = scan_matrix();
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
