// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The external SPI NOR flash for the flash stub (f4stub_spi.c). The protocol engine (f4stub_proto.c) has already
// checked ranges and erased bytes; these functions only drive the chip.
#ifndef F4STUB_SPI_H
#define F4STUB_SPI_H

#include <stdbool.h>
#include <stdint.h>

// Set up the bus and wake the chip: its size in bytes (0 when no chip answers) and its JEDEC ID
uint32_t f4spi_init(uint32_t pclk2_hz, uint8_t id[3]);
bool f4spi_read(uint32_t addr, uint8_t *dst, uint32_t n);
bool f4spi_erase(uint32_t addr);
bool f4spi_program(uint32_t addr, const uint8_t *data, uint32_t n);

#endif
