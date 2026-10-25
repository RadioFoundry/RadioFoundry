// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Generic I2C serial EEPROM of the 24-series class with a 16-bit memory address (24C32 to 24C512): random and
// sequential reads, page writes split at page boundaries, then the internal write cycle waited out by a fixed delay or
// by acknowledge polling. The board supplies the bus (bus/i2c_bus.h) and any locking.
#ifndef I2C_EEPROM_H
#define I2C_EEPROM_H

#include <stdint.h>
#include <stdbool.h>
#include "bus/i2c_bus.h"

#define I2C_EEPROM_PAGE_MAX	128u	// the largest page the driver handles (24C512: 128, many parts 64)

typedef struct {
	const i2c_bus_t *bus;
	uint8_t addr7;		// 7-bit device address (0x50 with A2..A0 low)
	uint32_t size;		// bytes, at most 64 KB (a 16-bit memory address)
	uint16_t page;		// write page in bytes, a power of two up to I2C_EEPROM_PAGE_MAX
	uint8_t write_ms;	// the data sheet's maximum write cycle time (5 ms for most parts)
	// Poll the device address after a page write until it acknowledges (it doesn't during the write cycle), at most
	// 2 x write_ms polls 1 ms apart. False: wait write_ms once
	bool ack_poll;
	// Longest read in one bus transaction (0: no limit). A shared bus is held for one transaction at a time, so a
	// limit keeps a long read from blocking the bus's other chips
	uint16_t read_chunk;
} i2c_eeprom_t;

// The device acknowledges its address (sets the address pointer to 0, writes nothing)
bool i2c_eeprom_probe(const i2c_eeprom_t *e);
// Read len bytes from addr. False if the range runs past size or the bus fails
bool i2c_eeprom_read(const i2c_eeprom_t *e, uint32_t addr, void *buf, uint32_t len);
// Write len bytes at addr, any alignment: one page write per page touched, each waited out. False if the range runs
// past size, the bus fails or the write cycle doesn't end in time; the pages before the failure are written
bool i2c_eeprom_write(const i2c_eeprom_t *e, uint32_t addr, const void *buf, uint32_t len);

#endif
