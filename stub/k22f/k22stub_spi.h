// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The external SPI NOR flash's bus for the K22F stub (k22stub_spi.c), wired as the board record says. Reads, erases and
// programs go through the shared command layer (stub/common/stubnor.h).
#ifndef K22STUB_SPI_H
#define K22STUB_SPI_H

#include <stdint.h>
#include "k22stub.h"

// Set up the pins from the board record and wake the chip: its size in bytes (0 when no chip answers) and its JEDEC ID
uint32_t k22spi_init(const k22_board_t *b, uint8_t id[3]);

#endif
