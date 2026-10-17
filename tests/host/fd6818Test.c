// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// FD6818 driver: band words, CTCSS word, and the bit-banged frames against a fake chip that samples the data line on
// rising clock edges while CS is low (docs/spec/hardware-dm32.md 8.1-8.4)
#include <stdio.h>
#include <string.h>
#include "rf/fd6818/fd6818.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- Fake chip on the three wires ---

typedef struct {
	uint8_t reg;
	uint16_t val;
} frame_t;

static struct {
	bool cs, clk, data, driving;
	uint32_t shift;
	int bits;
	bool reading;
	uint16_t out;		// value the chip shifts out in a read
	int out_bits;
	frame_t log[256];
	int n;
	int crit_depth, crit_max;
	int protocol_errors;
	uint16_t regs[256];
} chip;

static void fake_cs(bool level)
{
	if (!chip.cs && level) {	// end of a frame
		if (!chip.reading) {
			if (chip.bits != 24) {
				chip.protocol_errors++;
			} else if (chip.n < 256) {
				chip.log[chip.n].reg = (uint8_t)(chip.shift >> 16);
				chip.log[chip.n].val = (uint16_t)chip.shift;
				chip.regs[(chip.shift >> 16) & 0xFF] = (uint16_t)chip.shift;
				chip.n++;
			}
		}
		chip.reading = false;
	}
	if (chip.cs && !level) {
		chip.driving = true;
		chip.shift = 0;
		chip.bits = 0;
		chip.reading = false;
	}
	chip.cs = level;
}

static void fake_clk(bool level)
{
	bool rising = !chip.clk && level;
	chip.clk = level;
	if (!rising || chip.cs) {
		return;
	}
	// A read is an address with bit 7 set followed by the MCU letting go of the data line; an address with bit 7
	// set that the MCU keeps driving is a write (the init table's 0xB3 / 0xB4 / 0xC7 entries)
	if (!chip.driving) {
		if (!chip.reading) {
			if (chip.bits != 8 || (chip.shift & 0x80u) == 0) {
				chip.protocol_errors++;
			}
			chip.reading = true;
			chip.out = chip.regs[chip.shift & 0x7Fu];
			chip.out_bits = 0;
		}
		chip.out_bits++;
		return;
	}
	chip.shift = (chip.shift << 1) | (chip.data ? 1u : 0u);
	chip.bits++;
}

static void fake_data_out(bool level)
{
	chip.data = level;
	chip.driving = true;
}

static void fake_release(void)
{
	chip.driving = false;
}

static bool fake_data_in(void)
{
	// Bit n (from the MSB) is on the line while the clock is high after the n-th rising edge of the read
	int n = chip.out_bits - 1;
	if (n < 0 || n > 15) {
		return false;
	}
	return (chip.out >> (15 - n)) & 1u;
}

static void fake_delay(uint32_t us)
{
	(void)us;
}

static uint32_t fake_enter(void)
{
	if (++chip.crit_depth > chip.crit_max) {
		chip.crit_max = chip.crit_depth;
	}
	return 0;
}

static void fake_exit(uint32_t s)
{
	(void)s;
	chip.crit_depth--;
}

static const fd6818_bus_t bus = {
	.cs = fake_cs, .clk = fake_clk, .data_out = fake_data_out, .data_release = fake_release,
	.data_in = fake_data_in, .delay_us = fake_delay, .crit_enter = fake_enter, .crit_exit = fake_exit,
	.delay_ms = fake_delay,
};

static void reset_chip(void)
{
	memset(&chip, 0, sizeof chip);
	chip.cs = true;
}

static int find_last(uint8_t reg)
{
	for (int i = chip.n - 1; i >= 0; i--) {
		if (chip.log[i].reg == reg) {
			return i;
		}
	}
	return -1;
}

static void test_helpers(void)
{
	uint16_t a, b;
	CHECK(fd6818_band_words(14400000u, &a, &b) && a == 0xAF28 && b == 0x375C);	// 144 MHz: >= 124 MHz row
	CHECK(fd6818_band_words(43500000u, &a, &b) && a == 0x2F50 && b == 0x39F0);	// 435 MHz: >= 370 MHz row
	CHECK(fd6818_band_words(18500000u, &a, &b) && a == 0x3F48 && b == 0x38F0);	// 185 MHz
	CHECK(fd6818_band_words(18499999u, &a, &b) && a == 0x3F48);			// the row's own boundary value
	CHECK(fd6818_band_words(18499998u, &a, &b) && a == 0xAF28);
	CHECK(!fd6818_band_words(1500000u, &a, &b));					// below 16 MHz: nothing
	CHECK(fd6818_ctcss_word(885) == 1827);	// 88.5 Hz x 20.64888 (the old port's integer division gave 1651)
	CHECK(fd6818_ctcss_word(1000) == 2065);
	CHECK(fd6818_ctcss_word(2541) == 5247);
	CHECK(fd6818_rssi_raw(0x01FF) == 0xFF && fd6818_rssi_raw(0xFE00) == 0);
	CHECK(fd6818_rssi_dbm(100) == -110 && fd6818_rssi_dbm(0xFE00) == -160);	// guide: reg / 2 - 160
	CHECK(fd6818_noise(0xAB08) == 33 && fd6818_noise(0x00FF) == 255);	// low byte only, capped
	// the measured floor (43) stays shut at the loosest squelch level (opens below 64), a strong carrier (8) opens the
	// tightest (below 40): core/radio/radio_sm.h RADIO_SQL_*
	CHECK(fd6818_noise(43) >= 64 && fd6818_noise(8) < 40);
}

static void test_frames(void)
{
	fd6818_t dev;
	reset_chip();
	fd6818_attach(&dev, &bus);
	fd6818_write(&dev, 0x30, 0xBFF1);
	CHECK(chip.n == 1 && chip.log[0].reg == 0x30 && chip.log[0].val == 0xBFF1);
	CHECK(chip.protocol_errors == 0);
	CHECK(dev.state == 0xBFF1 && dev.writes == 1);
	CHECK(chip.crit_depth == 0 && chip.crit_max == 1);
	// The address goes out unmasked (bit 7 set is a read cycle on the chip: the sequences never send one)
	fd6818_write(&dev, 0xB3, 0xFE00);
	CHECK(chip.n == 2 && chip.log[1].reg == 0xB3 && chip.log[1].val == 0xFE00);

	chip.regs[0x67] = 0x0123;
	uint16_t v = fd6818_read(&dev, 0x67);
	CHECK(v == 0x0123);
	CHECK(chip.protocol_errors == 0);
	CHECK(chip.n == 2);	// a read logs no write
	chip.regs[0x65] = 0xA55A;
	CHECK(fd6818_read(&dev, 0x65) == 0xA55A);
}

static void test_sequences(void)
{
	fd6818_t dev;
	reset_chip();
	fd6818_attach(&dev, &bus);
	fd6818_init(&dev);
	CHECK(chip.n == 53 + 20);
	CHECK(chip.log[0].reg == 0x00 && chip.log[0].val == 0x8000);	// soft reset first
	CHECK(chip.log[52].reg == 0x09 && chip.log[52].val == 0xF09F);	// last init entry
	for (int i = 0; i < chip.n; i++) {
		CHECK(chip.log[i].reg < 0x80);	// no read cycle disguised as a write (the earlier 0xB3 / 0xB4 entries)
	}
	CHECK(chip.log[72].reg == 0x48 && chip.log[72].val == 0xBBBF);	// last post-init entry
	CHECK(chip.protocol_errors == 0);

	chip.n = 0;
	fd6818_set_freq(&dev, 144000000u);
	CHECK(chip.n == 5);
	CHECK(chip.log[0].reg == 0x30 && chip.log[0].val == 0);			// idle first
	CHECK(chip.log[1].reg == 0x1A && chip.log[1].val == 0xAF28);
	CHECK(chip.log[2].reg == 0x40 && chip.log[2].val == 0x375C);
	CHECK(chip.log[3].reg == 0x38 && chip.log[3].val == 0xBA00);
	CHECK(chip.log[4].reg == 0x39 && chip.log[4].val == 0x00DB);		// 144.000 MHz = 0x00DBBA00
	CHECK(dev.freq_10hz == 14400000u);

	// RX -> TX goes through idle; idle -> RX doesn't add an extra write
	chip.n = 0;
	fd6818_set_state(&dev, FD6818_STATE_RX);
	CHECK(chip.n == 1 && chip.log[0].val == FD6818_STATE_RX);
	chip.n = 0;
	fd6818_set_state(&dev, FD6818_STATE_TX);
	CHECK(chip.n == 2 && chip.log[0].val == FD6818_STATE_IDLE && chip.log[1].val == FD6818_STATE_TX);

	chip.n = 0;
	fd6818_dmr_burst(&dev, false, FD6818_BW_DMR);
	CHECK(chip.n == 3 && chip.log[0].reg == 0x43 && chip.log[0].val == 0x4048 && chip.log[2].val == FD6818_STATE_RX);
	chip.n = 0;
	fd6818_dmr_burst(&dev, true, FD6818_BW_DMR);
	CHECK(chip.n == 2 && chip.log[0].val == 0 && chip.log[1].val == FD6818_STATE_TX);

	chip.n = 0;
	fd6818_set_mode(&dev, FD6818_MODE_DMR);
	int i = find_last(0x43);
	CHECK(i >= 0 && chip.log[i].val == 0x4048);
	CHECK(chip.log[chip.n - 1].reg == 0x47 && chip.log[chip.n - 1].val == 0x0140);
	for (int k = 0; k < chip.n; k++) {
		CHECK(chip.log[k].reg < 0x80);	// the earlier 0xC7 entry was a read cycle
	}
	chip.n = 0;
	fd6818_set_mode(&dev, FD6818_MODE_FM);
	i = find_last(0x43);
	CHECK(i >= 0 && chip.log[i].val == 0x3028);

	chip.n = 0;
	fd6818_set_tx_ctcss(&dev, 885);
	CHECK(chip.n == 2 && chip.log[0].reg == 0x07 && chip.log[0].val == 1827 && chip.log[1].val == 0x9045);
}

// Receiver power saving in the radio's own order: idle, then 0x37 low; waking puts 0x37 back and leaves the state idle
static void test_rx_sleep(void)
{
	fd6818_t dev;
	reset_chip();
	fd6818_attach(&dev, &bus);
	fd6818_rx_sleep(&dev, true, FD6818_MODE_FM);
	CHECK(chip.n == 2 && chip.log[0].reg == 0x30 && chip.log[0].val == 0 && chip.log[1].reg == 0x37 &&
		chip.log[1].val == 0x1D11);
	CHECK(dev.state == FD6818_STATE_IDLE);
	chip.n = 0;
	fd6818_rx_sleep(&dev, true, FD6818_MODE_DMR);
	CHECK(chip.n == 2 && chip.log[1].val == 0x1D00);
	chip.n = 0;
	fd6818_rx_sleep(&dev, false, FD6818_MODE_FM);
	CHECK(chip.n == 2 && chip.log[0].reg == 0x37 && chip.log[0].val == 0x1F1F && chip.log[1].val == 0x1F0F);
}

// RX sub-audio set-up in the radio's own order, and the 0x0C detect bits
static void test_rx_tones(void)
{
	fd6818_t dev;
	reset_chip();
	fd6818_attach(&dev, &bus);

	fd6818_set_rx_tone(&dev, FD6818_TONE_CTCSS, 1000);	// 100.0 Hz
	CHECK(chip.n == 4);
	CHECK(chip.log[0].reg == 0x07 && chip.log[0].val == 0x0811);
	CHECK(chip.log[1].reg == 0x07 && chip.log[1].val == 0x21CE);
	CHECK(chip.log[2].reg == 0x52 && chip.log[2].val == 0x028F);
	CHECK(chip.log[3].reg == 0x51 && chip.log[3].val == 0x9045);

	chip.n = 0;
	fd6818_set_rx_tone(&dev, FD6818_TONE_DCS, 023);	// the datasheet's example: word 0x763813
	CHECK(chip.n == 4);
	CHECK(chip.log[0].reg == 0x07 && chip.log[0].val == 0x0AD7);
	CHECK(chip.log[1].reg == 0x08 && chip.log[1].val == 0x0813);
	CHECK(chip.log[2].reg == 0x08 && chip.log[2].val == 0x8763);
	CHECK(chip.log[3].reg == 0x51 && chip.log[3].val == 0x8045);

	chip.n = 0;
	fd6818_set_rx_tone(&dev, FD6818_TONE_DCS_INV, 023);
	CHECK(chip.n == 4 && chip.log[3].reg == 0x51 && chip.log[3].val == 0xA045);

	chip.n = 0;
	fd6818_set_rx_tone(&dev, FD6818_TONE_NONE, 0);
	CHECK(chip.n == 3);
	CHECK(chip.log[0].reg == 0x07 && chip.log[0].val == 0x0470);
	CHECK(chip.log[1].reg == 0x07 && chip.log[1].val == 0x21CE);
	CHECK(chip.log[2].reg == 0x51 && chip.log[2].val == 0);

	CHECK(fd6818_tone_found(0x0000, FD6818_TONE_NONE));
	CHECK(fd6818_tone_found(0x0400, FD6818_TONE_CTCSS) && !fd6818_tone_found(0xC000, FD6818_TONE_CTCSS));
	CHECK(fd6818_tone_found(0x4000, FD6818_TONE_DCS) && !fd6818_tone_found(0x8400, FD6818_TONE_DCS));
	CHECK(fd6818_tone_found(0x8000, FD6818_TONE_DCS_INV) && !fd6818_tone_found(0x4400, FD6818_TONE_DCS_INV));

	chip.regs[0x0C] = 0x4000;
	CHECK(fd6818_rx_tone_ok(&dev, FD6818_TONE_DCS) && !fd6818_rx_tone_ok(&dev, FD6818_TONE_CTCSS));
	CHECK(fd6818_rx_tone_ok(&dev, FD6818_TONE_NONE));
}

// The DM-1801 Pro set (docs/spec/hardware-dm1801pro.md 5): its own tables, the 0x0200 step, no 0x1A word
static void test_dm1801pro(void)
{
	fd6818_t dev;
	reset_chip();
	fd6818_attach_variant(&dev, &bus, &fd6818_variant_dm1801pro);
	fd6818_init(&dev);
	CHECK(chip.log[0].reg == 0x00 && chip.log[0].val == 0x8000);
	CHECK(chip.log[chip.n - 1].reg == 0x24 && chip.log[chip.n - 1].val == 0x8C7E);
	CHECK(dev.freq_10hz == 0);
	CHECK(chip.protocol_errors == 0);

	chip.n = 0;
	fd6818_set_freq(&dev, 446000000u);
	CHECK(chip.n == 4);
	CHECK(chip.log[0].reg == 0x30 && chip.log[0].val == 0);
	CHECK(chip.log[1].reg == 0x40 && chip.log[1].val == 0x34F0);	// no 0x1A write
	CHECK(chip.log[2].reg == 0x38 && chip.log[2].val == 0x8AC0);
	CHECK(chip.log[3].reg == 0x39 && chip.log[3].val == 0x02A8);	// 446.000 MHz = 0x02A88AC0

	chip.n = 0;
	fd6818_set_state(&dev, FD6818_STATE_RX);
	CHECK(chip.n == 3 && chip.log[0].reg == 0x37 && chip.log[1].val == 0x0200 && chip.log[2].val == 0xBFF1);
	chip.n = 0;
	fd6818_set_state(&dev, FD6818_STATE_TX);
	CHECK(chip.n == 5 && chip.log[0].val == FD6818_STATE_IDLE && chip.log[1].reg == 0x37 && chip.log[2].reg == 0x52 &&
		chip.log[3].val == 0x0200 && chip.log[4].val == 0xC1FE);
	CHECK(dev.state == 0xC1FE);
	chip.n = 0;
	fd6818_set_state(&dev, FD6818_STATE_IDLE);
	CHECK(chip.n == 1 && dev.state == 0);

	// DMR is the five mode registers and 0x3F = 0 (the radio's DMR receive value, 5.4); going back to FM restores 0x48
	// and ends on the 0x2B mode word
	chip.n = 0;
	fd6818_set_mode(&dev, FD6818_MODE_DMR);
	CHECK(chip.n == 6 && chip.log[0].reg == 0x47 && chip.log[0].val == 0x0140 && chip.log[1].reg == 0x48 &&
		chip.log[1].val == 0xB2F5 && chip.log[2].reg == 0x7E && chip.log[2].val == 0xB03F &&
		chip.log[3].reg == 0x2B && chip.log[3].val == 0x0700);
	CHECK(chip.log[5].reg == 0x3F && chip.log[5].val == 0x0000);
	chip.n = 0;
	fd6818_set_mode(&dev, FD6818_MODE_FM);
	CHECK(chip.n == 6 && chip.log[3].reg == 0x48 && chip.log[3].val == 0xB386 && chip.log[5].reg == 0x2B &&
		chip.log[5].val == 0xC000);

	// A DMR burst switches straight between the final words: no idle, no bandwidth
	chip.n = 0;
	fd6818_dmr_burst(&dev, true, dev.variant->bw_dmr);
	CHECK(chip.n == 2 && chip.log[0].reg == 0x52 && chip.log[1].reg == 0x30 && chip.log[1].val == 0xC1FE);
	chip.n = 0;
	fd6818_dmr_burst(&dev, false, dev.variant->bw_dmr);
	CHECK(chip.n == 1 && chip.log[0].reg == 0x30 && chip.log[0].val == 0xBFF1);
	CHECK(chip.protocol_errors == 0);
}

// A variant with burst tables switches straight between the two states: only the table, no idle, no bandwidth
static void test_burst_tables(void)
{
	static const fd6818_regval_t brx[] = { { 0x30, 0xBFF1 } };
	static const fd6818_regval_t btx[] = { { 0x52, 0x028F }, { 0x30, 0xC1FE } };
	fd6818_variant_t v = fd6818_variant_dm1801pro;
	v.burst_rx = brx;
	v.burst_rx_n = 1;
	v.burst_tx = btx;
	v.burst_tx_n = 2;
	fd6818_t dev;
	reset_chip();
	fd6818_attach_variant(&dev, &bus, &v);
	chip.n = 0;
	fd6818_dmr_burst(&dev, true, v.bw_dmr);
	CHECK(chip.n == 2 && chip.log[0].reg == 0x52 && chip.log[0].val == 0x028F && chip.log[1].reg == 0x30 &&
		chip.log[1].val == 0xC1FE);
	chip.n = 0;
	fd6818_dmr_burst(&dev, false, v.bw_dmr);
	CHECK(chip.n == 1 && chip.log[0].reg == 0x30 && chip.log[0].val == 0xBFF1);
	CHECK(chip.protocol_errors == 0);
	// The DM-32UV's set has none: idle first
	fd6818_attach_variant(&dev, &bus, &fd6818_variant_dm32);
	chip.n = 0;
	fd6818_dmr_burst(&dev, true, FD6818_BW_DMR);
	CHECK(chip.n >= 2 && chip.log[0].reg == 0x30 && chip.log[0].val == 0);
}

int main(void)
{
	test_dm1801pro();
	test_burst_tables();
	test_helpers();
	test_frames();
	test_sequences();
	test_rx_tones();
	test_rx_sleep();
	if (failures) {
		printf("fd6818Test: %d failures\n", failures);
		return 1;
	}
	printf("fd6818Test: all passed\n");
	return 0;
}
