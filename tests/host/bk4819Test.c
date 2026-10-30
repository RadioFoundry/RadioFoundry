// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// BK4819 driver: frames on two fake chips that share clock and data and have their own enables (the DB25-D's main
// and LO transceivers), the init and LO sequences, and the pure helpers (docs/spec/hardware-db25d.md 4)
#include <stdio.h>
#include <string.h>
#include "rf/bk4819/bk4819.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- Two fake chips on the three wires ---

typedef struct {
	uint8_t reg;
	uint16_t val;
} frame_t;

typedef struct {
	bool cs;
	uint32_t shift;
	int bits;
	bool reading;
	uint16_t out;
	int out_bits;
	frame_t log[256];
	int n;
	int protocol_errors;
	uint16_t regs[128];
} chip_t;

static chip_t chips[2];
static bool clk, data, driving;
static int crit_depth, crit_max;

static void fake_cs(chip_t *c, bool level)
{
	if (!c->cs && level && !c->reading) {	// end of a write frame
		if (c->bits != 24) {
			c->protocol_errors++;
		} else if (c->n < 256) {
			c->log[c->n].reg = (uint8_t)(c->shift >> 16);
			c->log[c->n].val = (uint16_t)c->shift;
			c->regs[(c->shift >> 16) & 0x7F] = (uint16_t)c->shift;
			c->n++;
		}
	}
	if (c->cs && !level) {
		if (!chips[0].cs || !chips[1].cs) {
			c->protocol_errors++;	// both chips selected at once
		}
		driving = true;
		c->shift = 0;
		c->bits = 0;
	}
	if (level) {
		c->reading = false;
	}
	c->cs = level;
}

static void cs_main(bool level) { fake_cs(&chips[0], level); }
static void cs_lo(bool level) { fake_cs(&chips[1], level); }

static void fake_clk(bool level)
{
	bool rising = !clk && level;
	clk = level;
	if (!rising) {
		return;
	}
	for (int i = 0; i < 2; i++) {
		chip_t *c = &chips[i];
		if (c->cs) {
			continue;
		}
		if (!driving) {
			if (!c->reading) {
				if (c->bits != 8 || (c->shift & 0x80u) == 0) {
					c->protocol_errors++;
				}
				c->reading = true;
				c->out = c->regs[c->shift & 0x7Fu];
				c->out_bits = 0;
			}
			c->out_bits++;
			continue;
		}
		c->shift = (c->shift << 1) | (data ? 1u : 0u);
		c->bits++;
	}
}

static void fake_data_out(bool level)
{
	data = level;
	driving = true;
}

static void fake_release(void)
{
	driving = false;
}

static bool fake_data_in(void)
{
	for (int i = 0; i < 2; i++) {
		chip_t *c = &chips[i];
		int n = c->out_bits - 1;
		if (!c->cs && c->reading && n >= 0 && n <= 15) {
			return (c->out >> (15 - n)) & 1u;
		}
	}
	return false;
}

static void fake_delay(uint32_t us)
{
	(void)us;
}

static uint32_t fake_enter(void)
{
	if (++crit_depth > crit_max) {
		crit_max = crit_depth;
	}
	return 0;
}

static void fake_exit(uint32_t s)
{
	(void)s;
	crit_depth--;
}

static const rfbus_3w_pins_t pins_main = {
	.cs = cs_main, .clk = fake_clk, .data_out = fake_data_out, .data_release = fake_release,
	.data_in = fake_data_in, .delay_us = fake_delay, .crit_enter = fake_enter, .crit_exit = fake_exit,
	.delay_ms = fake_delay,
};

static const rfbus_3w_pins_t pins_lo = {
	.cs = cs_lo, .clk = fake_clk, .data_out = fake_data_out, .data_release = fake_release,
	.data_in = fake_data_in, .delay_us = fake_delay, .crit_enter = fake_enter, .crit_exit = fake_exit,
	.delay_ms = fake_delay,
};

static rfbus_3w_t bus3_main, bus3_lo;

// A fresh bus per attach: its write count starts at 0
static rfbus_t *main_bus(void)
{
	rfbus_3w_attach(&bus3_main, &pins_main);
	return &bus3_main.bus;
}

static rfbus_t *lo_bus(void)
{
	rfbus_3w_attach(&bus3_lo, &pins_lo);
	return &bus3_lo.bus;
}

static void reset_chips(void)
{
	memset(chips, 0, sizeof chips);
	chips[0].cs = chips[1].cs = true;
	clk = data = driving = false;
	crit_depth = crit_max = 0;
}

static int find_last(const chip_t *c, uint8_t reg)
{
	for (int i = c->n - 1; i >= 0; i--) {
		if (c->log[i].reg == reg) {
			return i;
		}
	}
	return -1;
}

static void test_helpers(void)
{
	CHECK(bk4819_ctcss_word(885) == 1827);
	CHECK(bk4819_ctcss_word(1000) == 2065);
	CHECK(bk4819_ctcss_word(2541) == 5247);
	// The stock transmitter truncates: 100.0 Hz = 2064.888 -> 2064
	CHECK(bk4819_ctcss_word_tx(1000) == 2064 && bk4819_ctcss_word_tx(885) == 1827 && bk4819_ctcss_word_tx(2541) == 5246);
	CHECK(bk4819_rssi_dbm(0x0000) == -160);
	CHECK(bk4819_rssi_dbm(0x0082) == -95);		// 130 half-dB
	CHECK(bk4819_rssi_dbm(0xFFFF) == 95);		// bits above 8 ignored
	// The DB25-D stock squelch: open at -v dBm, close 2 dB lower (hardware-db25d.md 6.4)
	CHECK(bk4819_squelch_rssi(-136, 2) == (48u << 8 | 44u));
	CHECK(bk4819_squelch_rssi(-120, 2) == (80u << 8 | 76u));
	CHECK(bk4819_squelch_rssi(-160, 2) == 0x0000);
	CHECK(bk4819_squelch_rssi(-1, 2) == (0xFFu << 8 | 0xFBu));
	CHECK(bk4819_squelch_rssi(-159, 2) == (2u << 8 | 0u));
	CHECK(bk4819_squelch_rssi(-200, 0) == 0x0000);
}

static void test_frames(void)
{
	bk4819_t m, l;
	reset_chips();
	bk4819_attach(&m, main_bus());
	bk4819_attach(&l, lo_bus());
	bk4819_write(&m, 0x30, 0xBFF1);
	CHECK(chips[0].n == 1 && chips[0].log[0].reg == 0x30 && chips[0].log[0].val == 0xBFF1);
	CHECK(chips[1].n == 0);		// the other chip's enable stayed high
	CHECK(m.state == 0xBFF1 && m.bus->writes == 1);
	CHECK(crit_depth == 0 && crit_max == 1);

	bk4819_write(&l, 0x38, 0x1234);
	CHECK(chips[1].n == 1 && chips[1].log[0].reg == 0x38 && chips[1].log[0].val == 0x1234);
	CHECK(chips[0].n == 1);

	chips[0].regs[0x00] = BK4819_ID;
	chips[1].regs[0x00] = 0x6818;
	CHECK(bk4819_read(&m, 0x00) == BK4819_ID);
	CHECK(bk4819_read(&l, 0x00) == 0x6818);
	chips[0].regs[0x67] = 0x0123;
	CHECK(bk4819_read(&m, BK4819_REG_RSSI) == 0x0123);
	CHECK(chips[0].n == 1 && chips[1].n == 1);	// reads log no writes
	CHECK(chips[0].protocol_errors == 0 && chips[1].protocol_errors == 0);
}

static void test_init(void)
{
	bk4819_t m, l;
	reset_chips();
	bk4819_attach(&m, main_bus());
	bk4819_init_main(&m);
	const chip_t *c = &chips[0];
	CHECK(c->log[0].reg == 0x00 && c->log[0].val == 0x8000);	// soft reset, then release
	CHECK(c->log[1].reg == 0x00 && c->log[1].val == 0x0000);
	CHECK(c->log[2].reg == 0x33 && c->log[2].val == 0x0003);
	int i = find_last(c, 0x4F);
	CHECK(i >= 0 && c->log[i].val == 0x3332);
	i = find_last(c, 0x7D);
	CHECK(i >= 0 && c->log[i].val == 0xE34A);
	CHECK(c->log[c->n - 1].reg == 0x30 && c->log[c->n - 1].val == 0x0200);	// ends idle, bit 9 set as the stock
	CHECK(m.state == 0x0200 && m.idle == 0x0200 && m.af_gain == 0xB3B3);
	CHECK(chips[1].n == 0);

	bk4819_attach(&l, lo_bus());
	chips[1].regs[0x73] = 0x0102;
	bk4819_init_lo(&l, BK4819_LO_PA_MAX);
	c = &chips[1];
	i = find_last(c, 0x73);
	CHECK(i >= 0 && c->log[i].val == 0x0112);			// AFC off, the other bits kept
	CHECK(c->log[i + 1].reg == 0x47 && c->log[i + 1].val == 0x6943);
	i = find_last(c, 0x4F);
	CHECK(i >= 0 && c->log[i].val == 0x2F2E);
	i = find_last(c, 0x36);
	CHECK(i >= 0 && c->log[i].val == 0x00BF);
	CHECK(c->log[c->n - 1].reg == 0x30 && c->log[c->n - 1].val == 0);	// ends off, 0x37 left on
	CHECK(l.idle == 0 && find_last(c, 0x37) < c->n - 1 && c->log[find_last(c, 0x37)].val == BK4819_POWER_ON);
	CHECK(c->protocol_errors == 0 && chips[0].protocol_errors == 0);
}

static void test_sequences(void)
{
	bk4819_t m, l;
	reset_chips();
	bk4819_attach(&m, main_bus());
	bk4819_attach(&l, lo_bus());

	bk4819_set_freq(&m, 51550000u);
	const chip_t *c = &chips[0];
	CHECK(c->n == 3);
	CHECK(c->log[0].reg == 0x30 && c->log[0].val == 0);
	CHECK(c->log[1].reg == 0x38 && c->log[1].val == (5155000u & 0xFFFF));
	CHECK(c->log[2].reg == 0x39 && c->log[2].val == (5155000u >> 16));
	CHECK(m.freq_10hz == 5155000u);

	// The main chip's idle is 0x0200; BK4819_STATE_IDLE means the chip's own. RX -> TX goes through idle, idle -> RX is
	// one write
	m.idle = BK4819_STATE_IDLE_MAIN;
	chips[0].n = 0;
	bk4819_set_state(&m, BK4819_STATE_RX);
	CHECK(c->n == 2 && c->log[0].val == 0x0200 && c->log[1].val == BK4819_STATE_RX);
	chips[0].n = 0;
	bk4819_set_state(&m, BK4819_STATE_IDLE);
	bk4819_set_state(&m, BK4819_STATE_RX);
	CHECK(c->n == 2 && c->log[0].val == 0x0200 && c->log[1].val == BK4819_STATE_RX);
	chips[0].n = 0;
	bk4819_set_state(&m, BK4819_STATE_TX_MAIN);
	CHECK(c->n == 2 && c->log[0].val == 0x0200 && c->log[1].val == 0xC3FE);

	// The LO for 446.000 MHz sits 51.55 MHz above: 497.55 MHz
	bk4819_lo_tune(&l, 446000000u + 51550000u);
	c = &chips[1];
	CHECK(c->n == 6);
	CHECK(c->log[0].reg == 0x30 && c->log[0].val == 0);
	CHECK(c->log[1].reg == 0x38 && c->log[1].val == (49755000u & 0xFFFF));
	CHECK(c->log[2].reg == 0x39 && c->log[2].val == (49755000u >> 16));
	CHECK(c->log[3].reg == 0x37 && c->log[3].val == BK4819_POWER_ON);
	CHECK(c->log[4].reg == 0x30 && c->log[4].val == 0);
	CHECK(c->log[5].reg == 0x30 && c->log[5].val == 0xC1FE);	// the LO's value, bit 9 clear

	// Off is 0x30 = 0 and nothing else; the same frequency again isn't rewritten (stock TX end: 37, 30 = 0, 30 = 0xC1FE)
	chips[1].n = 0;
	bk4819_lo_off(&l);
	CHECK(c->n == 1 && c->log[0].reg == 0x30 && c->log[0].val == 0);
	chips[1].n = 0;
	bk4819_lo_tune(&l, 446000000u + 51550000u);
	CHECK(c->n == 4 && c->log[0].val == 0 && c->log[1].reg == 0x37 && c->log[1].val == BK4819_POWER_ON);
	CHECK(c->log[2].val == 0 && c->log[3].reg == 0x30 && c->log[3].val == 0xC1FE);

	// TX preparation in RX: the plain words, then with CTCSS (100.0 Hz, truncated word), as the stock
	c = &chips[0];
	chips[0].n = 0;
	bk4819_tx_prepare(&m, BK4819_TONE_NONE, 0);
	CHECK(c->n == 2 && c->log[0].reg == 0x40 && c->log[0].val == 0x34D0 && c->log[1].reg == 0x51 && c->log[1].val == 0x1050);
	chips[0].n = 0;
	bk4819_tx_prepare(&m, BK4819_TONE_CTCSS, 1000);
	CHECK(c->n == 3 && c->log[0].reg == 0x40 && c->log[0].val == 0x34A8 && c->log[1].reg == 0x51 && c->log[1].val == 0x9045);
	CHECK(c->log[2].reg == 0x07 && c->log[2].val == 2064);
	// DCS 023 (the stock order: 0x40, the rate in 0x07, 0x51, then the Golay word 0x763813 in two halves); inverted
	// sets 0x51 bit 13
	chips[0].n = 0;
	bk4819_tx_prepare(&m, BK4819_TONE_DCS, 023);
	CHECK(c->n == 5 && c->log[0].reg == 0x40 && c->log[0].val == 0x34A8 && c->log[1].reg == 0x07 && c->log[1].val == 0x0AD7);
	CHECK(c->log[2].reg == 0x51 && c->log[2].val == 0x8030);
	CHECK(c->log[3].reg == 0x08 && c->log[3].val == 0x0813 && c->log[4].reg == 0x08 && c->log[4].val == 0x8763);
	chips[0].n = 0;
	bk4819_tx_prepare(&m, BK4819_TONE_DCS_INV, 023);
	CHECK(c->n == 5 && c->log[2].reg == 0x51 && c->log[2].val == 0xA030);
	// DCS ends with the turn-off tail: 0x52 bit 15, then back to 0x028F; again after PTT again, and the end of the
	// transmission ends a tail still on. No tail after the end, for CTCSS, none, or DMR
	chips[0].n = 0;
	CHECK(bk4819_tx_tail_start(&m));
	bk4819_tx_tail_end(&m);
	CHECK(c->n == 2 && c->log[0].reg == 0x52 && c->log[0].val == 0x828F && c->log[1].reg == 0x52 && c->log[1].val == 0x028F);
	CHECK(bk4819_tx_tail_start(&m));
	chips[0].n = 0;
	bk4819_tx_end(&m);
	CHECK(c->n >= 1 && c->log[0].reg == 0x52 && c->log[0].val == 0x028F && !m.tail_on);
	CHECK(!bk4819_tx_tail_start(&m));
	bk4819_tx_prepare(&m, BK4819_TONE_CTCSS, 1000);
	CHECK(!bk4819_tx_tail_start(&m));
	bk4819_tx_prepare(&m, BK4819_TONE_DCS, 023);
	bk4819_dmr_tx_prepare(&m);
	chips[0].n = 0;
	CHECK(!bk4819_tx_tail_start(&m) && c->n == 0);

	// TX on 461.4625 MHz: idle (0x0200), frequency, 0x37, then 0xC3FE. 0x36 is not touched
	chips[0].n = 0;
	bk4819_tx_tune(&m, 461462500u);
	CHECK(c->n == 5);
	CHECK(c->log[0].reg == 0x30 && c->log[0].val == 0x0200);
	CHECK(c->log[1].reg == 0x38 && c->log[1].val == 0x22CA);
	CHECK(c->log[2].reg == 0x39 && c->log[2].val == 0x02C0);
	CHECK(c->log[3].reg == 0x37 && c->log[3].val == 0x1D0F);
	CHECK(c->log[4].reg == 0x30 && c->log[4].val == 0xC3FE && m.state == 0xC3FE);
	CHECK(find_last(c, 0x36) < 0);

	// TX tone (stock order): 47, 70 at the gain, 50 = 0xBB18, 37, idle, 0xC3FA, 71 = Hz x 10.3244 truncated, 50 = 0x3B18
	chips[0].n = 0;
	bk4819_tone_start(&m, 1750, BK4819_TONE_GAIN_STOCK);
	CHECK(c->n == 8);
	CHECK(c->log[0].reg == 0x47 && c->log[0].val == 0x6342);
	CHECK(c->log[1].reg == 0x70 && c->log[1].val == 0xB000);
	CHECK(c->log[2].reg == 0x50 && c->log[2].val == 0xBB18);
	CHECK(c->log[3].reg == 0x37 && c->log[3].val == 0x1D0F);
	CHECK(c->log[4].reg == 0x30 && c->log[4].val == 0x0200);
	CHECK(c->log[5].reg == 0x30 && c->log[5].val == 0xC3FA);
	CHECK(c->log[6].reg == 0x71 && c->log[6].val == 18067);
	CHECK(c->log[7].reg == 0x50 && c->log[7].val == 0x3B18);
	CHECK(m.tone_hz == 1750);
	// Key up: the generator off, the chip stays at 0xC3FA; key down: back on
	chips[0].n = 0;
	bk4819_tone_key(&m, false);
	bk4819_tone_key(&m, true);
	CHECK(c->n == 2 && c->log[0].reg == 0x70 && c->log[0].val == 0 && c->log[1].reg == 0x70 && c->log[1].val == 0xB000);
	// A retune writes the generator and the word only; the gain clamps to 7 bits
	chips[0].n = 0;
	bk4819_tone_start(&m, 1000, 200);
	CHECK(c->n == 3 && c->log[0].reg == 0x70 && c->log[0].val == 0xFF00 && c->log[1].reg == 0x71 && c->log[1].val == 10324);
	// Stop: 47, 50, 70, 37, idle, 0xC3FE, 40 = 0x34A8; a second stop and a key do nothing
	chips[0].n = 0;
	bk4819_tone_stop(&m);
	bk4819_tone_stop(&m);
	bk4819_tone_key(&m, true);
	CHECK(c->n == 7);
	CHECK(c->log[0].reg == 0x47 && c->log[0].val == 0x6042 && c->log[1].reg == 0x50 && c->log[1].val == 0x3B18);
	CHECK(c->log[2].reg == 0x70 && c->log[2].val == 0 && c->log[3].reg == 0x37 && c->log[3].val == 0x1D0F);
	CHECK(c->log[4].reg == 0x30 && c->log[4].val == 0x0200 && c->log[5].reg == 0x30 && c->log[5].val == 0xC3FE);
	CHECK(c->log[6].reg == 0x40 && c->log[6].val == 0x34A8);
	CHECK(m.tone_hz == 0);
	// End of a transmission, with or without a tone: 47, 50, 70, 37, idle, then the RX state; 0x36 stays
	bk4819_tone_start(&m, 1750, 48);
	chips[0].n = 0;
	bk4819_tx_end(&m);
	CHECK(c->n == 6);
	CHECK(c->log[0].reg == 0x47 && c->log[0].val == 0x6042 && c->log[1].reg == 0x50 && c->log[1].val == 0x3B18);
	CHECK(c->log[2].reg == 0x70 && c->log[2].val == 0 && c->log[3].reg == 0x37 && c->log[3].val == 0x1D0F);
	CHECK(c->log[4].reg == 0x30 && c->log[4].val == 0x0200 && c->log[5].reg == 0x30 && c->log[5].val == 0xBFF1);
	CHECK(m.tone_hz == 0 && m.state == BK4819_STATE_RX && find_last(c, 0x36) < 0);
	// DTMF D (941 + 1633 Hz, radio-features.md 3.7): both generators at the stock level, the microphone ADC off
	// (0xC3FA) while it sounds, the stock DTMF deviation; a second digit only retunes; 0 is the tone stop (mic back)
	chips[0].n = 0;
	bk4819_dtmf_start(&m, 941, 1633);
	CHECK(c->n == 10);
	CHECK(c->log[0].reg == 0x47 && c->log[0].val == 0x6342 && c->log[1].reg == 0x70 && c->log[1].val == 0x9090);
	CHECK(c->log[2].reg == 0x50 && c->log[2].val == 0xBB18 && c->log[3].reg == 0x37 && c->log[3].val == 0x1D0F);
	CHECK(c->log[5].reg == 0x30 && c->log[5].val == 0xC3FA && m.state == BK4819_STATE_TX_TONE);
	CHECK(c->log[6].reg == 0x40 && c->log[6].val == 0x348A);
	CHECK(c->log[7].reg == 0x71 && c->log[7].val == 9715 && c->log[8].reg == 0x72 && c->log[8].val == 16860);
	CHECK(c->log[9].reg == 0x50 && c->log[9].val == 0x3B18 && m.tone_hz == 941);
	chips[0].n = 0;
	bk4819_dtmf_start(&m, 697, 1209);
	CHECK(c->n == 3 && c->log[0].reg == 0x71 && c->log[0].val == 7196 && c->log[1].reg == 0x72 && c->log[1].val == 12482);
	CHECK(m.state == BK4819_STATE_TX_TONE);
	chips[0].n = 0;
	bk4819_dtmf_start(&m, 0, 0);
	CHECK(c->n == 7 && c->log[2].reg == 0x70 && c->log[2].val == 0);
	CHECK(c->log[5].reg == 0x30 && c->log[5].val == BK4819_STATE_TX_MAIN && m.state == BK4819_STATE_TX_MAIN);
	CHECK(c->log[6].reg == 0x40 && c->log[6].val == 0x34A8 && m.tone_hz == 0);
	CHECK(bk4819_pa_reg(0) == 0 && bk4819_pa_reg(15) == BK4819_PA_MAX && bk4819_pa_reg(99) == BK4819_PA_MAX);
	CHECK(bk4819_tone_word(697) == 7196 && bk4819_tone_word(852) == 8796 && bk4819_tone_word(1209) == 12482 &&
	      bk4819_tone_word(1336) == 13793 && bk4819_tone_word(1477) == 15249 && bk4819_tone_word(1633) == 16860);

	// Gain changes only the low nibble of the last 0x48 value; out of range clamps
	chips[0].n = 0;
	m.af_gain = 0xB3B3;
	bk4819_set_af_gain(&m, 7);
	CHECK(chips[0].n == 1 && chips[0].log[0].reg == 0x48 && chips[0].log[0].val == 0xB3B7);
	bk4819_set_af_gain(&m, 200);
	CHECK(chips[0].log[1].val == 0xB3BF && m.af_gain == 0xB3BF);

	// Analog mode set (stock): AFC on (0x73 bit 4 cleared, the rest kept), 0x7E, 0x2B, live microphone 0x50
	chips[0].regs[0x73] = 0x0112;
	chips[0].n = 0;
	bk4819_analog_mode(&m);
	CHECK(chips[0].n == 6 && chips[0].log[0].reg == 0x2A && chips[0].log[0].val == 0x4F18);
	CHECK(chips[0].log[1].reg == 0x73 && chips[0].log[1].val == 0x0102);
	CHECK(chips[0].log[2].reg == 0x47 && chips[0].log[2].val == 0x6042);
	CHECK(chips[0].log[3].reg == 0x7E && chips[0].log[3].val == 0x302E);
	CHECK(chips[0].log[4].reg == 0x2B && chips[0].log[4].val == 0xC000);
	CHECK(chips[0].log[5].reg == 0x50 && chips[0].log[5].val == 0x3B18);
	// The channel set: thresholds, bandwidth, and its 0x4F
	chips[0].n = 0;
	bk4819_set_bandwidth(&m, BK4819_BW_25K);
	bk4819_set_bandwidth(&m, BK4819_BW_12K5);
	CHECK(chips[0].n == 6 && chips[0].log[0].reg == 0x52 && chips[0].log[0].val == 0x028F);
	CHECK(chips[0].log[1].reg == 0x43 && chips[0].log[1].val == 0x3028 && chips[0].log[2].reg == 0x4F && chips[0].log[2].val == 0x3332);
	CHECK(chips[0].log[4].reg == 0x43 && chips[0].log[4].val == 0x4048 && chips[0].log[5].reg == 0x4F && chips[0].log[5].val == 0x3D3C);

	chips[0].n = 0;
	bk4819_set_rx_tone(&m, BK4819_TONE_CTCSS, 885);
	CHECK(chips[0].n == 3 && chips[0].log[0].reg == 0x07 && chips[0].log[0].val == 1827);
	CHECK(chips[0].log[2].reg == 0x51 && chips[0].log[2].val == 0x1000);
	chips[0].regs[0x0C] = 0x0400;
	CHECK(bk4819_rx_tone_ok(&m, BK4819_TONE_CTCSS));
	chips[0].regs[0x0C] = 0x0000;
	CHECK(!bk4819_rx_tone_ok(&m, BK4819_TONE_CTCSS));
	// DCS receive, the stock order: the rate, 0x51, the code's two halves, the rate with bit 14. D754 inverted: the
	// stock table's words 0x09EC / 0x820F
	chips[0].n = 0;
	bk4819_set_rx_tone(&m, BK4819_TONE_DCS, 023);
	CHECK(chips[0].n == 5 && chips[0].log[0].reg == 0x07 && chips[0].log[0].val == 0x0AD7);
	CHECK(chips[0].log[1].reg == 0x51 && chips[0].log[1].val == 0x8030);
	CHECK(chips[0].log[2].reg == 0x08 && chips[0].log[2].val == 0x0813 && chips[0].log[3].reg == 0x08 && chips[0].log[3].val == 0x8763);
	CHECK(chips[0].log[4].reg == 0x07 && chips[0].log[4].val == 0x4AD7);
	chips[0].n = 0;
	bk4819_set_rx_tone(&m, BK4819_TONE_DCS_INV, 0754);
	CHECK(chips[0].n == 5 && chips[0].log[1].val == 0xA030 && chips[0].log[2].val == 0x09EC && chips[0].log[3].val == 0x820F);
	// 0x0C: b14 the code, b15 the inverted code; none always passes
	CHECK(bk4819_tone_found(0x4000, BK4819_TONE_DCS) && !bk4819_tone_found(0x8000, BK4819_TONE_DCS));
	CHECK(bk4819_tone_found(0x8000, BK4819_TONE_DCS_INV) && !bk4819_tone_found(0x4400, BK4819_TONE_DCS_INV));
	CHECK(!bk4819_tone_found(0xC000, BK4819_TONE_CTCSS) && bk4819_tone_found(0, BK4819_TONE_NONE));
	// Behind an inverting receiver (the DB25-D's high-side LO) a normal code shows in b15 and an inverted one in b14
	CHECK(bk4819_heard(&m, 0x4000, BK4819_TONE_DCS) && !bk4819_heard(&m, 0x8000, BK4819_TONE_DCS));
	m.rx_inverted = true;
	CHECK(bk4819_heard(&m, 0x8000, BK4819_TONE_DCS) && !bk4819_heard(&m, 0x4000, BK4819_TONE_DCS));
	CHECK(bk4819_heard(&m, 0x4000, BK4819_TONE_DCS_INV) && !bk4819_heard(&m, 0x8000, BK4819_TONE_DCS_INV));
	CHECK(bk4819_heard(&m, 0x0400, BK4819_TONE_CTCSS));
	chips[0].regs[0x0C] = 0x8000;
	CHECK(bk4819_rx_tone_ok(&m, BK4819_TONE_DCS) && !bk4819_rx_tone_ok(&m, BK4819_TONE_DCS_INV));
	m.rx_inverted = false;
	chips[0].n = 0;
	bk4819_set_rx_tone(&m, BK4819_TONE_NONE, 0);
	CHECK(chips[0].n == 1 && chips[0].log[0].reg == 0x51 && chips[0].log[0].val == 0);
	CHECK(chips[0].protocol_errors == 0 && chips[1].protocol_errors == 0);
}

// DMR receive (hardware-db25d.md 4.5): the stock digital mode set in its order; the end is the stock analog mode set
// plus 0x27 and 0x48 as they were
static void test_dmr_rx(void)
{
	static const struct { uint8_t reg; uint16_t val; } want[] = {
		{ 0x2A, 0x4711 }, { 0x51, 0x0000 }, { 0x73, 0x0010 }, { 0x52, 0x028F }, { 0x47, 0x6943 }, { 0x7E, 0x3006 },
		{ 0x43, 0x4444 }, { 0x40, 0x0000 }, { 0x27, 0x0430 }, { 0x2B, 0x8707 }, { 0x48, 0xB174 }, { 0x47, 0x6143 },
	};
	bk4819_t m;
	reset_chips();
	bk4819_attach(&m, main_bus());
	chips[0].regs[0x27] = 0xA430;
	bk4819_set_af_gain(&m, 7);		// the FM volume's gain: 0xB3B7
	chips[0].n = 0;

	bk4819_dmr_rx_enter(&m);
	const chip_t *c = &chips[0];
	CHECK(m.digital && c->n == sizeof(want) / sizeof(want[0]));
	for (size_t i = 0; i < sizeof(want) / sizeof(want[0]) && i < (size_t)c->n; i++) {
		CHECK(c->log[i].reg == want[i].reg && c->log[i].val == want[i].val);
	}
	CHECK(m.af_gain == BK4819_AF_GAIN_DMR);
	bk4819_set_af_gain(&m, 4);		// the board's DMR gain keeps the digital word's upper bits
	CHECK(c->log[c->n - 1].reg == 0x48 && c->log[c->n - 1].val == 0xB174);

	chips[0].n = 0;
	bk4819_dmr_rx_enter(&m);		// a second enter must not save the DMR values over the analog ones
	chips[0].n = 0;
	bk4819_dmr_rx_end(&m);
	CHECK(!m.digital && c->n == 8);
	CHECK(c->log[0].reg == 0x2A && c->log[0].val == 0x4F18);
	CHECK(c->log[1].reg == 0x73 && c->log[1].val == 0);		// analog: AFC on again
	CHECK(c->log[2].reg == 0x47 && c->log[2].val == BK4819_AF_MUTE);
	CHECK(c->log[3].reg == 0x7E && c->log[3].val == 0x302E);
	CHECK(c->log[4].reg == 0x2B && c->log[4].val == 0xC000);
	CHECK(c->log[5].reg == 0x50 && c->log[5].val == BK4819_TX_AUDIO_LIVE);
	CHECK(c->log[6].reg == 0x27 && c->log[6].val == 0xA430);
	CHECK(c->log[7].reg == 0x48 && c->log[7].val == 0xB3B7);
	CHECK(m.af_gain == 0xB3B7);

	chips[0].n = 0;
	bk4819_dmr_rx_end(&m);		// not in DMR: nothing
	CHECK(c->n == 0);
	CHECK(chips[0].protocol_errors == 0);
}

// DMR transmit (4.5): the digital TX words, one burst's retune in the stock order on the main chip (TX on 461.4625 MHz,
// back to the IF) and the LO's resume, which leaves its frequency alone
static void test_dmr_tx(void)
{
	bk4819_t m, l;
	reset_chips();
	bk4819_attach(&m, main_bus());
	bk4819_attach(&l, lo_bus());
	m.idle = BK4819_STATE_IDLE_MAIN;
	const chip_t *c = &chips[0];

	bk4819_dmr_tx_prepare(&m);
	CHECK(c->n == 2 && c->log[0].reg == 0x40 && c->log[0].val == 0x0000 && c->log[1].reg == 0x51 && c->log[1].val == 0x0000);

	chips[0].n = 0;
	bk4819_burst_tune(&m, 461462500u, BK4819_STATE_TX_MAIN);
	CHECK(c->n == 5);
	CHECK(c->log[0].reg == 0x38 && c->log[0].val == 0x22CA);	// 46,146,250 x 10 Hz
	CHECK(c->log[1].reg == 0x39 && c->log[1].val == 0x02C0);
	CHECK(c->log[2].reg == 0x37 && c->log[2].val == BK4819_POWER_ON);
	CHECK(c->log[3].reg == 0x30 && c->log[3].val == 0x0200);
	CHECK(c->log[4].reg == 0x30 && c->log[4].val == 0xC3FE);
	CHECK(m.state == 0xC3FE && m.freq_10hz == 46146250u);

	chips[0].n = 0;
	bk4819_burst_tune(&m, 51550000u, BK4819_STATE_RX);
	CHECK(c->n == 5);
	CHECK(c->log[0].reg == 0x38 && c->log[0].val == 0xA8B8);	// 5,155,000
	CHECK(c->log[1].reg == 0x39 && c->log[1].val == 0x004E);
	CHECK(c->log[3].reg == 0x30 && c->log[3].val == 0x0200);
	CHECK(c->log[4].reg == 0x30 && c->log[4].val == 0xBFF1);

	c = &chips[1];
	bk4819_lo_tune(&l, 461462500u + 51550000u);
	bk4819_lo_off(&l);
	chips[1].n = 0;
	bk4819_lo_resume(&l);
	CHECK(c->n == 3);
	CHECK(c->log[0].reg == 0x37 && c->log[0].val == BK4819_POWER_ON);
	CHECK(c->log[1].reg == 0x30 && c->log[1].val == 0x0000);
	CHECK(c->log[2].reg == 0x30 && c->log[2].val == 0xC1FE);
	CHECK(l.freq_10hz == 51301250u);
	CHECK(chips[0].protocol_errors == 0 && chips[1].protocol_errors == 0);
}

int main(void)
{
	test_helpers();
	test_frames();
	test_init();
	test_sequences();
	test_dmr_rx();
	test_dmr_tx();
	if (failures) {
		printf("bk4819Test: %d failures\n", failures);
		return 1;
	}
	printf("bk4819Test: all passed\n");
	return 0;
}
