// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-88 SoC A keypad, side keys, PTT and encoder (docs/spec/hardware-gd88.md 9.2).
//
// Six row lines (PTB18-PTB23, idle low) and three sense lines (PTB9-PTB11, low = closed). A scan takes every row
// high, reads the senses (a key wired from a sense to ground shows up there: the direct positions), then drives one
// row low at a time. The rows go back low after the scan. The PTT pulls UART1's RXD (PTA9, also the PC link) low; every
// 0 bit on that line does too, so a low read within 50 ms of a good byte is serial data, not PTT
// (drivers/input/sharedptt), and the line then has to read low on three scans in a row.
#include "gd88.h"
#include "config.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "input/keymatrix/keymatrix.h"
#include "input/quadrature/quadrature.h"
#include "input/sharedptt/shared_ptt.h"

#define N		KEYMATRIX_NONE
#define PTT_SCANS	3
#define ROW_SETTLE_US	5u

// [row][sense], the matrix codes 9,10,11 / 12,13,14 / 15,16,17 / 18,8,19 / 3,2,4 / 5,1,6 (9-17 the digits 1-9, 8 the
// 0, 18 *, 19 #, 3 P4, 4 the mode key, 1, 2, 5, 6 navigation). Direct (no row): 22 P2, 23 P1, 24 P3
static const uint8_t map[KEY_ROWS * KEY_SENSES] = {
	KEY_1, KEY_2, KEY_3,
	KEY_4, KEY_5, KEY_6,
	KEY_7, KEY_8, KEY_9,
	KEY_STAR, KEY_0, KEY_HASH,
	KEY_P4, KEY_NAV2, KEY_MODE,
	KEY_NAV5, KEY_NAV1, KEY_NAV6,
};
static const uint8_t direct[KEY_SENSES] = { KEY_P2, KEY_P1, KEY_P3 };
static const keymatrix_t matrix = { .rows = KEY_ROWS, .cols = KEY_SENSES, .map = map, .direct = direct };

static uint32_t last_raw, stable;
static uint8_t ptt_low;
static shared_ptt_t ptt_gate;	// PTA9 is also the host UART's RXD

// Encoder: both lines on both edges into the table decoder, two steps per detent resting where A == B (as
// hardware-db25d.md).
// TODO(gd88): unknown, needs a radio: the direction (swap PIN_ENC_A / PIN_ENC_B if clockwise steps down) and whether a
// detent is two steps (one event per detent, when the lines are equal)
#define ENC_STEPS	2
static quad_dec_t enc;
static int32_t enc_taken;

static uint8_t read_senses_low(void)
{
	uint8_t low = 0;
	for (uint8_t c = 0; c < KEY_SENSES; c++) {
		if (!hrc_pin_read((uint8_t)(PIN_KEY_SENSE0 + c))) {
			low |= (uint8_t)(1u << c);
		}
	}
	return low;
}

static void rows_all(bool level)
{
	for (uint8_t r = 0; r < KEY_ROWS; r++) {
		hrc_pin_write((uint8_t)(PIN_KEY_ROW0 + r), level);
	}
}

static uint32_t scan_matrix(void)
{
	uint8_t row_low[KEY_ROWS];
	rows_all(true);
	hal_delay_us(ROW_SETTLE_US);
	uint8_t idle_low = read_senses_low();
	for (uint8_t r = 0; r < KEY_ROWS; r++) {
		hrc_pin_write((uint8_t)(PIN_KEY_ROW0 + r), false);
		hal_delay_us(ROW_SETTLE_US);
		row_low[r] = read_senses_low();
		hrc_pin_write((uint8_t)(PIN_KEY_ROW0 + r), true);
	}
	rows_all(false);
	return keymatrix_decode(&matrix, idle_low, row_low);
}

// GPIO B bank interrupt: only the encoder's two lines have their interrupts on. Acked before the lines are read, so
// an edge during the read raises it again
static void gpiob_isr(void)
{
	uint32_t pending = hrc_gpio_pending(GPIOB_BASE);
	hrc_gpio_ack(GPIOB_BASE, pending);
	if (pending & (hrc_gpio_bit(PIN_ENC_A) | hrc_gpio_bit(PIN_ENC_B))) {
		quad_sample(&enc, QUAD_AB(hrc_pin_read(PIN_ENC_A), hrc_pin_read(PIN_ENC_B)));
	}
}

static void init(void)
{
	for (uint8_t r = 0; r < KEY_ROWS; r++) {
		hrc_pin_out((uint8_t)(PIN_KEY_ROW0 + r), false);	// idle low
	}
	for (uint8_t c = 0; c < KEY_SENSES; c++) {
		hrc_pin_in((uint8_t)(PIN_KEY_SENSE0 + c));
	}
	hrc_pin_in(PIN_ENC_A);
	hrc_pin_in(PIN_ENC_B);
	quad_init(&enc, ENC_STEPS, QUAD_REST_EQUAL, QUAD_AB(hrc_pin_read(PIN_ENC_A), hrc_pin_read(PIN_ENC_B)));
	enc_taken = 0;
	ptt_low = 0;
	hrc_irq_register(IRQ_GPIOB, gpiob_isr);
	hrc_pin_irq(PIN_ENC_A, true, true, false);
	hrc_pin_irq(PIN_ENC_B, true, true, false);
	hrc_irq_enable(IRQ_GPIOB, true);
}

uint32_t gd88_input_raw(void)
{
	return last_raw;
}

uint32_t gd88_input_ptt_ignored(void)
{
	return ptt_gate.ignored;
}

// Called every 10 ms: a change is reported once two scans in a row agree
static uint32_t scan(void)
{
	uint32_t raw = scan_matrix();
	uint32_t rx_ms;
	bool rx_seen = hrc_host_uart_last_rx(&rx_ms);
	if (shared_ptt_pressed(&ptt_gate, !hrc_pin_read(PIN_PTT), rx_seen, rx_ms, hal_millis())) {
		if (ptt_low < PTT_SCANS) {
			ptt_low++;
		}
	} else {
		ptt_low = 0;
	}
	if (ptt_low >= PTT_SCANS) {
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
	return quad_take(&enc, &enc_taken);
}

static void encoder_stats(hal_encoder_stats_t *st)
{
	st->detents = enc.detents;
	st->transitions = enc.transitions;
	st->illegal = enc.illegal;
	st->reversals = enc.reversals;
	st->steps_per_detent = ENC_STEPS;
}

const hal_input_t gd88_input = {
	.init = init,
	.scan = scan,
	.encoder = encoder,
	.encoder_stats = encoder_stats,
};
