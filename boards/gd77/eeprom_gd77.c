// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 EEPROM: a 24C512-class part at 0x50 on I2C bus A, shared with the AT1846S (docs/spec/hardware-gd77.md 6.3).
// ODR only reads it: the factory calibration (6.4) and the radio's own logical codeplug space. Reads go in 64-byte
// transactions so the AT1846S never waits long for the bus.
#include "gd77.h"
#include "storage_gd77.h"
#include "cal_blocks_gd77.h"
#include "flash/i2c_eeprom/i2c_eeprom.h"

static bool ee_bus_write(void *ctx, uint8_t addr7, const uint8_t *data, size_t n)
{
	(void)ctx;
	return gd77_i2c_write(addr7, data, (uint32_t)n);
}

static bool ee_bus_write_read(void *ctx, uint8_t addr7, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
	(void)ctx;
	return gd77_i2c_write_read(addr7, w, (uint32_t)wn, r, (uint32_t)rn);
}

static const i2c_bus_t bus = { .write = ee_bus_write, .write_read = ee_bus_write_read, .ctx = NULL };

static const i2c_eeprom_t eeprom = {
	.bus = &bus,
	.addr7 = GD77_EEPROM_ADDR7,
	.size = GD77_EEPROM_SIZE,
	.page = GD77_EEPROM_PAGE,
	.write_ms = GD77_EEPROM_WRITE_MS,
	.ack_poll = true,
	.read_chunk = 64,
};

static bool present;

bool gd77_eeprom_read(uint32_t addr, void *buf, uint32_t len)
{
	return i2c_eeprom_read(&eeprom, addr, buf, len);
}

bool gd77_eeprom_present(void)
{
	return present;
}

static bool cal_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
	(void)ctx;
	return gd77_eeprom_read(addr, buf, len);
}

bool gd77_logical_read(uint32_t logical, void *buf, uint32_t len)
{
	uint8_t *p = buf;
	gd77_logical_piece_t piece;
	while (len > 0) {
		if (!gd77_logical_piece(logical, len, &piece)) {
			return false;
		}
		bool ok = piece.eeprom ? gd77_eeprom_read(piece.addr, p, piece.len) :
					 gd77_flash_raw_read(piece.addr, p, piece.len);
		if (!ok) {
			return false;
		}
		logical += piece.len;
		p += piece.len;
		len -= piece.len;
	}
	return true;
}

// After the bus lock exists; before the radio is set up, so it can apply the calibration from the start
void gd77_eeprom_init(void)
{
	present = i2c_eeprom_probe(&eeprom);
	gd77_cal_load(cal_read, NULL);
}
