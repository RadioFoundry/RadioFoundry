// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32F4 flash stub's target facts (docs/spec/stub-stm32f4.md): what every STM32F405 has (the flash map, the
// readable memory ranges) and the board record the host sends in CONFIG. The protocol engine is shared
// (stub/common/stubeng.h); f4stub_main.c is the glue, f4stub_board.c checks a board record (portable, host tested).
#ifndef F4STUB_H
#define F4STUB_H

#include <stdbool.h>
#include <stdint.h>
#include "f4stub_header.h"	// F4STUB_VERSION and the image header
#include "stubeng.h"

#define F4STUB_NAME		"F4STUB"
#define F4STUB_MAGIC		0x46345342u	// 'F4SB' (big endian on the wire, as every field here)

#define F4_FLASH_BASE		0x08000000u
// No board record opens sector 0 (the vector table the chip boots from) for writing. Each board's own bootloader end
// comes in its record; on every radio so far that is sector 3 (0x0800C000)
#define F4_MIN_APP_BASE		0x08004000u

// HELLO flags (the target's bits)
#define F4_HELLO_USB_ADOPTED	0x01u	// the stub took over RadioFoundry's configured USB device (no re-enumeration)

// The board record (CONFIG payload, 24 bytes, big endian). Pins are port << 4 | pin (port 0 = A ... 8 = I), 0xFF for
// none. The host keeps one profile per radio (stub/f4stub.py BOARDS)
#define F4_REC_LEN		24u
#define F4_REC_LAYOUT		1u	// byte 1: this layout
#define F4_PIN_NONE		0xFFu
enum {
	F4_REC_BOARD	= 0,	// u8, nonzero: the host's board id, echoed by HELLO
	F4_REC_FORMAT	= 1,	// u8, F4_REC_LAYOUT
	F4_REC_SPI_BUS	= 2,	// u8: 0 no SPI flash, 1-3 SPI1-SPI3
	F4_REC_SPI_MODE	= 3,	// u8: CPOL << 1 | CPHA
	F4_REC_HSE_HZ	= 4,	// u32: the crystal (4-26 MHz), for the clock tree the stub reads back
	F4_REC_APP_BASE	= 8,	// u32: the first writable flash address, the end of the bootloader
	F4_REC_SPI_HZ	= 12,	// u32: the fastest SPI clock to use (the stub takes the next slower prescaler)
	F4_REC_SCK	= 16,	// pins: SCK, MISO, MOSI
	F4_REC_MISO	= 17,
	F4_REC_MOSI	= 18,
	F4_REC_AF	= 19,	// u8: their alternate function number
	F4_REC_CS	= 20,	// pin: the flash's chip select (driven by the stub)
	F4_REC_IDLE0	= 21,	// pins: other chip selects on the same bus, held high (a display on that bus)
	F4_REC_IDLE1	= 22,
	F4_REC_ZERO	= 23,	// reserved, 0
};

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
