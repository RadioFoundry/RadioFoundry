// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DB25-D front keys, encoder and the mic's PTT (docs/spec/hardware-db25d.md 3.3).
//
// Five drive lines and three sense lines make a 15-position matrix. The front keys' positions were found on the radio
// by pressing each one with the diagnostics page's raw scan running; the other positions report as spares. The mic's PTT pulls
// its RXD line (UART1 RXD, also the PC link and the hand mic's key frames) low. Every 0 bit on that line pulls it low
// too, so a low read within 50 ms of a good byte is serial data, not PTT (drivers/input/sharedptt, as on the other HR_C7000 boards),
// and the line then has to read low on three scans in a row (INF: the pad level is assumed readable while the pin is
// the UART's). The hand mic's PTT is PTC11, low while held (found on the radio), as the stock firmware reads it.
#include "db25d.h"
#include "config.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "input/keymatrix/keymatrix.h"
#include "input/quadrature/quadrature.h"
#include "input/sharedptt/shared_ptt.h"

#define N		KEYMATRIX_NONE
#define PTT_SCANS	3

// [drive][sense]: drive lines PTB18, PTB20, PTB21, PTB22, PTB23; sense lines PTB9, PTB10, PTB11. No front key uses
// the first two drive lines or the direct positions (the stock firmware's codes for them, 3.3)
static const uint8_t map[KEY_DRIVES * KEY_SENSES] = {
	N, N, N,
	N, N, N,
	N, KEY_P1, KEY_P2,
	KEY_P3, KEY_VOL, KEY_TONE,
	KEY_BE, KEY_ENC_PUSH, N,
};
static const uint8_t direct[KEY_SENSES] = { N, N, N };

// The hand mic's key numbers (3.4): 0-9 the digits, 10 *, 11 #, 12-14 P4-P6, 18 P7
static uint8_t mic_bit(uint8_t code)
{
	if (code <= 9) {
		return (uint8_t)(KEY_MIC_0 + code);
	}
	switch (code) {
	case 10: return KEY_MIC_STAR;
	case 11: return KEY_MIC_HASH;
	case 12: return KEY_MIC_P4;
	case 13: return KEY_MIC_P5;
	case 14: return KEY_MIC_P6;
	case 18: return KEY_MIC_P7;
	default: return N;
	}
}
static const keymatrix_t matrix = { .rows = KEY_DRIVES, .cols = KEY_SENSES, .map = map, .direct = direct };

static uint32_t last_raw, stable;
static uint8_t ptt_low;
static shared_ptt_t ptt_gate;	// PTA9 is also the host UART's RXD
static uint8_t mic_levels;
static uint16_t a9_changes, c11_changes;

// Encoder: both lines on both edges into the table decoder, two steps per detent resting where A == B (as on the
// other HR_C7000 boards). PTB8 goes in as A and PTB5 as B: the other way round, clockwise stepped down on the radio. The interrupt is
// the only writer
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
		quad_sample(&enc, QUAD_AB(hrc_pin_read(PIN_ENC_B), hrc_pin_read(PIN_ENC_A)));
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
	hrc_pin_in(PIN_PTT2);
	hrc_pin_in(PIN_ENC_A);
	hrc_pin_in(PIN_ENC_B);
	quad_init(&enc, ENC_STEPS, QUAD_REST_EQUAL, QUAD_AB(hrc_pin_read(PIN_ENC_B), hrc_pin_read(PIN_ENC_A)));
	enc_taken = 0;
	ptt_low = 0;
	mic_levels = (uint8_t)((hrc_pin_read(PIN_MIC_RXD) ? 1u : 0u) | (hrc_pin_read(PIN_PTT2) ? 2u : 0u));
	hrc_irq_register(IRQ_GPIOB, gpiob_isr);
	hrc_pin_irq(PIN_ENC_A, true, true, false);
	hrc_pin_irq(PIN_ENC_B, true, true, false);
	hrc_irq_enable(IRQ_GPIOB, true);
#if CONFIG_DIAG
	db25d_miccap_init();
#endif
}

uint32_t db25d_input_raw(void)
{
	return last_raw;
}

uint8_t db25d_mic_lines(uint16_t *a9, uint16_t *c11)
{
	*a9 = a9_changes;
	*c11 = c11_changes;
	return mic_levels;
}

uint32_t db25d_input_ptt_ignored(void)
{
	return ptt_gate.ignored;
}

void db25d_mic_lines_reset(void)
{
	a9_changes = 0;
	c11_changes = 0;
}

static void sample_mic_lines(void)
{
	uint8_t now = (uint8_t)((hrc_pin_read(PIN_MIC_RXD) ? 1u : 0u) | (hrc_pin_read(PIN_PTT2) ? 2u : 0u));
	uint8_t diff = now ^ mic_levels;
	if ((diff & 1u) && a9_changes < UINT16_MAX) {
		a9_changes++;
	}
	if ((diff & 2u) && c11_changes < UINT16_MAX) {
		c11_changes++;
	}
	mic_levels = now;
}

// Called every 10 ms: a change is reported once two scans in a row agree
static uint32_t scan(void)
{
	uint32_t raw = scan_matrix();
	uint8_t mic = mic_bit(db25d_mic_key());
	if (mic != N) {
		raw |= 1u << mic;
	}
	sample_mic_lines();
	uint32_t rx_ms;
	bool rx_seen = hrc_host_uart_last_rx(&rx_ms);
	bool rxd_low = shared_ptt_pressed(&ptt_gate, !hrc_pin_read(PIN_MIC_RXD), rx_seen, rx_ms, hal_millis());
	if (rxd_low || !hrc_pin_read(PIN_PTT2)) {
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
