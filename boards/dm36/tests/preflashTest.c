// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "fuzz.h"
#include "input/quadrature/quadrature.h"
#include "../pins.h"

static uint32_t mux, ddr[3], levels[3], outs[3], ins[3];
#define IO_DIPLEX1 mux
#define GPIOA_BASE 0
#define GPIOB_BASE 1
#define GPIOC_BASE 2
#define GPIO_DDR 0
#define REG32(a) ddr[a]
static void hrc_pin_out(uint8_t pin, bool on)
{
	uint32_t bit = 1u << (pin % 32);
	outs[pin / 32] |= bit;
	ddr[pin / 32] |= bit;
	levels[pin / 32] = (levels[pin / 32] & ~bit) | (on ? bit : 0);
}
static void hrc_pin_in(uint8_t pin)
{
	ins[pin / 32] |= 1u << (pin % 32);
	ddr[pin / 32] &= ~(1u << (pin % 32));
}
#include BOARD_IO_HEADER
#include BOARD_VOLUME_HEADER

int main(void)
{
	fuzz_t f;
	fuzz_init(&f, 0x3621);
	for (long n = 0; n < fuzz_iters(1000); n++) {
		mux = fuzz_next(&f);
		for (unsigned i = 0; i < 3; i++) {
			ddr[i] = fuzz_next(&f);
			levels[i] = fuzz_next(&f);
			outs[i] = ins[i] = 0;
		}
		board_io_init();
		assert(mux == 0xFFEFFE07u);
		assert(ddr[0] == 0x270604E0u && ddr[1] == 0x23A0D01Fu && ddr[2] == 0x00047FC4u);
		assert(outs[0] == 0x26000000u && outs[1] == 0x23000000u && outs[2] == 0x40000u);
		assert((levels[0] & outs[0]) == 0x20000000u);
		assert((levels[1] & outs[1]) == 0x22000000u);
		assert((levels[2] & outs[2]) == 0);
		assert(ins[0] == 0 && ins[1] == 0x00400220u && ins[2] == 0);
	}
	assert(board_volume_fm(0) == 0xB281 && board_volume_fm(1) == 0xB281);
	assert(board_volume_fm(RADIO_VOLUME_MAX) == 0xB28F && board_volume_fm(255) == 0xB28F);
	for (unsigned step = 2; step <= RADIO_VOLUME_MAX; step++) {
		assert(board_volume_fm(step) >= board_volume_fm(step - 1));	// every step up is at least as loud
	}
	quad_dec_t q;
	quad_init(&q, 2, QUAD_REST_EQUAL, QUAD_AB(0, 0));
	assert(quad_sample(&q, QUAD_AB(0, 1)) == 0); // B leads A
	assert(quad_sample(&q, QUAD_AB(1, 1)) == 1);
	assert(quad_sample(&q, QUAD_AB(0, 1)) == 0); // A leads B
	assert(quad_sample(&q, QUAD_AB(0, 0)) == -1);
	printf("preflashTest: %ld seeded pin states, the volume steps and encoder directions passed\n", fuzz_iters(1000));
	return 0;
}
