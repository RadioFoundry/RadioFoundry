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
	// The init table's 0xB3 / 0xB4 / 0xC7 addresses go out unmasked
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
	CHECK(chip.n == 57 + 20);
	CHECK(chip.log[0].reg == 0x00 && chip.log[0].val == 0x8000);	// soft reset first
	CHECK(chip.log[56].reg == 0x09 && chip.log[56].val == 0xF09F);	// last init entry
	CHECK(chip.log[76].reg == 0x48 && chip.log[76].val == 0xBBBF);	// last post-init entry
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
	chip.n = 0;
	fd6818_set_mode(&dev, FD6818_MODE_FM);
	i = find_last(0x43);
	CHECK(i >= 0 && chip.log[i].val == 0x3028);

	chip.n = 0;
	fd6818_set_tx_ctcss(&dev, 885);
	CHECK(chip.n == 2 && chip.log[0].reg == 0x07 && chip.log[0].val == 1827 && chip.log[1].val == 0x9045);
	chip.n = 0;
	fd6818_set_rx_ctcss(&dev, 0);
	CHECK(chip.n == 1 && chip.log[0].reg == 0x51 && chip.log[0].val == 0);
}

int main(void)
{
	test_helpers();
	test_frames();
	test_sequences();
	if (failures) {
		printf("fd6818Test: %d failures\n", failures);
		return 1;
	}
	printf("fd6818Test: all passed\n");
	return 0;
}
