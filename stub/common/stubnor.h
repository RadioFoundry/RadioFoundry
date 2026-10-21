// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The flash stubs' external SPI NOR flash, bus-independent: the common 25-series command set (JEDEC ID 0x9F, read 0x03
// with three address bytes, write enable 0x06, status 0x05, 4 KB sector erase 0x20, page program 0x02, release from
// power-down 0xAB), so parts up to 16 MB work. The target wires the bus (nor_hw_*: a SPI block on the STM32F4, GPIO
// pins on the K22F). The protocol engine (stubeng.c) has already checked ranges and erased bytes.
#ifndef STUBNOR_H
#define STUBNOR_H

#include <stdbool.h>
#include <stdint.h>

// Wake the chip on a bus the target has set up: its size in bytes (0 when no chip answers) and its JEDEC ID. Waits
// for an erase ODR may have left running
uint32_t stubnor_probe(uint8_t id[3]);
bool stubnor_read(uint32_t addr, uint8_t *dst, uint32_t n);
bool stubnor_erase(uint32_t addr);
bool stubnor_program(uint32_t addr, const uint8_t *data, uint32_t n);

// ---- the target's bus
// Chip select on (true) or off; off waits for the last byte to leave
void nor_hw_select(bool on);
// n bytes out (tx NULL: 0xFF) and in (rx NULL: dropped); false when the bus stalls
bool nor_hw_xfer(const uint8_t *tx, uint8_t *rx, uint32_t n);

#endif
