// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
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

static const bk4819_bus_t bus_main = {
	.cs = cs_main, .clk = fake_clk, .data_out = fake_data_out, .data_release = fake_release,
	.data_in = fake_data_in, .delay_us = fake_delay, .crit_enter = fake_enter, .crit_exit = fake_exit,
	.delay_ms = fake_delay,
};

static const bk4819_bus_t bus_lo = {
	.cs = cs_lo, .clk = fake_clk, .data_out = fake_data_out, .data_release = fake_release,
	.data_in = fake_data_in, .delay_us = fake_delay, .crit_enter = fake_enter, .crit_exit = fake_exit,
	.delay_ms = fake_delay,
};

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
	CHECK(bk4819_rssi_dbm(0x0000) == -160);
	CHECK(bk4819_rssi_dbm(0x0082) == -95);		// 130 half-dB
	CHECK(bk4819_rssi_dbm(0xFFFF) == 95);		// bits above 8 ignored
}

static void test_frames(void)
{
	bk4819_t m, l;
	reset_chips();
	bk4819_attach(&m, &bus_main);
	bk4819_attach(&l, &bus_lo);
	bk4819_write(&m, 0x30, 0xBFF1);
	CHECK(chips[0].n == 1 && chips[0].log[0].reg == 0x30 && chips[0].log[0].val == 0xBFF1);
	CHECK(chips[1].n == 0);		// the other chip's enable stayed high
	CHECK(m.state == 0xBFF1 && m.writes == 1);
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
	bk4819_attach(&m, &bus_main);
	bk4819_init_main(&m);
	const chip_t *c = &chips[0];
	CHECK(c->log[0].reg == 0x00 && c->log[0].val == 0x8000);	// soft reset, then release
	CHECK(c->log[1].reg == 0x00 && c->log[1].val == 0x0000);
	CHECK(c->log[2].reg == 0x33 && c->log[2].val == 0x0003);
	int i = find_last(c, 0x4F);
	CHECK(i >= 0 && c->log[i].val == 0x3332);
	i = find_last(c, 0x7D);
	CHECK(i >= 0 && c->log[i].val == 0xE34A);
	CHECK(c->log[c->n - 1].reg == 0x30 && c->log[c->n - 1].val == 0);	// ends idle
	CHECK(m.state == 0 && m.af_gain == 0xB3B3);
	CHECK(chips[1].n == 0);

	bk4819_attach(&l, &bus_lo);
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
	CHECK(c->log[c->n - 2].reg == 0x30 && c->log[c->n - 2].val == 0);	// ends off
	CHECK(c->log[c->n - 1].reg == 0x37 && c->log[c->n - 1].val == BK4819_POWER_OFF);
	CHECK(c->protocol_errors == 0 && chips[0].protocol_errors == 0);
}

static void test_sequences(void)
{
	bk4819_t m, l;
	reset_chips();
	bk4819_attach(&m, &bus_main);
	bk4819_attach(&l, &bus_lo);

	bk4819_set_freq(&m, 51550000u);
	const chip_t *c = &chips[0];
	CHECK(c->n == 3);
	CHECK(c->log[0].reg == 0x30 && c->log[0].val == 0);
	CHECK(c->log[1].reg == 0x38 && c->log[1].val == (5155000u & 0xFFFF));
	CHECK(c->log[2].reg == 0x39 && c->log[2].val == (5155000u >> 16));
	CHECK(m.freq_10hz == 5155000u);

	// RX -> TX goes through idle; idle -> RX is one write
	chips[0].n = 0;
	bk4819_set_state(&m, BK4819_STATE_RX);
	CHECK(c->n == 1 && c->log[0].val == BK4819_STATE_RX);
	chips[0].n = 0;
	bk4819_set_state(&m, BK4819_STATE_TX);
	CHECK(c->n == 2 && c->log[0].val == BK4819_STATE_IDLE && c->log[1].val == BK4819_STATE_TX);

	// The LO for 446.000 MHz sits 51.55 MHz above: 497.55 MHz
	bk4819_lo_tune(&l, 446000000u + 51550000u);
	c = &chips[1];
	CHECK(c->n == 6);
	CHECK(c->log[0].reg == 0x30 && c->log[0].val == 0);
	CHECK(c->log[1].reg == 0x38 && c->log[1].val == (49755000u & 0xFFFF));
	CHECK(c->log[2].reg == 0x39 && c->log[2].val == (49755000u >> 16));
	CHECK(c->log[3].reg == 0x37 && c->log[3].val == BK4819_POWER_ON);
	CHECK(c->log[4].reg == 0x30 && c->log[4].val == 0);
	CHECK(c->log[5].reg == 0x30 && c->log[5].val == BK4819_STATE_TX);

	chips[1].n = 0;
	bk4819_lo_off(&l);
	CHECK(c->n == 2 && c->log[0].val == 0 && c->log[1].reg == 0x37 && c->log[1].val == BK4819_POWER_OFF);

	// Gain changes only the low nibble of the last 0x48 value; out of range clamps
	chips[0].n = 0;
	m.af_gain = 0xB3B3;
	bk4819_set_af_gain(&m, 7);
	CHECK(chips[0].n == 1 && chips[0].log[0].reg == 0x48 && chips[0].log[0].val == 0xB3B7);
	bk4819_set_af_gain(&m, 200);
	CHECK(chips[0].log[1].val == 0xB3BF && m.af_gain == 0xB3BF);

	chips[0].n = 0;
	bk4819_set_rx_ctcss(&m, 885);
	CHECK(chips[0].n == 3 && chips[0].log[0].reg == 0x07 && chips[0].log[0].val == 1827);
	CHECK(chips[0].log[2].reg == 0x51 && chips[0].log[2].val == 0x1000);
	chips[0].regs[0x0C] = 0x0400;
	CHECK(bk4819_ctcss_found(&m));
	chips[0].regs[0x0C] = 0x0000;
	CHECK(!bk4819_ctcss_found(&m));
	CHECK(chips[0].protocol_errors == 0 && chips[1].protocol_errors == 0);
}

int main(void)
{
	test_helpers();
	test_frames();
	test_init();
	test_sequences();
	if (failures) {
		printf("bk4819Test: %d failures\n", failures);
		return 1;
	}
	printf("bk4819Test: all passed\n");
	return 0;
}
