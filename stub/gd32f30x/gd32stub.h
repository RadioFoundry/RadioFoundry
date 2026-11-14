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
// The engine's sectors are 4 KB everywhere: two 2 KB pages in bank 0, one page in bank 1. The map covers the part's
// flash, up to the family's largest (3 MB, 768 sectors); ERASE carries a sector past 255 in two bytes
// (docs/spec/stub-gd32f30x.md 2)
#define GD32_SECTOR_SIZE	0x1000u
#define GD32_STUB_SECTORS	768u
#define GD32_STUB_FLASH_MAX	(GD32_STUB_SECTORS * GD32_SECTOR_SIZE)
// No board record opens sector 0 (the vector table the chip boots from) for writing; each board's bootloader end
// comes in its record
#define GD32_MIN_APP_BASE	0x08001000u

// The board record (CONFIG payload, big endian). Layout 1 (8 bytes) holds only what the engine needs; layout 2
// (16 bytes) adds a bit-banged SPI NAND the stub reads as the external memory (docs/spec/stub-gd32f30x.md 5). The
// stub drives no other board pin (the D+ pull-up stays as RadioFoundry left it, section 4). The host keeps one profile
// per radio
#define GD32_REC_LEN		8u
#define GD32_REC_LAYOUT		1u	// byte 1: this layout
#define GD32_REC_LEN_NAND	16u
#define GD32_REC_LAYOUT_NAND	2u
enum {
	GD32_REC_BOARD		= 0,	// u8, nonzero: the host's board id, echoed by HELLO
	GD32_REC_FORMAT		= 1,	// u8, GD32_REC_LAYOUT or GD32_REC_LAYOUT_NAND
	GD32_REC_ZERO		= 2,	// u16 reserved, 0
	GD32_REC_APP_BASE	= 4,	// u32: the first writable flash address, the end of the bootloader
	// Layout 2: the NAND's pins, each port << 4 | pin (port 0 = A): chip select, clock, data out (IO0), data in (IO1)
	GD32_REC_NAND_CS	= 8,
	GD32_REC_NAND_CLK	= 9,
	GD32_REC_NAND_IO0	= 10,
	GD32_REC_NAND_IO1	= 11,
	GD32_REC_NAND_FLAGS	= 12,	// u8: GD32_NAND_DUAL
	GD32_REC_NAND_ZERO	= 13,	// u8 reserved, 0
	GD32_REC_NAND_MB	= 14,	// u16: the chip's size in MB (main areas), 1 to 2048
};
#define GD32_NAND_DUAL		0x01u	// read in dual SPI (0x3B): IO0 turns input for the data
#define GD32_NAND_PAGE		2048u	// main area bytes per page
#define GD32_NAND_MB_MAX	2048u

// HELLO flags (the target's bits)
#define GD32_HELLO_USB_ADOPTED	0x01u	// the stub took over RadioFoundry's configured USB device (no re-enumeration)

typedef struct {
	uint8_t board;
	uint32_t app_base;
	bool nand;			// layout 2
	uint8_t nand_pin[4];		// CS, CLK, IO0, IO1 (port << 4 | pin)
	uint8_t nand_flags;
	uint16_t nand_mb;
} gd32_board_t;

// The SPI NAND (gd32stub_nand.c, target only): set the pins up and read the ID; the main areas from addr, linear
bool gd32nand_init(const gd32_board_t *b);
bool gd32nand_read(uint32_t addr, void *dst, uint32_t n);

// Check a board record and decode it. The application base is only decoded here; the engine checks it against the
// flash map
bool gd32_board_parse(const uint8_t *rec, uint32_t n, gd32_board_t *b);

extern const stubeng_run_t gd32_sector_runs[1];
extern const stubeng_range_t gd32_peek_ranges[3];
// The engine's flash description for a part with flash_kb of flash (cut to GD32_STUB_FLASH_MAX)
stubeng_flash_t gd32_flash(uint16_t flash_kb);

#endif
