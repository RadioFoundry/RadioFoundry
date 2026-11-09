// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 data flash layout and the board's own record sector (store_md9600.h, docs/spec/hardware-md9600.md 12.2).
// Pure: it works through the hal_flash_t operations, so tests/host/spiNorTest.c runs it on the SPI NOR model with
// power cuts.
#include <stddef.h>
#include "store_md9600.h"

// The map of the other usb-dfu radios with this flash layout: the open-firmware codeplug where the CPS tools expect it; settings and journal at
// 0x011000-0x01AFFF. 0x010000-0x010FFF stays free (no calibration copy: the calibration is in the security
// registers, which the core never sees). The stock codeplug at 0 is overwritten by the first open-firmware write
const hal_flash_map_t md9600_flash_map = {
	.settings_addr = 0x011000u,
	.settings_size = 0x008000u,	// 8 sectors
	.journal_addr = 0x019000u,	// 2 sectors
	.openfw_eeprom = 0x000000u,
	.openfw_flash = 0x020000u,
	.openfw_end = 0xE00000u,
	.calibration_addr = 0,
	.calibration_size = 0,
	.native_addr = 0xE00000u,
	.native_size = 0x100000u,
	.staging_addr = 0xF00000u,	// 256 KB of the unused top megabyte (docs/spec/tftp.md 4)
	.staging_size = 0x040000u,
};

#define SLOTS		(MD9600_STORE_SECTOR_SIZE / MD9600_REV_RECORD_SIZE)
#define CHUNK_SLOTS	8u

static bool slot_free(const uint8_t *s)
{
	for (uint32_t i = 0; i < MD9600_REV_RECORD_SIZE; i++) {
		if (s[i] != 0xFFu) {
			return false;
		}
	}
	return true;
}

// The newest valid record and the first free slot (SLOTS when the sector is full). False when the flash can't be read
static bool scan(const hal_flash_t *f, md9600_rev_t *rev, uint32_t *free_slot)
{
	uint8_t buf[CHUNK_SLOTS * MD9600_REV_RECORD_SIZE];
	*rev = MD9600_REV_UNKNOWN;
	*free_slot = SLOTS;
	for (uint32_t s = 0; s < SLOTS; s += CHUNK_SLOTS) {
		if (!f->read(MD9600_STORE_REV_SECTOR + s * MD9600_REV_RECORD_SIZE, buf, sizeof buf)) {
			return false;
		}
		for (uint32_t k = 0; k < CHUNK_SLOTS; k++) {
			const uint8_t *p = buf + k * MD9600_REV_RECORD_SIZE;
			if (slot_free(p)) {
				*free_slot = s + k;
				return true;	// records are appended: nothing past the first free slot
			}
			md9600_rev_t r = md9600_rev_record_decode(p);
			if (r != MD9600_REV_UNKNOWN) {
				*rev = r;	// a torn slot (cut while it was programmed) decodes as unknown and is passed over
			}
		}
	}
	return true;
}

static bool usable(const hal_flash_t *f)
{
	return f != NULL && f->map == &md9600_flash_map && f->size >= MD9600_STORE_FULL_SIZE;
}

md9600_rev_t md9600_store_rev_load(const hal_flash_t *f)
{
	md9600_rev_t rev;
	uint32_t free_slot;
	if (!usable(f) || !scan(f, &rev, &free_slot)) {
		return MD9600_REV_UNKNOWN;
	}
	return rev;
}

bool md9600_store_rev_save(const hal_flash_t *f, md9600_rev_t rev)
{
	md9600_rev_t cur;
	uint32_t slot;
	uint8_t rec[MD9600_REV_RECORD_SIZE];
	if (!usable(f) || md9600_rev_desc(rev) == NULL || !scan(f, &cur, &slot)) {
		return false;
	}
	if (cur == rev) {
		return true;
	}
	if (slot == SLOTS) {
		// Full (some 500 changes): start over. A cut during this erase loses the record, and the radio falls back
		// to the probe or the build default with transmit refused, as on a new radio
		if (!f->erase(MD9600_STORE_REV_SECTOR)) {
			return false;
		}
		slot = 0;
	}
	md9600_rev_record_encode(rev, rec);
	return f->write(MD9600_STORE_REV_SECTOR + slot * MD9600_REV_RECORD_SIZE, rec, sizeof rec) &&
	       md9600_store_rev_load(f) == rev;
}
