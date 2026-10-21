// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The Kinetis K22F flash stub's target facts (docs/spec/stub-k22f.md): what every MK22FN512 has (the flash map, the
// readable memory ranges) and the board record the host sends in CONFIG. The protocol engine is shared
// (stub/common/stubeng.h); k22stub_main.c is the glue, k22stub_board.c checks a board record (portable, host tested).
#ifndef K22STUB_H
#define K22STUB_H

#include <stdbool.h>
#include <stdint.h>
#include "stubeng.h"

#define K22STUB_VERSION		3u		// the shared engine's protocol: CONFIG, the board record, HELLO's echo
#define K22STUB_NAME		"K2STUB"
#define K22STUB_MAGIC		0x4B325342u	// 'K2SB' (big endian on the wire, as every field here)

#define K22_FLASH_BASE		0x00000000u
#define K22_SECTOR		0x800u		// 2 KB erase sector
// The program unit on the wire. The FTFA programs longwords; the protocol's unit is 8 bytes, which keeps the commit
// (the first unit at the application base, written last) whole on a part that programs phrases
#define K22_PROG_UNIT		8u
// Sector 0 holds the vector table the chip boots from and the flash configuration field (0x400..0x40F: the security
// byte and the protection bits). No board record opens it for writing; each board's own loader end comes in its record
#define K22_MIN_APP_BASE	0x00000800u

// What ODR passes at the jump (r0 flags, r1 USB data toggles, r2 the core clock) and where the stub runs:
// platform/k22f/stub_handoff.h

// HELLO flags (the target's bits)
#define K22_HELLO_USB_ADOPTED	0x01u	// the stub took over ODR's configured USB device (no re-enumeration)

// The board record (CONFIG payload, 24 bytes, big endian). Pins are port * 32 + pin (port 0 = A ... 4 = E, as
// platform/k22f/k22f_pin.h), 0xFF for none. The host keeps one profile per radio (stub/k22stub.py BOARDS)
#define K22_REC_LEN		24u
#define K22_REC_LAYOUT		1u	// byte 1: this layout
#define K22_PIN_NONE		0xFFu
enum {
	K22_REC_BOARD	= 0,	// u8, nonzero: the host's board id, echoed by HELLO
	K22_REC_FORMAT	= 1,	// u8, K22_REC_LAYOUT
	K22_REC_XFLASH	= 2,	// u8: 0 no SPI flash, 1 SPI NOR on GPIO pins (mode 0, bit-banged)
	K22_REC_CS_MODE	= 3,	// u8: 0 the chip select idles driven high, 1 released to its pull-up (input)
	K22_REC_APP_BASE = 4,	// u32: the first writable flash address, the end of the radio's loader
	K22_REC_APP_END	= 8,	// u32: the first address above the writable flash (radio data above stays)
	K22_REC_XFLOOR	= 12,	// u32: the SPI flash below this is the radio's own and never written (4 KB multiple)
	K22_REC_SCK	= 16,	// pins: SCK, MOSI, MISO, the chip select
	K22_REC_MOSI	= 17,
	K22_REC_MISO	= 18,
	K22_REC_CS	= 19,
	K22_REC_ZERO	= 20,	// 4 bytes reserved, 0
};

typedef struct {
	uint8_t board;
	uint8_t xflash;		// 0: none
	bool cs_released;
	uint32_t app_base, app_end, xfloor;
	uint8_t sck, mosi, miso, cs;
} k22_board_t;

// Check a board record and decode it. The application range is only decoded here; the engine checks it against the
// flash map
bool k22_board_parse(const uint8_t *rec, uint32_t n, k22_board_t *b);

// The MK22FN512 sector map (2 KB each) and PEEK's ranges besides the flash
extern const stubeng_run_t k22_sector_runs[1];
extern const stubeng_range_t k22_peek_ranges[3];
// The engine's flash description for a part with flash_kb of program flash
stubeng_flash_t k22_flash(uint16_t flash_kb);
// SIM_FCFG1's PFSIZE code in KB; 0 for a code this family doesn't use
uint16_t k22_flash_kb(uint32_t pfsize);

#endif
