// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The GD32F30x flash stub's target facts (docs/spec/stub-gd32f30x.md): the flash map the protocol engine sees, PEEK's
// ranges, and the board record the host sends in CONFIG. The protocol engine is shared (stub/common/stubeng.h);
// gd32stub_main.c is the glue, gd32stub_board.c checks a board record (portable, host tested).
#ifndef GD32STUB_H
#define GD32STUB_H

#include <stdbool.h>
#include <stdint.h>
#include "gd32stub_header.h"
#include "stubeng.h"

#define GD32STUB_NAME		"GDSTUB"
#define GD32STUB_MAGIC		0x47445342u	// 'GDSB': ERASE and RESET carry it (big endian on the wire)

#define GD32_FLASH_START	0x08000000u
#define GD32_BANK0_END		0x08080000u	// bank 0 (2 KB pages) ends here; bank 1 (4 KB pages) follows on larger parts
// The engine's sectors are 4 KB everywhere: two 2 KB pages in bank 0, one page in bank 1. The ERASE request carries
// the sector as one byte, so the map stops at 256 sectors (1 MB); everything RadioFoundry and the vendor images
// place is below that (docs/spec/stub-gd32f30x.md 2)
#define GD32_SECTOR_SIZE	0x1000u
#define GD32_STUB_FLASH_MAX	(256u * GD32_SECTOR_SIZE)
// No board record opens sector 0 (the vector table the chip boots from) for writing; each board's bootloader end
// comes in its record
#define GD32_MIN_APP_BASE	0x08001000u

// The board record (CONFIG payload, 8 bytes, big endian). The stub drives no board pin (the D+ pull-up stays as
// RadioFoundry left it, section 4), so the record holds only what the engine needs. The host keeps one profile per radio
#define GD32_REC_LEN		8u
#define GD32_REC_LAYOUT		1u	// byte 1: this layout
enum {
	GD32_REC_BOARD		= 0,	// u8, nonzero: the host's board id, echoed by HELLO
	GD32_REC_FORMAT		= 1,	// u8, GD32_REC_LAYOUT
	GD32_REC_ZERO		= 2,	// u16 reserved, 0
	GD32_REC_APP_BASE	= 4,	// u32: the first writable flash address, the end of the bootloader
};

// HELLO flags (the target's bits)
#define GD32_HELLO_USB_ADOPTED	0x01u	// the stub took over RadioFoundry's configured USB device (no re-enumeration)

typedef struct {
	uint8_t board;
	uint32_t app_base;
} gd32_board_t;

// Check a board record and decode it. The application base is only decoded here; the engine checks it against the
// flash map
bool gd32_board_parse(const uint8_t *rec, uint32_t n, gd32_board_t *b);

extern const stubeng_run_t gd32_sector_runs[1];
extern const stubeng_range_t gd32_peek_ranges[3];
// The engine's flash description for a part with flash_kb of flash (cut to GD32_STUB_FLASH_MAX)
stubeng_flash_t gd32_flash(uint16_t flash_kb);

#endif
