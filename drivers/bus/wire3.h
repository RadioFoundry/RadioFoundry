// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A write-only three-wire serial bus (clock, data, a chip select per device) as the synthesizer and IF receiver drivers
// see it (rf/pll1590, rf/lmx2571, rf/ak2365a). The board wires it to GPIO bit-banging or a peripheral and keeps any
// locking; a driver only builds words. Frames are MSB first and latched when the chip select goes high.
#ifndef WIRE3_H
#define WIRE3_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	// Shift out the low bits (1..32) of word with chip select cs asserted for the whole frame
	bool (*write)(void *ctx, uint8_t cs, uint32_t word, uint8_t bits);
	void *ctx;
} wire3_bus_t;

#endif
