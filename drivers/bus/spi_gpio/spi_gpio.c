// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Bit-banged SPI master (spi_gpio.h)
#include <stddef.h>
#include "spi_gpio.h"

static inline void half(const spi_gpio_t *b)
{
	if (b->delay != NULL) {
		b->delay();
	}
}

static inline bool sample(const spi_gpio_t *b)
{
	return b->miso != NULL && b->miso();
}

void spi_gpio_idle(const spi_gpio_t *b)
{
	b->sck(b->cpol);
}

uint8_t spi_gpio_byte(const spi_gpio_t *b, uint8_t out)
{
	uint8_t in = 0;
	for (uint8_t m = 0x80u; m != 0; m >>= 1) {
		bool bit = (out & m) != 0;
		bool got;
		if (!b->cpha) {
			b->mosi(bit);
			half(b);
			b->sck(!b->cpol);	// leading edge: sample
			got = sample(b);
			half(b);
			b->sck(b->cpol);
		} else {
			b->sck(!b->cpol);	// leading edge: shift out
			b->mosi(bit);
			half(b);
			b->sck(b->cpol);	// trailing edge: sample
			got = sample(b);
			half(b);
		}
		if (got) {
			in |= m;
		}
	}
	return in;
}

void spi_gpio_xfer(const spi_gpio_t *b, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		uint8_t v = spi_gpio_byte(b, tx != NULL ? tx[i] : 0xFFu);
		if (rx != NULL) {
			rx[i] = v;
		}
	}
}
