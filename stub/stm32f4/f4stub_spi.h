// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The external SPI NOR flash's bus for the flash stub (f4stub_spi.c), wired as the board record says. Reads, erases
// and programs go through the shared command layer (stub/common/stubnor.h).
#ifndef F4STUB_SPI_H
#define F4STUB_SPI_H

#include <stdbool.h>
#include <stdint.h>
#include "f4stub.h"

// Set up the bus from the board record (pclk_hz: the bus's APB clock) and wake the chip: its size in bytes (0 when no
// chip answers) and its JEDEC ID
uint32_t f4spi_init(const f4_board_t *b, uint32_t pclk_hz, uint8_t id[3]);

#endif
