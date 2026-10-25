// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// An I2C master as a board's chip glue sees it: whole transactions to a 7-bit address. The software master
// (bus/i2c_gpio) and an SoC's I2C controller both fit behind it, so a board can move a bus from one to the other
// without touching the code that talks to the chips. Locking stays with the board.
#ifndef I2C_BUS_H
#define I2C_BUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	// Write n bytes. True when the address and every byte were acknowledged
	bool (*write)(void *ctx, uint8_t addr7, const uint8_t *data, size_t n);
	// Write wn bytes, repeated START, read rn bytes. True when the writes were acknowledged
	bool (*write_read)(void *ctx, uint8_t addr7, const uint8_t *w, size_t wn, uint8_t *r, size_t rn);
	void *ctx;
} i2c_bus_t;

#endif
