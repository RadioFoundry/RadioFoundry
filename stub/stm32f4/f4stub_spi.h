// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The external SPI NOR flash for the flash stub (f4stub_spi.c), wired as the board record says. The protocol engine
// (stub/common/stubeng.c) has already checked ranges and erased bytes; these functions only drive the chip.
#ifndef F4STUB_SPI_H
#define F4STUB_SPI_H

#include <stdbool.h>
#include <stdint.h>
#include "f4stub.h"

// Set up the bus from the board record (pclk_hz: the bus's APB clock) and wake the chip: its size in bytes (0 when no
// chip answers) and its JEDEC ID
uint32_t f4spi_init(const f4_board_t *b, uint32_t pclk_hz, uint8_t id[3]);
bool f4spi_read(uint32_t addr, uint8_t *dst, uint32_t n);
bool f4spi_erase(uint32_t addr);
bool f4spi_program(uint32_t addr, const uint8_t *data, uint32_t n);
// reg 0: status register 2 into dst[0]. reg 1-3: the security register's 256 bytes into dst (src NULL), or erase it
// and program src's 256 bytes. The engine has checked the chip's maker, the lock bit and the magic
bool f4spi_sreg(unsigned reg, uint8_t *dst, const uint8_t *src);

#endif
