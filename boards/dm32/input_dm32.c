// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-32UV keypad, side keys, PTT and encoder (docs/spec/hardware-dm32.md 6.1-6.3).
// The keypad rows are LCD DB0-DB4: a scan takes them back from the LCD controller under the LCD bus lock. SK1 / SK2
// pull columns 3 / 4 low with no row driven (keymatrix.h).
#include "dm32.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "input/keymatrix/keymatrix.h"

#define DIPLEX2_DB0_DB6_GPIO	(0x7Fu << 9)	// IO_DIPLEX2 bits 9-15: DB0-DB6 as GPIOs
#define N			KEYMATRIX_NONE

// [row][column 1..4] (6.1)
static const uint8_t map[KEY_ROWS * KEY_COLS] = {
	KEY_GREEN, KEY_3, KEY_2, KEY_1,
	KEY_LEFT, KEY_6, KEY_5, KEY_4,
	KEY_RIGHT, KEY_9, KEY_8, KEY_7,
	KEY_RED, KEY_HASH, KEY_0, KEY_STAR,
	KEY_UP, KEY_DOWN, N, N,
};
static const uint8_t direct[KEY_COLS] = { N, N, KEY_SK1, KEY_SK2 };
static const keymatrix_t matrix = { .rows = KEY_ROWS, .cols = KEY_COLS, .map = map, .direct = direct };

static uint32_t last_raw, stable;
static volatile int32_t enc_count;

static uint8_t read_columns_low(void)
{
	uint8_t low = 0;
	for (uint8_t c = 0; c < KEY_COLS; c++) {
		if (!hrc_pin_read((uint8_t)(PIN_KEY_COL1 + c))) {
			low |= (uint8_t)(1u << c);
		}
	}
	return low;
}

static uint32_t scan_matrix(void)
{
	uint8_t row_low[KEY_ROWS];
	dm32_lcdbus_lock();
	uint32_t s = hrc_irq_save();
	uint32_t saved = IO_DIPLEX2;
	IO_DIPLEX2 = saved | DIPLEX2_DB0_DB6_GPIO;
	hrc_irq_restore(s);
	for (uint8_t r = 0; r < KEY_ROWS; r++) {
		hrc_pin_out((uint8_t)(PIN_KEY_ROW0 + r), true);
	}
	hal_delay_us(5);
	uint8_t idle_low = read_columns_low();
	for (uint8_t r = 0; r < KEY_ROWS; r++) {
		hrc_pin_write((uint8_t)(PIN_KEY_ROW0 + r), false);
		hal_delay_us(5);
		row_low[r] = read_columns_low();
		hrc_pin_write((uint8_t)(PIN_KEY_ROW0 + r), true);
	}
	s = hrc_irq_save();
	IO_DIPLEX2 = saved;
	hrc_irq_restore(s);
	dm32_lcdbus_unlock();
	return keymatrix_decode(&matrix, idle_low, row_low);
}

// GPIO B bank interrupt: only the encoder's B line has its interrupt on
static void gpiob_isr(void)
{
	uint32_t pending = hrc_gpio_pending(GPIOB_BASE);
	hrc_gpio_ack(GPIOB_BASE, pending);
	if (pending & hrc_gpio_bit(PIN_ENC_B)) {
		enc_count += keymatrix_encoder_step(hrc_pin_read(PIN_ENC_A), hrc_pin_read(PIN_ENC_B));
	}
}

static void init(void)
{
	hrc_pin_in(PIN_PTT);
	hrc_pin_in(PIN_ENC_A);
	hrc_pin_in(PIN_ENC_B);
	enc_count = 0;
	hrc_irq_register(IRQ_GPIOB, gpiob_isr);
	hrc_pin_irq(PIN_ENC_B, true, true, false);
	hrc_irq_enable(IRQ_GPIOB, true);
}

// Called every 10 ms: a change is reported once two scans in a row agree
static uint32_t scan(void)
{
	uint32_t raw = scan_matrix();
	if (!hrc_pin_read(PIN_PTT)) {
		raw |= 1u << KEY_PTT;
	}
	if (raw == last_raw) {
		stable = raw;
	}
	last_raw = raw;
	return stable;
}

static int8_t encoder(void)
{
	uint32_t s = hrc_irq_save();
	int32_t n = enc_count;
	enc_count = 0;
	hrc_irq_restore(s);
	return (int8_t)(n > 127 ? 127 : n < -127 ? -127 : n);
}

const hal_input_t dm32_input = {
	.init = init,
	.scan = scan,
	.encoder = encoder,
};
