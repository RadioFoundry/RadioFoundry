// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The STM32F4 flash stub's target facts (docs/spec/stub-stm32f4.md): what every STM32F405 has (the flash map, the
// readable memory ranges) and the board record the host sends in CONFIG. The protocol engine is shared
// (stub/common/stubeng.h); f4stub_main.c is the glue, f4stub_board.c checks a board record (portable, host tested).
#ifndef F4STUB_H
#define F4STUB_H

#include <stdbool.h>
#include <stdint.h>
#include "f4stub_header.h"	// F4STUB_VERSION and the image header

#define F4STUB_BLOCK		4096u			// the unit of SUMS and READ (CRC per block, not per erase sector)
#define F4STUB_MAX_PAYLOAD	(F4STUB_BLOCK + 8u)	// a PROGRAM of one block (address + 4096) or a READ record
#define F4STUB_FRAME_HDR	6u			// 02 op/status len_hi len_lo sum 03
#define F4STUB_MAGIC		0x46345342u		// 'F4SB' (big endian on the wire, as every field here)

#define F4_FLASH_BASE		0x08000000u
// No board record opens sector 0 (the vector table the chip boots from) for writing. Each board's own bootloader end
// comes in its record; on every radio so far that is sector 3 (0x0800C000)
#define F4_MIN_APP_BASE		0x08004000u

// HELLO flags (the target's bits)
#define F4_HELLO_USB_ADOPTED	0x01u	// the stub took over ODR's configured USB device (no re-enumeration)

typedef struct {
	void *ctx;
	// Send bytes to the host (a whole frame per call)
	void (*write)(void *ctx, const uint8_t *buf, uint32_t n);
	// Copy n bytes from a memory address the engine has already checked
	void (*read)(void *ctx, uint32_t addr, void *dst, uint32_t n);
	// Erase one flash sector; false on a controller error
	bool (*erase)(void *ctx, unsigned sector);
	// Program n words at addr (word aligned, already checked blank); false on a controller error
	bool (*program)(void *ctx, uint32_t addr, const uint32_t *words, uint32_t n);
	// Called after the RESET reply has been sent; does not return on the target
	void (*reset)(void *ctx);
	// The external SPI flash (NULL where there is none): read, erase one 4 KB sector, program bytes inside one sector
	// (the engine has checked they read erased); false on a chip that stays busy or doesn't answer
	bool (*xread)(void *ctx, uint32_t addr, void *dst, uint32_t n);
	bool (*xerase)(void *ctx, uint32_t addr);
	bool (*xprogram)(void *ctx, uint32_t addr, const uint8_t *data, uint32_t n);
	// Optional (NULL: none): feed the watchdog. The engine calls it at every request, before each erase or program, and
	// once per 4 KB block in SUMS and READ, so no request runs a second without a feed (docs/spec/watchdog.md 3.1)
	void (*feed)(void *ctx);
	uint32_t xsize;		// SPI flash bytes (0: no chip answered)
	uint8_t xid[3];		// its JEDEC ID
	uint16_t flash_kb;	// the flash size register
	uint8_t rdp;		// FLASH_OPTCR bits 15:8
	uint8_t flags;		// F4_HELLO_*
} f4stub_port_t;

typedef struct {
	uint8_t board;
	uint8_t spi_bus;	// 0: none
	uint8_t spi_mode;
	uint8_t af;
	uint32_t hse_hz;
	uint32_t app_base;
	uint32_t spi_hz;
	uint8_t sck, miso, mosi, cs;
	uint8_t idle[2];
} f4_board_t;

// Check a board record and decode it. The application base is only decoded here; the engine checks it against the
// flash map
bool f4_board_parse(const uint8_t *rec, uint32_t n, f4_board_t *b);

// The STM32F405/407 sector map (16, 16, 16, 16, 64, then 128 KB each) and PEEK's ranges besides the flash
extern const stubeng_run_t f4_sector_runs[3];
extern const stubeng_range_t f4_peek_ranges[4];
// The engine's flash description for a part with flash_kb of flash
stubeng_flash_t f4_flash(uint16_t flash_kb);

#endif
