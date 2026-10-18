// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The STM32F4 flash stub's protocol engine (f4stub.h, docs/spec/stub-stm32f4.md). Portable: no register access.
//
// Safety rules, checked here so a host tool with a bug can't break them:
//  - sectors 0-2 (the bootloader) are never erased or programmed, and nothing here can write the option bytes;
//  - the interlock: any erase other than sector 3, and any program outside sector 3, needs the application's first
//    word (its initial stack pointer at 0x0800C000) to read erased. The bootloader stays in its DFU mode when that
//    word is not a RAM address, so while any application sector is half written the radio comes up in the bootloader
//    on its own. The host programs that word last, as the commit;
//  - a program only goes to words that read erased, and every word is read back;
//  - the external SPI flash holds no code: erase and program there need only the magic (erase) and erased bytes
//    (program), with every byte read back. What to write is the host's choice (a backup put back, a codeplug).
#include <string.h>
#include "f4stub.h"

#define SUM_MAX		(F4STUB_MAX_PAYLOAD / 5u)	// SUMS records per reply
#define READ_REC_HDR	7u				// crc32 u32, method u8, len u16

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

uint32_t f4stub_crc32(uint32_t crc, const void *buf, uint32_t len)
{
	// zlib's CRC-32, a nibble at a time: a 64-byte table instead of 1 KB
	static const uint32_t tab[16] = {
		0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
		0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
	};
	const uint8_t *p = buf;

	crc = ~crc;
	while (len-- > 0) {
		crc ^= *p++;
		crc = (crc >> 4) ^ tab[crc & 15u];
		crc = (crc >> 4) ^ tab[crc & 15u];
	}
	return ~crc;
}

unsigned f4_sector_count(uint16_t flash_kb)
{
	// 16+16+16+16+64 = 128 KB in sectors 0-4, then 128 KB each (single bank parts, up to 1 MB)
	if (flash_kb < 64u) {
		return flash_kb / 16u;
	}
	if (flash_kb < 128u) {
		return 4u;
	}
	unsigned n = 5u + (flash_kb - 128u) / 128u;
	return n > 12u ? 12u : n;
}

uint32_t f4_sector_addr(unsigned sector)
{
	if (sector < 4u) {
		return F4_FLASH_BASE + sector * 0x4000u;
	}
	if (sector == 4u) {
		return F4_FLASH_BASE + 0x10000u;
	}
	return F4_FLASH_BASE + 0x20000u + (sector - 5u) * 0x20000u;
}

uint32_t f4_sector_size(unsigned sector)
{
	return sector < 4u ? 0x4000u : (sector == 4u ? 0x10000u : 0x20000u);
}

int f4_sector_of(uint32_t addr, uint16_t flash_kb)
{
	unsigned n = f4_sector_count(flash_kb);
	for (unsigned i = 0; i < n; i++) {
		if (addr >= f4_sector_addr(i) && addr - f4_sector_addr(i) < f4_sector_size(i)) {
			return (int)i;
		}
	}
	return -1;
}

static uint32_t flash_end(const f4stub_t *s)
{
	unsigned n = f4_sector_count(s->port->flash_kb);
	return n == 0 ? F4_FLASH_BASE : f4_sector_addr(n - 1u) + f4_sector_size(n - 1u);
}

static bool in(uint32_t addr, uint32_t n, uint32_t lo, uint32_t hi)
{
	return addr >= lo && addr <= hi && n <= hi - addr;
}

// PEEK's readable ranges: flash, system memory with OTP, the UID and the flash size register, the option bytes, SRAM
// and CCM. Peripherals are refused: a read there can clear a flag or pop a FIFO (the USB core's, for one)
static bool peek_allowed(const f4stub_t *s, uint32_t addr, uint32_t n)
{
	return in(addr, n, F4_FLASH_BASE, flash_end(s)) || in(addr, n, 0x1FFF0000u, 0x1FFF7A30u) ||
	       in(addr, n, 0x1FFFC000u, 0x1FFFC010u) || in(addr, n, 0x20000000u, 0x20020000u) ||
	       in(addr, n, 0x10000000u, 0x10010000u);
}

static uint32_t read32(const f4stub_t *s, uint32_t addr)
{
	uint32_t v;
	s->port->read(s->port->ctx, addr, &v, 4);
	return v;
}

static bool app_committed(const f4stub_t *s)
{
	return read32(s, F4_APP_BASE) != 0xFFFFFFFFu;
}

static void reply(f4stub_t *s, uint8_t status, uint32_t n)
{
	uint8_t sum = 0;
	for (uint32_t i = 0; i < n; i++) {
		sum = (uint8_t)(sum + s->out[F4STUB_FRAME_HDR + i]);
	}
	s->out[0] = 0x02;
	s->out[1] = status;
	s->out[2] = (uint8_t)(n >> 8);
	s->out[3] = (uint8_t)n;
	s->out[4] = sum;
	s->out[5] = 0x03;
	s->port->write(s->port->ctx, s->out, F4STUB_FRAME_HDR + n);
}

static void status(f4stub_t *s, uint8_t st)
{
	reply(s, st, 0);
}

static void op_hello(f4stub_t *s)
{
	uint8_t *o = s->out + F4STUB_FRAME_HDR;
	memcpy(o, "F4STUB", 6);
	o[6] = F4STUB_VERSION;
	put16(o + 7, F4STUB_MAX_PAYLOAD);
	put16(o + 9, s->port->flash_kb);
	o[11] = s->port->rdp;
	o[12] = s->port->flags;
	put32(o + 13, F4_APP_BASE);
	put32(o + 17, s->port->xread != NULL ? s->port->xsize : 0u);
	memcpy(o + 21, s->port->xid, 3);
	reply(s, F4ST_OK, 24);
}

static void op_peek(f4stub_t *s, const uint8_t *p, uint16_t len)
{
	if (len != 6) {
		status(s, F4ST_BADARG);
		return;
	}
	uint32_t addr = get32(p);
	uint16_t n = get16(p + 4);
	if (n == 0 || n > F4STUB_MAX_PAYLOAD) {
		status(s, F4ST_BADARG);
		return;
	}
	if (!peek_allowed(s, addr, n)) {
		status(s, F4ST_REFUSED);
		return;
	}
	s->port->read(s->port->ctx, addr, s->out + F4STUB_FRAME_HDR, n);
	reply(s, F4ST_OK, n);
}

// The memory byte SUMS and READ may end with (none: internal flash)
static int mem_arg(const uint8_t *p, uint16_t len, uint16_t base_len)
{
	if (len == base_len) {
		return F4_MEM_INTERNAL;
	}
	return len == base_len + 1u && p[base_len] <= F4_MEM_SPI ? p[base_len] : -1;
}

static bool spi_present(const f4stub_t *s)
{
	return s->port->xread != NULL && s->port->xsize != 0;
}

// The blocks [addr, addr + count * 4 KB) lie in the memory and start on a block boundary
static bool blocks_ok(const f4stub_t *s, int mem, uint32_t addr, uint32_t count)
{
	if (count == 0) {
		return false;
	}
	if (mem == F4_MEM_SPI) {
		return spi_present(s) && addr % F4STUB_BLOCK == 0 && in(addr, count * F4STUB_BLOCK, 0, s->port->xsize);
	}
	return (addr - F4_FLASH_BASE) % F4STUB_BLOCK == 0 && in(addr, count * F4STUB_BLOCK, F4_FLASH_BASE, flash_end(s));
}

static bool mem_read(f4stub_t *s, int mem, uint32_t addr, void *dst, uint32_t n)
{
	if (mem == F4_MEM_SPI) {
		return s->port->xread(s->port->ctx, addr, dst, n);
	}
	s->port->read(s->port->ctx, addr, dst, n);
	return true;
}

// CRC-32 of one 4 KB block, read through the word buffer in 1 KB pieces; *blank when every byte is 0xFF. False when
// the SPI flash didn't answer
static bool block_crc(f4stub_t *s, int mem, uint32_t addr, uint32_t *crc, bool *blank)
{
	uint8_t *buf = (uint8_t *)s->words;
	*crc = 0;
	*blank = true;
	for (uint32_t off = 0; off < F4STUB_BLOCK; off += 1024u) {
		if (!mem_read(s, mem, addr + off, buf, 1024u)) {
			return false;
		}
		for (uint32_t i = 0; i < 1024u && *blank; i++) {
			*blank = buf[i] == 0xFFu;
		}
		*crc = f4stub_crc32(*crc, buf, 1024u);
	}
	return true;
}

static void op_sums(f4stub_t *s, const uint8_t *p, uint16_t len)
{
	int mem = mem_arg(p, len, 6);
	if (mem < 0) {
		status(s, F4ST_BADARG);
		return;
	}
	uint32_t addr = get32(p);
	uint16_t count = get16(p + 4);
	if (count > SUM_MAX || !blocks_ok(s, mem, addr, count)) {
		status(s, F4ST_BADARG);
		return;
	}
	uint8_t *o = s->out + F4STUB_FRAME_HDR;
	for (uint16_t i = 0; i < count; i++) {
		uint32_t crc;
		bool blank;
		if (!block_crc(s, mem, addr + i * F4STUB_BLOCK, &crc, &blank)) {
			status(s, F4ST_FLASHERR);
			return;
		}
		put32(o + i * 5u, crc);
		o[i * 5u + 4u] = blank ? 1u : 0u;
	}
	reply(s, F4ST_OK, count * 5u);
}

// One frame per block. Compression is not offered (USB full speed is fast enough): a compress request is answered
// raw, which the record's method byte tells the host
static void op_read(f4stub_t *s, const uint8_t *p, uint16_t len)
{
	int mem = mem_arg(p, len, 6);
	if (mem < 0) {
		status(s, F4ST_BADARG);
		return;
	}
	uint32_t addr = get32(p);
	uint8_t count = p[4];
	if (!blocks_ok(s, mem, addr, count)) {
		status(s, F4ST_BADARG);
		return;
	}
	uint8_t *o = s->out + F4STUB_FRAME_HDR;
	for (uint8_t i = 0; i < count; i++) {
		uint32_t a = addr + (uint32_t)i * F4STUB_BLOCK;
		if (!mem_read(s, mem, a, o + READ_REC_HDR, F4STUB_BLOCK)) {
			status(s, F4ST_FLASHERR);
			return;
		}
		bool blank = true;
		for (uint32_t k = 0; k < F4STUB_BLOCK && blank; k++) {
			blank = o[READ_REC_HDR + k] == 0xFFu;
		}
		put32(o, f4stub_crc32(0, o + READ_REC_HDR, F4STUB_BLOCK));
		o[4] = blank ? F4_METHOD_BLANK : F4_METHOD_RAW;
		uint16_t n = blank ? 0u : (uint16_t)F4STUB_BLOCK;
		put16(o + 5, n);
		reply(s, F4ST_OK, READ_REC_HDR + n);
	}
}

static void op_xerase(f4stub_t *s, const uint8_t *p, uint16_t len)
{
	if (len != 8) {
		status(s, F4ST_BADARG);
		return;
	}
	uint32_t addr = get32(p);
	if (get32(p + 4) != F4STUB_MAGIC) {
		status(s, F4ST_REFUSED);
		return;
	}
	if (!blocks_ok(s, F4_MEM_SPI, addr, 1)) {
		status(s, F4ST_BADARG);
		return;
	}
	status(s, s->port->xerase(s->port->ctx, addr) ? F4ST_OK : F4ST_FLASHERR);
}

// Bytes inside one 4 KB sector, all reading erased first, read back after
static void op_xprogram(f4stub_t *s, const uint8_t *p, uint16_t len)
{
	if (len < 5 || len - 4u > F4STUB_BLOCK) {
		status(s, F4ST_BADARG);
		return;
	}
	uint32_t addr = get32(p);
	uint32_t n = len - 4u;
	if (!spi_present(s) || !in(addr, n, 0, s->port->xsize) || addr / F4STUB_BLOCK != (addr + n - 1u) / F4STUB_BLOCK) {
		status(s, F4ST_BADARG);
		return;
	}
	uint8_t *buf = (uint8_t *)s->words;
	if (!s->port->xread(s->port->ctx, addr, buf, n)) {
		status(s, F4ST_FLASHERR);
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (buf[i] != 0xFFu) {
			status(s, F4ST_REFUSED);	// not erased: nothing written
			return;
		}
	}
	if (!s->port->xprogram(s->port->ctx, addr, p + 4, n) || !s->port->xread(s->port->ctx, addr, buf, n) ||
	    memcmp(buf, p + 4, n) != 0) {
		status(s, F4ST_FLASHERR);
		return;
	}
	status(s, F4ST_OK);
}

static void op_erase(f4stub_t *s, const uint8_t *p, uint16_t len)
{
	if (len != 5) {
		status(s, F4ST_BADARG);
		return;
	}
	unsigned sector = p[0];
	if (get32(p + 1) != F4STUB_MAGIC || sector < F4_PROTECTED_SECTORS) {
		status(s, F4ST_REFUSED);
		return;
	}
	if (sector >= f4_sector_count(s->port->flash_kb)) {
		status(s, F4ST_BADARG);
		return;
	}
	if ((int)sector != f4_sector_of(F4_APP_BASE, s->port->flash_kb) && app_committed(s)) {
		status(s, F4ST_REFUSED);	// the interlock: erase sector 3 first
		return;
	}
	status(s, s->port->erase(s->port->ctx, sector) ? F4ST_OK : F4ST_FLASHERR);
}

static void op_program(f4stub_t *s, const uint8_t *p, uint16_t len)
{
	if (len < 8 || (len - 4u) % 4u != 0 || len - 4u > F4STUB_BLOCK) {
		status(s, F4ST_BADARG);
		return;
	}
	uint32_t addr = get32(p);
	uint32_t n = (len - 4u) / 4u;
	if (addr % 4u != 0 || !in(addr, n * 4u, F4_FLASH_BASE, flash_end(s))) {
		status(s, F4ST_BADARG);
		return;
	}
	if (addr < F4_APP_BASE) {
		status(s, F4ST_REFUSED);	// the bootloader
		return;
	}
	int sec3 = f4_sector_of(F4_APP_BASE, s->port->flash_kb);
	uint32_t s3_end = f4_sector_addr((unsigned)sec3) + f4_sector_size((unsigned)sec3);
	if (addr + n * 4u > s3_end && app_committed(s)) {
		status(s, F4ST_REFUSED);	// the interlock: the application must not look valid while this is written
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (read32(s, addr + i * 4u) != 0xFFFFFFFFu) {
			status(s, F4ST_REFUSED);	// not erased: nothing written
			return;
		}
		memcpy(&s->words[i], p + 4u + i * 4u, 4);
	}
	if (!s->port->program(s->port->ctx, addr, s->words, n)) {
		status(s, F4ST_FLASHERR);
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (read32(s, addr + i * 4u) != s->words[i]) {
			status(s, F4ST_FLASHERR);
			return;
		}
	}
	status(s, F4ST_OK);
}

static void dispatch(f4stub_t *s, uint8_t op, const uint8_t *p, uint16_t len)
{
	s->requests++;
	switch (op) {
	case F4OP_HELLO:
		op_hello(s);
		break;
	case F4OP_PEEK:
		op_peek(s, p, len);
		break;
	case F4OP_SUMS:
		op_sums(s, p, len);
		break;
	case F4OP_READ:
		op_read(s, p, len);
		break;
	case F4OP_ERASE:
		op_erase(s, p, len);
		break;
	case F4OP_PROGRAM:
		op_program(s, p, len);
		break;
	case F4OP_XERASE:
		op_xerase(s, p, len);
		break;
	case F4OP_XPROGRAM:
		op_xprogram(s, p, len);
		break;
	case F4OP_RESET:
		if (len != 4 || get32(p) != F4STUB_MAGIC) {
			status(s, F4ST_REFUSED);
			break;
		}
		status(s, F4ST_OK);
		s->port->reset(s->port->ctx);
		break;
	default:
		status(s, F4ST_BADOP);
		break;
	}
}

void f4stub_init(f4stub_t *s, const f4stub_port_t *port)
{
	memset(s, 0, sizeof(*s));
	s->port = port;
}

void f4stub_idle(f4stub_t *s)
{
	if (s->have != 0) {
		s->bad_frames++;
	}
	s->have = 0;
}

// Drop the first byte of what has been collected and look for the next 0x02 (a frame that didn't check out)
static void resync(f4stub_t *s)
{
	uint32_t k = 1;
	while (k < s->have && s->frame[k] != 0x02) {
		k++;
	}
	memmove(s->frame, s->frame + k, s->have - k);
	s->have -= k;
	s->bad_frames++;
}

void f4stub_feed(f4stub_t *s, const uint8_t *buf, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		if (s->have == 0 && buf[i] != 0x02) {
			s->bad_frames++;
			continue;
		}
		s->frame[s->have++] = buf[i];
		// A resync may leave a shorter candidate that is already complete or wrong: settle it before the next byte
		while (s->have >= F4STUB_FRAME_HDR) {
			if (s->frame[5] != 0x03) {
				resync(s);
				continue;
			}
			uint16_t len = (uint16_t)((s->frame[2] << 8) | s->frame[3]);
			if (len > F4STUB_MAX_PAYLOAD) {
				status(s, F4ST_TOOLONG);
				s->have = 0;
				break;
			}
			if (s->have < F4STUB_FRAME_HDR + len) {
				break;
			}
			uint8_t sum = 0;
			for (uint16_t k = 0; k < len; k++) {
				sum = (uint8_t)(sum + s->frame[F4STUB_FRAME_HDR + k]);
			}
			s->have = 0;
			if (sum != s->frame[4]) {
				status(s, F4ST_BADSUM);
			} else {
				dispatch(s, s->frame[1], s->frame + F4STUB_FRAME_HDR, len);
			}
			break;
		}
	}
}
