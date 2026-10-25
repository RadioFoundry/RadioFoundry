// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Generic 24-series I2C EEPROM (i2c_eeprom.h): the protocol of the Microchip 24LC512 / 24AA512 data sheet. A write is
// the device address, two memory address bytes (high first) and up to one page of data; bytes past the page end would
// wrap to the start of the same page, so the driver never sends them. During the write cycle the device doesn't
// acknowledge its address.
#include <string.h>
#include "i2c_eeprom.h"
#include "hal_time.h"

static bool valid(const i2c_eeprom_t *e, uint32_t addr, uint32_t len)
{
	return e->size != 0 && e->size <= 0x10000u && addr <= e->size && len <= e->size - addr;
}

bool i2c_eeprom_probe(const i2c_eeprom_t *e)
{
	static const uint8_t zero[2] = { 0, 0 };
	return e->bus->write(e->bus->ctx, e->addr7, zero, sizeof(zero));
}

bool i2c_eeprom_read(const i2c_eeprom_t *e, uint32_t addr, void *buf, uint32_t len)
{
	uint8_t *p = buf;
	if (!valid(e, addr, len)) {
		return false;
	}
	while (len > 0) {
		uint32_t n = e->read_chunk != 0 && len > e->read_chunk ? e->read_chunk : len;
		uint8_t a[2] = { (uint8_t)(addr >> 8), (uint8_t)addr };
		if (!e->bus->write_read(e->bus->ctx, e->addr7, a, sizeof(a), p, n)) {
			return false;
		}
		addr += n;
		p += n;
		len -= n;
	}
	return true;
}

// The write cycle after a page: the device answers its address again when it is done
static bool wait_written(const i2c_eeprom_t *e)
{
	if (!e->ack_poll) {
		hal_delay_ms(e->write_ms);
		return true;
	}
	for (uint32_t i = 0; i < 2u * e->write_ms; i++) {
		hal_delay_ms(1);
		if (e->bus->write(e->bus->ctx, e->addr7, NULL, 0)) {
			return true;
		}
	}
	return false;
}

bool i2c_eeprom_write(const i2c_eeprom_t *e, uint32_t addr, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;
	uint8_t frame[2 + I2C_EEPROM_PAGE_MAX];
	if (!valid(e, addr, len) || e->page == 0 || e->page > I2C_EEPROM_PAGE_MAX || (e->page & (e->page - 1u)) != 0) {
		return false;
	}
	while (len > 0) {
		uint32_t room = e->page - (addr & (e->page - 1u));
		uint32_t n = len < room ? len : room;
		frame[0] = (uint8_t)(addr >> 8);
		frame[1] = (uint8_t)addr;
		memcpy(&frame[2], p, n);
		if (!e->bus->write(e->bus->ctx, e->addr7, frame, 2u + n) || !wait_written(e)) {
			return false;
		}
		addr += n;
		p += n;
		len -= n;
	}
	return true;
}
