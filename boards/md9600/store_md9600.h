// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 data flash: the map the core gets and the board's own record sector (docs/spec/hardware-md9600.md 12.2).
//
// The factory calibration and the hardware options are in the flash's security registers 1-3, outside the main
// array. Nothing in this firmware can write them: the SPI NOR driver has no security register erase or program and
// never writes a status register (so the registers' one-time lock bits stay as they are). The host test checks that
// no such command ever reaches the chip.
#ifndef STORE_MD9600_H
#define STORE_MD9600_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_flash.h"
#include "rfplan_md9600.h"

#define MD9600_STORE_FULL_SIZE		0x1000000u	// the map needs the 16 MB part
#define MD9600_STORE_SECTOR_SIZE	0x1000u
// The board record sector: in the hole between the core's journal (0x019000-0x01AFFF) and the open-firmware flash
// image (0x020000), which no CPS address maps to
#define MD9600_STORE_REV_SECTOR		0x01F000u

extern const hal_flash_map_t md9600_flash_map;

// The stored board revision. Records (rfplan_md9600.h, 8 bytes) are appended to the sector and the newest valid one
// counts, so a power cut while one is written leaves the one before it; the sector is erased only when it is full.
// MD9600_REV_UNKNOWN when there is none, the flash isn't usable or its map isn't this one
md9600_rev_t md9600_store_rev_load(const hal_flash_t *f);
// Store rev (skipped when it is already the stored one); true once it reads back
bool md9600_store_rev_save(const hal_flash_t *f, md9600_rev_t rev);

#endif
