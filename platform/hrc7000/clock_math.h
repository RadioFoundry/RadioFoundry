// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 clock arithmetic as pure functions, so the host tests can check what clock.c, uart.c and the boards derive
// from the PLL (docs/spec/hardware-dm32.md 1.1). No register access here.
#ifndef PLATFORM_HRC7000_CLOCK_MATH_H
#define PLATFORM_HRC7000_CLOCK_MATH_H

#include <stdint.h>

// CLK_MGR 0x010 (BPLL): bits 23:18 refdiv, 17:6 fbdiv, 5:3 postdiv1, 2:0 postdiv2. The bits above 23 are the PLL's
// power and mode flags and are kept
// always_inline: RAM code (the stub hand-over) calls these and must not branch into XIP flash
#define HRC_MATH static inline __attribute__((always_inline))

#define HRC_PLL_FIELDS		0x00FFFFFFu

HRC_MATH uint32_t hrc_pll_word(uint32_t old, uint32_t refdiv, uint32_t fbdiv, uint32_t post1, uint32_t post2)
{
	return (old & ~HRC_PLL_FIELDS) | (refdiv << 18) | (fbdiv << 6) | (post1 << 3) | post2;
}

// The BPLL output (the CPU clock with CLK_MGR 0x024 at 1:1) for a register word, 0 for a word with a zero divider
HRC_MATH uint32_t hrc_pll_out_hz(uint32_t word, uint32_t xtal_hz)
{
	uint32_t refdiv = (word >> 18) & 0x3Fu;
	uint32_t fbdiv = (word >> 6) & 0xFFFu;
	uint32_t post1 = (word >> 3) & 0x7u;
	uint32_t post2 = word & 0x7u;
	if (refdiv == 0 || post1 == 0 || post2 == 0) {
		return 0;
	}
	return (uint32_t)((uint64_t)xtal_hz * fbdiv / refdiv / post1 / post2);
}

// 16550-style divisor for a baud rate, rounded to nearest (the divisor is clock / (16 * baud))
HRC_MATH uint32_t hrc_uart_div(uint32_t apb_hz, uint32_t baud)
{
	return (apb_hz + 8u * baud) / (16u * baud);
}

// The baud rate a divisor really gives, for the tests and the docs
HRC_MATH uint32_t hrc_uart_baud(uint32_t apb_hz, uint32_t div)
{
	return div != 0 ? apb_hz / (16u * div) : 0;
}

// Back-to-back writes to an APB output that make each phase of a bit-banged line at least about 30 ns (a write takes at
// least one APB cycle): the ST7735S wants 15 ns minimum and a 66 ns cycle
HRC_MATH uint32_t hrc_bitbang_repeat(uint32_t apb_hz)
{
	return (apb_hz + 33333332u) / 33333333u;
}

#endif
