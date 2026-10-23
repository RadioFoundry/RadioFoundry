// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// An SPI master as the chip drivers see it (flash/spi_nor, flash/spi_nand): a chip select and a full-duplex byte
// transfer. The board wires it to its controller and pin; locking stays with the board.
#ifndef SPI_BUS_H
#define SPI_BUS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	void (*select)(bool active);	// true = chip select asserted (low on the wire)
	// Clock len bytes. tx NULL sends 0xFF, rx NULL discards what comes back
	bool (*xfer)(const uint8_t *tx, uint8_t *rx, uint32_t len);
} spi_bus_t;

#endif
