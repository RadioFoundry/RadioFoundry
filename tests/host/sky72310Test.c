// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// SKY72310 synthesizer: divider arithmetic against the data sheet's worked example (SKY72310-11 data sheet 203639L,
// figure 4 case 1), the serial word layout and write order against a model of the chip's registers, and the MD-380
// frequency plan's ratios (docs/spec/hardware-md380.md 5.2)
#include <stdio.h>
#include <string.h>
#include "rf/sky72310/sky72310.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- Chip model: 16 write-only registers, the divide ratio latched when the dividend MSB register is written ---

static struct {
	uint16_t reg[16];
	uint16_t words[64];
	int n;
	uint32_t latched_n_reg, latched_dividend;
	int latches;
	bool lock;
} chip;

static bool model_write(void *ctx, uint16_t word)
{
	uint8_t a = (uint8_t)(word >> 12);
	if (chip.n < 64) {
		chip.words[chip.n] = word;
	}
	chip.n++;
	chip.reg[a] = word & 0x0FFFu;
	if (a == SKY72310_REG_DIVIDEND_MSB) {
		chip.latched_n_reg = chip.reg[SKY72310_REG_DIVIDER] & 0x1FFu;
		chip.latched_dividend = ((uint32_t)(chip.reg[SKY72310_REG_DIVIDEND_MSB] & 0x3FFu) << 8) |
					(chip.reg[SKY72310_REG_DIVIDEND_LSB] & 0xFFu);
		chip.latches++;
	}
	return true;
}

static bool model_locked(void *ctx)
{
	return chip.lock;
}

static void no_delay(uint32_t us)
{
	(void)us;
}

static const sky72310_bus_t bus = { model_write, model_locked, no_delay, NULL };

static uint32_t absdiff(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

static void test_datasheet_example(void)
{
	// Case 1: 902.4530 MHz from a 40 MHz crystal divided by 2: N 45.12265, N_reg 13, dividend 32152
	// (MSB register 00 0111 1101, LSB register 1001 1000)
	sky72310_ratio_t r;
	CHECK(sky72310_ratio(40000000u, 2, 902453000u, &r));
	CHECK(r.n_reg == 13);
	CHECK(r.dividend == 32152u);
	CHECK((r.dividend >> 8) == 0x07Du);
	CHECK((r.dividend & 0xFFu) == 0x98u);
	// Step 20 MHz / 2^18 = 76.3 Hz: the frequency back is within one step
	CHECK(absdiff(sky72310_vco_hz(40000000u, 2, &r), 902453000u) <= 77u);
}

static void test_limits(void)
{
	sky72310_ratio_t r;
	CHECK(!sky72310_ratio(16800000u, 0, 400000000u, &r));		// no divider 0
	CHECK(!sky72310_ratio(16800000u, 33, 400000000u, &r));		// 1..32
	CHECK(!sky72310_ratio(16800000u, 1, 400000000u, &r));		// N 23.8: below the fractional range
	CHECK(sky72310_ratio(16800000u, 2, 400000000u, &r));		// N 47.6
	CHECK(!sky72310_ratio(1000000u, 1, 600000000u, &r));		// N 600: above it
	// Every frequency of the fractional range maps back within one step
	for (uint32_t f = 340000000u; f <= 450000000u; f += 1234567u) {
		CHECK(sky72310_ratio(16800000u, 2, f, &r));
		CHECK(r.n_reg >= 6 && r.n_reg <= 505);
		CHECK(r.dividend < (1u << 18));
		CHECK(absdiff(sky72310_vco_hz(16800000u, 2, &r), f) <= 33u);	// 8.4 MHz / 2^18 = 32 Hz
	}
	// A fraction that rounds up to the next whole N carries into N_reg
	CHECK(sky72310_ratio(16800000u, 2, 8400000u * 48u - 1u, &r));
	CHECK(r.n_reg == 16 && r.dividend == 0);
}

// Signed mode: a fraction over one half goes to the next N with a negative dividend. Unsigned, these left the MD-380
// 8.4 MHz low (issue 108: 443.700 MHz RX, LO 393.75 MHz, N 46.875, received nothing; N 47.158 at 446.075 did)
static void test_signed_fraction(void)
{
	sky72310_ratio_t r;
	CHECK(sky72310_ratio(16800000u, 2, 393750000u, &r));	// N 46.875
	CHECK(r.n_reg == 47 - 32);
	CHECK(r.dividend == (1u << 18) - 32768u);		// -0.125 x 2^18
	CHECK(sky72310_vco_hz(16800000u, 2, &r) == 393750000u);
	CHECK(sky72310_ratio(16800000u, 2, 396125000u, &r));	// N 47.158: below one half, positive
	CHECK(r.n_reg == 47 - 32);
	CHECK(r.dividend < (1u << 17));
	// Every fraction stays within -0.5 .. +0.5 and maps back within one step
	for (uint32_t f = 340000000u; f <= 450000000u; f += 777777u) {
		CHECK(sky72310_ratio(16800000u, 2, f, &r));
		uint32_t mag = r.dividend >= (1u << 17) ? (1u << 18) - r.dividend : r.dividend;
		CHECK(mag <= (1u << 17));
		CHECK(absdiff(sky72310_vco_hz(16800000u, 2, &r), f) <= 33u);
	}
}

static void test_words_and_order(void)
{
	sky72310_t dev;
	memset(&chip, 0, sizeof(chip));
	sky72310_attach(&dev, &bus, 16800000u, 12);
	CHECK(sky72310_init(&dev));
	CHECK(chip.reg[SKY72310_REG_POWER_MUX] == 0);		// powered, fractional-N, 18-bit, Mux_out = oscillator
	CHECK(chip.reg[SKY72310_REG_DELTA_SIGMA] == 0x0A0u);	// figure 13: bits 11-7 00001, signed mode
	CHECK(chip.reg[SKY72310_REG_MOD_CTRL] == 0);
	CHECK(chip.reg[SKY72310_REG_PD_GAIN] == 12);
	CHECK(chip.words[0] >> 12 == SKY72310_REG_POWER_MUX);

	chip.n = 0;
	CHECK(sky72310_set_vco(&dev, 2, 396050000u));		// MD-380 UHF RX at 446.000 MHz, LO 49.95 MHz below
	CHECK(chip.n == 4);
	CHECK(chip.words[0] == sky72310_word(SKY72310_REG_REF_DIV, 1));	// divider 2 = index 1
	CHECK(chip.words[1] >> 12 == SKY72310_REG_DIVIDER);
	CHECK(chip.words[2] >> 12 == SKY72310_REG_DIVIDEND_LSB);
	CHECK(chip.words[3] >> 12 == SKY72310_REG_DIVIDEND_MSB);	// last: the ratio takes effect here
	CHECK(chip.latches == 1);
	sky72310_ratio_t got = { (uint16_t)chip.latched_n_reg, chip.latched_dividend };
	CHECK(absdiff(sky72310_vco_hz(16800000u, 2, &got), 396050000u) <= 33u);
	CHECK(dev.vco_hz == 396050000u);

	// Same divider: no reference divider write
	chip.n = 0;
	CHECK(sky72310_set_vco(&dev, 2, 446000000u));
	CHECK(chip.n == 3);
	CHECK(chip.latches == 2);
	// Out of range: nothing written, the last frequency forgotten only on a bus error
	chip.n = 0;
	CHECK(!sky72310_set_vco(&dev, 2, 100000000u));
	CHECK(chip.n == 0);
	CHECK(dev.vco_hz == 446000000u);

	// Power down keeps the Mux_out selection
	dev.mux = SKY72310_MUX_FPD;
	CHECK(sky72310_power_down(&dev, true));
	CHECK(chip.reg[SKY72310_REG_POWER_MUX] == ((SKY72310_MUX_FPD << SKY72310_MUX_SHIFT) | SKY72310_PWR_FULL_DOWN));
	CHECK(sky72310_power_down(&dev, false));
	CHECK(chip.reg[SKY72310_REG_POWER_MUX] == (SKY72310_MUX_FPD << SKY72310_MUX_SHIFT));

	chip.lock = false;
	CHECK(!sky72310_wait_lock(&dev, 100));
	chip.lock = true;
	CHECK(sky72310_wait_lock(&dev, 100));
}

int main(void)
{
	test_datasheet_example();
	test_limits();
	test_words_and_order();
	test_signed_fraction();
	printf("sky72310Test: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
