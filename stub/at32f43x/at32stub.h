// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AT32F43x flash stub's target facts (docs/spec/stub-at32f43x.md): the flash map the protocol engine sees, the
// banks, PEEK's ranges, and the board record the host sends in CONFIG. The protocol engine is shared
// (stub/common/stubeng.h); at32stub_main.c is the glue, at32stub_board.c checks a board record (portable, host tested).
#ifndef AT32STUB_H
#define AT32STUB_H

#include <stdbool.h>
#include <stdint.h>
#include "at32stub_header.h"
#include "stubeng.h"

#define AT32STUB_NAME		"ATSTUB"
#define AT32STUB_MAGIC		0x41545342u	// 'ATSB': ERASE and RESET carry it (big endian on the wire)

#define AT32_FLASH_START	0x08000000u
// The engine's sectors are 4 KB everywhere, the larger of the two sector sizes the AT32F435/437 parts have (2 KB or
// 4 KB, by part: the flash driver erases what each 4 KB holds either way, at32stub_flash.c). The ERASE request carries
// the sector as one byte, so the map stops at 256 sectors (1 MB); everything RadioFoundry and the vendor images place
// is below that (docs/spec/stub-at32f43x.md 2)
#define AT32_SECTOR_SIZE	0x1000u
#define AT32_STUB_FLASH_MAX	(256u * AT32_SECTOR_SIZE)
// The physical erase unit the flash driver steps in: the smaller sector size, so a 4 KB engine sector is never more
// than two of them
#define AT32_ERASE_STEP		0x800u
// No board record opens sector 0 (the vector table the chip boots from) for writing; each board's bootloader end
// comes in its record
#define AT32_MIN_APP_BASE	0x08001000u
// Where bank 2 starts: on parts over 1 MB bank 1 is the first 2 MB (the vendor flash library in a stock image tests
// these limits, docs/spec/stub-at32f43x.md 2); the 1 MB parts split into two 512 KB banks (reference manual); smaller
// parts have one bank
#define AT32_BANK2_LARGE	0x08200000u
#define AT32_BANK2_1MB		0x08080000u

// The board record (CONFIG payload, 8 bytes, big endian): the GD32F30x stub's layout. The stub drives no board pin,
// so the record holds only what the engine needs. The host keeps one profile per radio
#define AT32_REC_LEN		8u
#define AT32_REC_LAYOUT		1u	// byte 1: this layout
enum {
	AT32_REC_BOARD		= 0,	// u8, nonzero: the host's board id, echoed by HELLO
	AT32_REC_FORMAT		= 1,	// u8, AT32_REC_LAYOUT
	AT32_REC_ZERO		= 2,	// u16 reserved, 0
	AT32_REC_APP_BASE	= 4,	// u32: the first writable flash address, the end of the bootloader
};

// HELLO flags (the target's bits)
#define AT32_HELLO_USB_ADOPTED	0x01u	// the stub took over RadioFoundry's configured USB device (no re-enumeration)

typedef struct {
	uint8_t board;
	uint32_t app_base;
} at32_board_t;

// Check a board record and decode it. The application base is only decoded here; the engine checks it against the
// flash map
bool at32_board_parse(const uint8_t *rec, uint32_t n, at32_board_t *b);

extern const stubeng_run_t at32_sector_runs[1];
extern const stubeng_range_t at32_peek_ranges[3];
// The engine's flash description for a part with flash_kb of flash (cut to AT32_STUB_FLASH_MAX)
stubeng_flash_t at32_flash(uint16_t flash_kb);
// The first address of bank 2 on a part with flash_kb of flash, 0 for a part with one bank
uint32_t at32_bank2_start(uint16_t flash_kb);

#endif
