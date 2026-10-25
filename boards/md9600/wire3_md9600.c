// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A write-only three-wire bus bit-banged on GPIO (periph_md9600.h, drivers/bus/wire3.h), for chips on pins no SPI
// peripheral reaches. Per bit: clock low, data set, half a period, clock high (the chip takes the bit on the rising
// edge), half a period. The chip select goes low before the first bit and high after the last, which latches the
// word; clock and data are left low.
#include "periph_md9600.h"

static inline void wait_cycles(uint32_t cycles)
{
	uint32_t start = DWT->CYCCNT;
	while ((uint32_t)(DWT->CYCCNT - start) < cycles) {
	}
}

bool stm32_wire3_write(void *ctx, uint8_t cs, uint32_t word, uint8_t bits)
{
	const stm32_wire3_t *w = ctx;
	if (cs >= w->cs_count || bits == 0 || bits > 32) {
		return false;
	}
	uint32_t half = (SystemCoreClock / 1000000u) * w->half_period_ns / 1000u + 1u;
	stm32_pin_write(w->clk_port, w->clk_pin, false);
	stm32_pin_write(w->cs_port[cs], w->cs_pin[cs], false);
	wait_cycles(half);
	for (uint32_t m = 1u << (bits - 1u); m != 0; m >>= 1) {
		stm32_pin_write(w->clk_port, w->clk_pin, false);
		stm32_pin_write(w->data_port, w->data_pin, (word & m) != 0);
		wait_cycles(half);
		stm32_pin_write(w->clk_port, w->clk_pin, true);
		wait_cycles(half);
	}
	stm32_pin_write(w->clk_port, w->clk_pin, false);
	wait_cycles(half);
	stm32_pin_write(w->cs_port[cs], w->cs_pin[cs], true);
	stm32_pin_write(w->data_port, w->data_pin, false);
	wait_cycles(half);
	return true;
}
