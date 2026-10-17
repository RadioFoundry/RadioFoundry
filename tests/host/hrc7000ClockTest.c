// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 clock arithmetic (platform/hrc7000/clock_math.h): the PLL words clock.c writes and the rates they give, the
// UART divisor at every APB clock the boards run, and the bit-bang phase length. The DB25-D runs 168 / 84 MHz and hands
// over to the runtime stub (built for 72 / 36 MHz) with the loader's clocks restored.
#include <stdint.h>
#include <stdio.h>
#include "../../platform/hrc7000/clock_math.h"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

#define XTAL 24000000u

static int within_pct(uint32_t v, uint32_t ref, uint32_t tenths)
{
	uint32_t d = v > ref ? v - ref : ref - v;
	return (uint64_t)d * 1000u <= (uint64_t)ref * tenths;
}

int main(void)
{
	// clock.c's set-up: 24 MHz / 1 x 28 / 2 / 2 = 168 MHz, VCO 672 MHz, and the word keeps the PLL's flag bits
	uint32_t w = hrc_pll_word(0, 1, 28, 2, 2);
	CHECK(w == ((1u << 18) | (28u << 6) | (2u << 3) | 2u));
	CHECK(hrc_pll_out_hz(w, XTAL) == 168000000u);
	CHECK(hrc_pll_word(0xFF000000u, 1, 28, 2, 2) == (0xFF000000u | w));
	CHECK(hrc_pll_word(0xFFFFFFFFu, 1, 28, 2, 2) == (0xFF000000u | w));

	// The DB25-D loader's 72 MHz (24 MHz x 24 / 2 / 4) is a different word, and the decoder reads it back
	uint32_t lw = hrc_pll_word(0, 1, 24, 2, 4);
	CHECK(hrc_pll_out_hz(lw, XTAL) == 72000000u);
	CHECK(lw != w);
	CHECK(hrc_pll_out_hz(hrc_pll_word(0, 1, 24, 4, 2), XTAL) == 72000000u);
	// The reset default of the BPLL (user guide 4.2.5.4): refdiv 2, fbdiv 0x28, 2, 2 = 120 MHz
	CHECK(hrc_pll_out_hz((2u << 18) | (0x28u << 6) | (2u << 3) | 2u, XTAL) == 120000000u);
	CHECK(hrc_pll_out_hz(0, XTAL) == 0);	// no divider set: no answer, no divide by zero

	// The rest follows from APB = CPU / 2, and the timers need a whole number of cycles per microsecond and per tick
	uint32_t cpu[2] = { 168000000u, 72000000u };
	for (int i = 0; i < 2; i++) {
		uint32_t apb = cpu[i] / 2u;
		CHECK(apb % 1000000u == 0);
		CHECK(apb % 1000u == 0);
		// The host UART at 115200: nearest divisor, within 3 % at 36 MHz (20: 2.3 % slow), 1 % at 84 MHz (46)
		uint32_t d = hrc_uart_div(apb, 115200u);
		CHECK(d == (i == 0 ? 46u : 20u));
		CHECK(within_pct(hrc_uart_baud(apb, d), 115200u, i == 0 ? 10u : 30u));
		// The bit-banged display bus: each phase at least 30 ns (one APB cycle per write), and not absurdly long
		uint32_t r = hrc_bitbang_repeat(apb);
		CHECK(r >= 1 && (uint64_t)r * 1000000000u >= (uint64_t)apb * 30u);
		CHECK(r <= 4);
		// SPI2 (display) at divider 6 stays under the ST7735S's 15 MHz, NAND SPI0 at divider 4 under its 133 MHz
		CHECK(apb / 6u <= 15000000u);
		CHECK(apb / 4u <= 133000000u);
	}
	CHECK(hrc_bitbang_repeat(36000000u) == 2 && hrc_bitbang_repeat(84000000u) == 3);
	// The ROM's own divisor at 24 MHz (the stub's ROM-load build) is /13
	CHECK(hrc_uart_div(24000000u, 115200u) == 13u);
	// The stub hand-over: the divisor for the loader's APB differs from ours, so the firmware sets it after the restore
	CHECK(hrc_uart_div(84000000u, 115200u) != hrc_uart_div(36000000u, 115200u));
	CHECK(hrc_uart_div(0, 115200u) == 0);

	if (failures == 0) {
		printf("hrc7000ClockTest: all passed\n");
	}
	return failures != 0;
}
