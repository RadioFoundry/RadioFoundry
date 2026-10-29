// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The flash stubs' shared protocol engine (docs/spec/stub-stm32f4.md 13, "The shared engine"): framing, the
// operations, the safety rules, CRC-32 SUMS and block READ, for any microcontroller whose stub runs from RAM and talks
// over a byte stream. No hardware access, so the host tests run it against a simulated flash.
//
// A target (stub/stm32f4 for the STM32F405 radios; a Kinetis K22F target can follow the same pattern) supplies a
// stubeng_port_t:
//  - its HELLO name (6 bytes), protocol version and op magic;
//  - the internal flash geometry (stubeng_flash_t): base address, size and the sector map as runs of equal sectors,
//    the program unit (4 bytes on the STM32F4, 8 on a part that programs phrases) and the lowest address any CONFIG may
//    open for writing (the target's own floor, below every board's bootloader end);
//  - the address ranges PEEK may read besides the internal flash (RAM, ID registers; never peripherals);
//  - callbacks: write a reply, read memory, erase a sector, program words, reset, the external SPI flash (read, erase
//    a 4 KB sector, program inside one), configure: check and apply the board record the host sends in CONFIG, and
//    optionally feed the watchdog.
//
// The board record is the target's business (its layout is in the target's header); the engine keeps a copy for the
// HELLO echo and takes these facts back from the target: the application base (the first writable internal flash
// address, which is also the commit word, see below), optionally the end of the writable internal flash (a radio that
// keeps data in its last sectors) and the SPI flash's size, JEDEC ID and optionally its write floor (a radio whose own
// data fills the chip's first part). Until a CONFIG has been accepted the stub reads internal flash only, and every
// write answers STUB_ST_NOCONFIG.
//
// Safety rules, checked here so a host tool with a bug can't break them:
//  - nothing below the application base or from the application end up is erased or programmed, and neither can be
//    configured below the target's floor or off a sector start;
//  - the interlock: any erase other than the commit sector (the one holding the application base), and any program
//    outside it, needs the word at the application base to read erased. The radios' bootloaders treat an erased first
//    word as "no application", so while any application sector is half written the radio stays in its bootloader. The
//    host programs that word last, as the commit;
//  - a program only goes to units that read erased, and every word is read back;
//  - the external SPI flash holds no code: erase there needs the magic, program needs erased bytes, every byte is read
//    back, and nothing below the board's write floor is erased or programmed. What to write is the host's choice (a
//    backup put back, a codeplug);
//  - its security registers (SREAD / SWRITE, a Winbond chip only): a write carries the magic, is refused while the
//    register's one-time lock bit (status register 2, LB1-LB3) is set, and is read back. Nothing ever writes a status
//    register, so nothing can set a lock bit.
#ifndef STUBENG_H
#define STUBENG_H

#include <stdbool.h>
#include <stdint.h>

#define STUBENG_BLOCK		4096u			// the unit of SUMS and READ (CRC per block, not per erase sector)
#define STUBENG_MAX_PAYLOAD	(STUBENG_BLOCK + 8u)	// a PROGRAM of one block (address + 4096) or a READ record
#define STUBENG_FRAME_HDR	6u			// 02 op/status len_hi len_lo sum 03
#define STUBENG_CONFIG_MAX	32u			// the longest board record a target may define
#define STUBENG_HELLO_LEN	26u			// HELLO without the board record echo
#define STUBENG_SREG_LEN	256u			// one SPI flash security register
#define STUBENG_SREG_COUNT	3u			// registers 1-3
#define STUBENG_SREG_MFR	0xEFu			// the JEDEC manufacturer whose command set SREAD / SWRITE use (Winbond)
#define STUBENG_SR2_LB1		0x08u			// status register 2's LB1 bit; LB2 and LB3 are the next two up

// The memories SUMS and READ take as an optional last byte: internal flash (the default when the byte is left out) or
// the external SPI flash (addresses from 0)
enum { STUB_MEM_INTERNAL = 0, STUB_MEM_SPI = 1 };

// Request ops. Numbers shared with the HR_C7000 stub where the meaning is the same (HELLO, PEEK, SUMS, READ)
enum {
	STUB_OP_HELLO		= 0x01,	// -> name[6], ver u8, max payload u16, flash KB u16, security u8, flags u8, app base u32,
					//    SPI flash size u32, JEDEC ID[3], board id u8, record length u8, record (the echo)
	STUB_OP_PEEK		= 0x02,	// addr u32, n u16 -> n bytes (internal flash and the target's ranges)
	STUB_OP_SUMS		= 0x07,	// addr u32, count u16 [, mem u8] -> count x (crc32 u32, blank u8) per 4 KB block
	STUB_OP_READ		= 0x09,	// addr u32, count u8, compress u8 [, mem u8] -> count frames: crc32, method, len u16, data
	STUB_OP_RESET		= 0x0D,	// magic u32 -> ok, then the target's reset
	STUB_OP_ERASE		= 0x10,	// sector u8, magic u32 -> ok once the sector is erased
	STUB_OP_PROGRAM		= 0x11,	// addr u32, data (whole program units) -> ok once programmed and read back
	STUB_OP_XERASE		= 0x12,	// addr u32 (a 4 KB SPI flash sector), magic u32 -> ok once erased
	STUB_OP_XPROGRAM	= 0x13,	// addr u32, data (1 to 4096 bytes in one 4 KB sector) -> ok once programmed and read back
	STUB_OP_CONFIG		= 0x14,	// the target's board record -> ok once applied (HELLO echoes it)
	STUB_OP_SREAD		= 0x15,	// reg u8 (0-3) -> status register 2 u8; for reg 1-3 then crc32 u32, the 256 bytes
	STUB_OP_SWRITE		= 0x16,	// reg u8 (1-3), magic u32, 256 bytes -> ok once erased, programmed and read back
};

// Reply status bytes (the HR_C7000 stub's values where they exist)
enum {
	STUB_ST_OK		= 0x05,
	STUB_ST_BADSUM		= 0x0A,	// the request's checksum is wrong: send it again
	STUB_ST_BADOP		= 0x0B,	// unknown op
	STUB_ST_BADARG		= 0x0D,	// wrong length, alignment or range; a board record the target refused
	STUB_ST_TOOLONG		= 0x0E,	// the payload length is over STUBENG_MAX_PAYLOAD
	STUB_ST_REFUSED		= 0x0F,	// a safety rule said no (protected sector, interlock, not blank, bad magic)
	STUB_ST_FLASHERR	= 0x10,	// the flash controller reported an error, or the read-back differs
	STUB_ST_NOCONFIG	= 0x11,	// a write before a board record was accepted
};

// READ record methods (as the HR_C7000 stub's)
enum { STUB_METHOD_RAW = 0, STUB_METHOD_LZ4 = 1, STUB_METHOD_BLANK = 2 };

// HELLO flags: the target's own bits below 0x80; the engine adds STUB_HELLO_CONFIGURED
#define STUB_HELLO_CONFIGURED	0x80u

// `count` sectors of `size` bytes each, one run of the sector map
typedef struct {
	uint32_t size;
	uint16_t count;
} stubeng_run_t;

typedef struct {
	uint32_t base;			// first address
	uint32_t size;			// bytes on this chip: sectors that don't fit wholly are not there
	const stubeng_run_t *runs;	// the sector map in address order
	uint8_t nruns;
	uint8_t prog_unit;		// PROGRAM's address and length granularity: 4 or 8 bytes
	uint32_t min_app_base;		// no CONFIG opens anything below this for writing
} stubeng_flash_t;

typedef struct {
	uint32_t lo, hi;		// [lo, hi)
} stubeng_range_t;

// What the target's configure gives back to the engine
typedef struct {
	uint32_t app_base;		// first writable internal flash address, a sector start (the engine checks it)
	uint32_t app_end;		// first address above the writable internal flash, a sector start; 0: the flash's end
	uint32_t xsize;			// SPI flash bytes (0: none, or none answered)
	uint32_t xfloor;		// SPI flash below this is never erased or programmed (0: none of it is protected)
	uint8_t xid[3];			// its JEDEC ID
} stubeng_config_t;

typedef struct {
	void *ctx;
	const char *name;		// HELLO's first 6 bytes ("F4STUB")
	uint8_t version;		// the target's protocol version
	uint32_t magic;			// ERASE, XERASE and RESET carry it
	stubeng_flash_t flash;
	const stubeng_range_t *peek;	// PEEK's ranges besides the internal flash
	uint8_t npeek;
	uint8_t security;		// HELLO byte 11 (the STM32F4's RDP byte)
	uint8_t flags;			// HELLO byte 12, the target's bits

	// Send bytes to the host (a whole frame per call)
	void (*write)(void *ctx, const uint8_t *buf, uint32_t n);
	// Copy n bytes from an address the engine has already checked
	void (*read)(void *ctx, uint32_t addr, void *dst, uint32_t n);
	// Erase one internal flash sector (index in the map); false on a controller error
	bool (*erase)(void *ctx, unsigned sector);
	// Program n words at addr (aligned to the program unit, checked erased); false on a controller error
	bool (*program)(void *ctx, uint32_t addr, const uint32_t *words, uint32_t n);
	// Called after the RESET reply has been sent; does not return on the target
	void (*reset)(void *ctx);
	// The external SPI flash (NULL where the target has none): read, erase one 4 KB sector, program bytes inside one
	// sector (checked erased); false on a chip that stays busy or doesn't answer
	bool (*xread)(void *ctx, uint32_t addr, void *dst, uint32_t n);
	bool (*xerase)(void *ctx, uint32_t addr);
	bool (*xprogram)(void *ctx, uint32_t addr, const uint8_t *data, uint32_t n);
	// Optional (NULL: none): the SPI flash's security registers. reg 0: status register 2 into dst[0]; reg 1-3 with
	// src NULL: the register's 256 bytes into dst; with src: erase the register, then program src's 256 bytes. Never
	// a status register write: the lock bits are one-time
	bool (*xsreg)(void *ctx, unsigned reg, uint8_t *dst, const uint8_t *src);
	// Check and apply a board record (1 to STUBENG_CONFIG_MAX bytes, the target's layout, first byte the nonzero
	// board id). False: refused, and the stub is left unconfigured
	bool (*configure)(void *ctx, const uint8_t *rec, uint32_t n, stubeng_config_t *out);
	// Optional (NULL: none): feed the target's watchdog. The engine calls it at every request, before each erase or
	// program, and once per 4 KB block in SUMS and READ, so no request runs a second without a feed
	// (docs/spec/watchdog.md 3.1)
	void (*feed)(void *ctx);
} stubeng_port_t;

typedef struct {
	const stubeng_port_t *port;
	uint8_t frame[STUBENG_FRAME_HDR + STUBENG_MAX_PAYLOAD];
	uint32_t have;		// bytes of the current request collected
	uint8_t out[STUBENG_FRAME_HDR + STUBENG_MAX_PAYLOAD];
	uint32_t words[STUBENG_BLOCK / 4u];	// PROGRAM's words; also the read buffer of SUMS and XPROGRAM
	uint32_t requests, bad_frames;
	// The board configuration (CONFIG): the record, and what the target derived from it
	bool configured;
	uint8_t rec_len;
	uint8_t rec[STUBENG_CONFIG_MAX];
	stubeng_config_t cfg;
} stubeng_t;

void stubeng_init(stubeng_t *s, const stubeng_port_t *port);
// Feed received bytes; complete requests are handled (and answered) as they finish
void stubeng_feed(stubeng_t *s, const uint8_t *buf, uint32_t n);
// The line went quiet mid-request: drop the partial frame
void stubeng_idle(stubeng_t *s);

// The sector map (exposed for the host tests and the target glue): sectors on this chip, a sector's address and size,
// and the sector holding an address (-1 outside flash)
unsigned stubeng_sector_count(const stubeng_flash_t *f);
uint32_t stubeng_sector_addr(const stubeng_flash_t *f, unsigned sector);
uint32_t stubeng_sector_size(const stubeng_flash_t *f, unsigned sector);
int stubeng_sector_of(const stubeng_flash_t *f, uint32_t addr);

uint32_t stubeng_crc32(uint32_t crc, const void *buf, uint32_t len);

#endif
