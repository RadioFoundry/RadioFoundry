// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The STM32F4 flash stub's protocol engine (docs/spec/stub-stm32f4.md): framing, the operations and the safety rules,
// with no hardware access, so the host tests run it against a simulated flash. The target glue (f4stub_main.c) gives
// it a port: a byte sink, memory reads and the flash controller's erase and program.
#ifndef F4STUB_H
#define F4STUB_H

#include <stdbool.h>
#include <stdint.h>

#define F4STUB_VERSION		1u
#define F4STUB_BLOCK		4096u			// the unit of SUMS and READ (CRC per block, not per erase sector)
#define F4STUB_MAX_PAYLOAD	(F4STUB_BLOCK + 8u)	// a PROGRAM of one block (address + 4096) or a READ record
#define F4STUB_FRAME_HDR	6u			// 02 op/status len_hi len_lo sum 03
#define F4STUB_MAGIC		0x46345342u		// 'F4SB' (big endian on the wire, as every field here)

// Flash map facts the stub enforces (STM32F405/407: sectors 0-3 16 KB, 4 64 KB, 5-11 128 KB). The AnyRoad / TYT
// bootloader is sectors 0-2 on every radio this stub serves; the application's vector table starts sector 3
#define F4_FLASH_BASE		0x08000000u
#define F4_APP_BASE		0x0800C000u		// sector 3: the bootloader starts the application here
#define F4_PROTECTED_SECTORS	3u			// 0-2: never erased or programmed

// Request ops. Numbers shared with the HR_C7000 stub where the meaning is the same (HELLO, PEEK, SUMS, READ)
enum {
	F4OP_HELLO	= 0x01,	// -> "F4STUB" ver u8, max payload u16, flash KB u16, RDP u8, flags u8, app base u32
	F4OP_PEEK	= 0x02,	// addr u32, n u16 -> n bytes (flash, system memory / OTP / option bytes, SRAM, CCM)
	F4OP_SUMS	= 0x07,	// addr u32, count u16 -> count x (crc32 u32, blank u8) per 4 KB flash block
	F4OP_READ	= 0x09,	// addr u32, count u8, compress u8 -> count frames: crc32 u32, method u8, len u16, data
	F4OP_RESET	= 0x0D,	// magic u32 -> ok, then a system reset (the bootloader, then the application)
	F4OP_ERASE	= 0x10,	// sector u8, magic u32 -> ok once the sector is erased
	F4OP_PROGRAM	= 0x11,	// addr u32, data (n x 4 bytes) -> ok once programmed and read back
};

// Reply status bytes (the HR_C7000 stub's values where they exist)
enum {
	F4ST_OK		= 0x05,
	F4ST_BADSUM	= 0x0A,	// the request's checksum is wrong: send it again
	F4ST_BADOP	= 0x0B,	// unknown op
	F4ST_BADARG	= 0x0D,	// wrong length, alignment or range
	F4ST_TOOLONG	= 0x0E,	// the payload length is over F4STUB_MAX_PAYLOAD
	F4ST_REFUSED	= 0x0F,	// a safety rule said no (protected sector, interlock, not blank, bad magic)
	F4ST_FLASHERR	= 0x10,	// the flash controller reported an error, or the read-back differs
};

// READ record methods (as the HR_C7000 stub's)
enum { F4_METHOD_RAW = 0, F4_METHOD_LZ4 = 1, F4_METHOD_BLANK = 2 };

// HELLO flags
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
	uint16_t flash_kb;	// the flash size register
	uint8_t rdp;		// FLASH_OPTCR bits 15:8
	uint8_t flags;		// F4_HELLO_*
} f4stub_port_t;

typedef struct {
	const f4stub_port_t *port;
	uint8_t frame[F4STUB_FRAME_HDR + F4STUB_MAX_PAYLOAD];
	uint32_t have;		// bytes of the current request collected
	uint8_t out[F4STUB_FRAME_HDR + F4STUB_MAX_PAYLOAD];
	uint32_t words[F4STUB_BLOCK / 4u];
	uint32_t requests, bad_frames;
} f4stub_t;

void f4stub_init(f4stub_t *s, const f4stub_port_t *port);
// Feed received bytes; complete requests are handled (and answered) as they finish
void f4stub_feed(f4stub_t *s, const uint8_t *buf, uint32_t n);
// The line went quiet mid-request: drop the partial frame
void f4stub_idle(f4stub_t *s);

// Flash geometry (exposed for the host tests and the target glue): sector count for the flash size, a sector's base
// and size, and the sector holding an address (-1 outside flash)
unsigned f4_sector_count(uint16_t flash_kb);
uint32_t f4_sector_addr(unsigned sector);
uint32_t f4_sector_size(unsigned sector);
int f4_sector_of(uint32_t addr, uint16_t flash_kb);

uint32_t f4stub_crc32(uint32_t crc, const void *buf, uint32_t len);

#endif
