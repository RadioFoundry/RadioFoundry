// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DB25-D front keys, encoder and the mic's PTT (docs/spec/hardware-db25d.md 3.3).
//
// Five drive lines and three sense lines make a 15-position matrix. Which front key sits where isn't traced, so the
// map below is provisional: the diagnostics page shows the raw scan for the bring-up to fix it. The mic's PTT pulls
// its RXD line (UART1 RXD, also the PC link) low; a serial byte at 115200 is low for at most 87 us, so the line counts
// as PTT only when it reads low on three scans in a row (INF: the pad level is assumed readable while the pin is the
// UART's).
#include "db25d.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "input/keymatrix/keymatrix.h"
#include "input/quadrature/quadrature.h"

#define N		KEYMATRIX_NONE
#define PTT_SCANS	3

// [drive][sense] (provisional)
static const uint8_t map[KEY_DRIVES * KEY_SENSES] = {
	KEY_P1, KEY_P2, KEY_P3,
	KEY_VOL, KEY_TONE, KEY_BE,
	KEY_MENU, KEY_ENC_PUSH, KEY_SPARE0,
	KEY_SPARE1, KEY_SPARE2, KEY_SPARE3,
	KEY_SPARE4, KEY_SPARE5, KEY_SPARE6,
};
static const uint8_t direct[KEY_SENSES] = { N, N, N };
static const keymatrix_t matrix = { .rows = KEY_DRIVES, .cols = KEY_SENSES, .map = map, .direct = direct };

static uint32_t last_raw, stable;
static uint8_t ptt_low;

// Encoder: both lines on both edges into the table decoder, two steps per detent resting where A == B (as on the
// DM-32; to check on the radio). The interrupt is the only writer
#define ENC_STEPS	2
static quad_dec_t enc;
static int32_t enc_taken;

static uint8_t drive_pin(uint8_t r)
{
	return r == 0 ? PIN_KEY_DRIVE0 : (uint8_t)(PIN_KEY_DRIVE1 + r - 1u);
}

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

static uint32_t scan_matrix(void)
{
	uint8_t row_low[KEY_DRIVES];
	uint8_t idle_low = read_senses_low();
	for (uint8_t r = 0; r < KEY_DRIVES; r++) {
		hrc_pin_write(drive_pin(r), false);
		hal_delay_us(5);
		row_low[r] = read_senses_low();
		hrc_pin_write(drive_pin(r), true);
	}
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
	for (uint8_t r = 0; r < KEY_DRIVES; r++) {
		hrc_pin_out(drive_pin(r), true);	// idle high
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

uint32_t db25d_input_raw(void)
{
	return last_raw;
}

// Called every 10 ms: a change is reported once two scans in a row agree
static uint32_t scan(void)
{
	uint32_t raw = scan_matrix();
	if (!hrc_pin_read(PIN_MIC_RXD)) {
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

const hal_input_t db25d_input = {
	.init = init,
	.scan = scan,
	.encoder = encoder,
	.encoder_stats = encoder_stats,
};
