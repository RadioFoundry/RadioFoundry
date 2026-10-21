// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The flash stubs' shared protocol engine (stubeng.h, docs/spec/stub-stm32f4.md 13). Portable: no register access.
// The safety rules are listed in stubeng.h.
#include <string.h>
#include "stubeng.h"

#define SUM_MAX		(STUBENG_MAX_PAYLOAD / 5u)	// SUMS records per reply
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

uint32_t stubeng_crc32(uint32_t crc, const void *buf, uint32_t len)
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

// ---- the sector map: runs of equal sectors, cut off at the chip's size

unsigned stubeng_sector_count(const stubeng_flash_t *f)
{
	uint32_t off = 0;
	unsigned n = 0;
	for (unsigned r = 0; r < f->nruns; r++) {
		for (unsigned k = 0; k < f->runs[r].count; k++) {
			if (f->runs[r].size > f->size - off) {
				return n;
			}
			off += f->runs[r].size;
			n++;
		}
	}
	return n;
}

uint32_t stubeng_sector_addr(const stubeng_flash_t *f, unsigned sector)
{
	uint32_t addr = f->base;
	for (unsigned r = 0; r < f->nruns; r++) {
		if (sector < f->runs[r].count) {
			return addr + sector * f->runs[r].size;
		}
		sector -= f->runs[r].count;
		addr += f->runs[r].count * f->runs[r].size;
	}
	return addr;
}

uint32_t stubeng_sector_size(const stubeng_flash_t *f, unsigned sector)
{
	for (unsigned r = 0; r < f->nruns; r++) {
		if (sector < f->runs[r].count) {
			return f->runs[r].size;
		}
		sector -= f->runs[r].count;
	}
	return 0;
}

int stubeng_sector_of(const stubeng_flash_t *f, uint32_t addr)
{
	unsigned n = stubeng_sector_count(f);
	for (unsigned i = 0; i < n; i++) {
		uint32_t a = stubeng_sector_addr(f, i);
		if (addr >= a && addr - a < stubeng_sector_size(f, i)) {
			return (int)i;
		}
	}
	return -1;
}

static const stubeng_flash_t *geo(const stubeng_t *s)
{
	return &s->port->flash;
}

static uint32_t flash_end(const stubeng_t *s)
{
	unsigned n = stubeng_sector_count(geo(s));
	return n == 0 ? geo(s)->base : stubeng_sector_addr(geo(s), n - 1u) + stubeng_sector_size(geo(s), n - 1u);
}

static bool in(uint32_t addr, uint32_t n, uint32_t lo, uint32_t hi)
{
	return addr >= lo && addr <= hi && n <= hi - addr;
}

// PEEK's readable ranges: the internal flash and the target's list. Anything else is refused: a read of a peripheral
// can clear a flag or pop a FIFO
static bool peek_allowed(const stubeng_t *s, uint32_t addr, uint32_t n)
{
	if (in(addr, n, geo(s)->base, flash_end(s))) {
		return true;
	}
	for (unsigned i = 0; i < s->port->npeek; i++) {
		if (in(addr, n, s->port->peek[i].lo, s->port->peek[i].hi)) {
			return true;
		}
	}
	return false;
}

static uint32_t read32(const stubeng_t *s, uint32_t addr)
{
	uint32_t v;
	s->port->read(s->port->ctx, addr, &v, 4);
	return v;
}

static bool app_committed(const stubeng_t *s)
{
	return read32(s, s->cfg.app_base) != 0xFFFFFFFFu;
}

static void reply(stubeng_t *s, uint8_t status, uint32_t n)
{
	uint8_t sum = 0;
	for (uint32_t i = 0; i < n; i++) {
		sum = (uint8_t)(sum + s->out[STUBENG_FRAME_HDR + i]);
	}
	s->out[0] = 0x02;
	s->out[1] = status;
	s->out[2] = (uint8_t)(n >> 8);
	s->out[3] = (uint8_t)n;
	s->out[4] = sum;
	s->out[5] = 0x03;
	s->port->write(s->port->ctx, s->out, STUBENG_FRAME_HDR + n);
}

static void status(stubeng_t *s, uint8_t st)
{
	reply(s, st, 0);
}

static void op_hello(stubeng_t *s)
{
	uint8_t *o = s->out + STUBENG_FRAME_HDR;
	memcpy(o, s->port->name, 6);
	o[6] = s->port->version;
	put16(o + 7, STUBENG_MAX_PAYLOAD);
	put16(o + 9, (uint16_t)(geo(s)->size / 1024u));
	o[11] = s->port->security;
	o[12] = (uint8_t)(s->port->flags | (s->configured ? STUB_HELLO_CONFIGURED : 0u));
	put32(o + 13, s->configured ? s->cfg.app_base : 0u);
	put32(o + 17, s->port->xread != NULL ? s->cfg.xsize : 0u);
	memcpy(o + 21, s->cfg.xid, 3);
	o[24] = s->configured ? s->rec[0] : 0u;
	o[25] = s->configured ? s->rec_len : 0u;
	memcpy(o + STUBENG_HELLO_LEN, s->rec, o[25]);
	reply(s, STUB_ST_OK, STUBENG_HELLO_LEN + o[25]);
}

// The board record. Whatever happens, the stub is unconfigured until the target and the engine have both accepted it
static void op_config(stubeng_t *s, const uint8_t *p, uint16_t len)
{
	s->configured = false;
	memset(&s->cfg, 0, sizeof(s->cfg));
	s->rec_len = 0;
	if (len == 0 || len > STUBENG_CONFIG_MAX || p[0] == 0 || s->port->configure == NULL) {
		status(s, STUB_ST_BADARG);
		return;
	}
	stubeng_config_t c;
	memset(&c, 0, sizeof(c));
	if (!s->port->configure(s->port->ctx, p, len, &c)) {
		status(s, STUB_ST_BADARG);
		return;
	}
	// The application base: a sector start inside the flash, not below the target's floor, never sector 0
	int sec = stubeng_sector_of(geo(s), c.app_base);
	if (sec <= 0 || stubeng_sector_addr(geo(s), (unsigned)sec) != c.app_base || c.app_base < geo(s)->min_app_base) {
		status(s, STUB_ST_BADARG);
		return;
	}
	s->cfg = c;
	memcpy(s->rec, p, len);
	s->rec_len = (uint8_t)len;
	s->configured = true;
	status(s, STUB_ST_OK);
}

static void op_peek(stubeng_t *s, const uint8_t *p, uint16_t len)
{
	if (len != 6) {
		status(s, STUB_ST_BADARG);
		return;
	}
	uint32_t addr = get32(p);
	uint16_t n = get16(p + 4);
	if (n == 0 || n > STUBENG_MAX_PAYLOAD) {
		status(s, STUB_ST_BADARG);
		return;
	}
	if (!peek_allowed(s, addr, n)) {
		status(s, STUB_ST_REFUSED);
		return;
	}
	s->port->read(s->port->ctx, addr, s->out + STUBENG_FRAME_HDR, n);
	reply(s, STUB_ST_OK, n);
}

// The memory byte SUMS and READ may end with (none: internal flash)
static int mem_arg(const uint8_t *p, uint16_t len, uint16_t base_len)
{
	if (len == base_len) {
		return STUB_MEM_INTERNAL;
	}
	return len == base_len + 1u && p[base_len] <= STUB_MEM_SPI ? p[base_len] : -1;
}

static bool spi_present(const stubeng_t *s)
{
	return s->port->xread != NULL && s->cfg.xsize != 0;
}

// The blocks [addr, addr + count * 4 KB) lie in the memory and start on a block boundary
static bool blocks_ok(const stubeng_t *s, int mem, uint32_t addr, uint32_t count)
{
	if (count == 0) {
		return false;
	}
	if (mem == STUB_MEM_SPI) {
		return spi_present(s) && addr % STUBENG_BLOCK == 0 && in(addr, count * STUBENG_BLOCK, 0, s->cfg.xsize);
	}
	return (addr - geo(s)->base) % STUBENG_BLOCK == 0 &&
	       in(addr, count * STUBENG_BLOCK, geo(s)->base, flash_end(s));
}

static bool mem_read(stubeng_t *s, int mem, uint32_t addr, void *dst, uint32_t n)
{
	if (mem == STUB_MEM_SPI) {
		return s->port->xread(s->port->ctx, addr, dst, n);
	}
	s->port->read(s->port->ctx, addr, dst, n);
	return true;
}

// CRC-32 of one 4 KB block, read through the word buffer in 1 KB pieces; *blank when every byte is 0xFF. False when
// the SPI flash didn't answer
static bool block_crc(stubeng_t *s, int mem, uint32_t addr, uint32_t *crc, bool *blank)
{
	uint8_t *buf = (uint8_t *)s->words;
	*crc = 0;
	*blank = true;
	for (uint32_t off = 0; off < STUBENG_BLOCK; off += 1024u) {
		if (!mem_read(s, mem, addr + off, buf, 1024u)) {
			return false;
		}
		for (uint32_t i = 0; i < 1024u && *blank; i++) {
			*blank = buf[i] == 0xFFu;
		}
		*crc = stubeng_crc32(*crc, buf, 1024u);
	}
	return true;
}

static void op_sums(stubeng_t *s, const uint8_t *p, uint16_t len)
{
	int mem = mem_arg(p, len, 6);
	if (mem < 0) {
		status(s, STUB_ST_BADARG);
		return;
	}
	uint32_t addr = get32(p);
	uint16_t count = get16(p + 4);
	if (count > SUM_MAX || !blocks_ok(s, mem, addr, count)) {
		status(s, STUB_ST_BADARG);
		return;
	}
	uint8_t *o = s->out + STUBENG_FRAME_HDR;
	for (uint16_t i = 0; i < count; i++) {
		uint32_t crc;
		bool blank;
		if (!block_crc(s, mem, addr + i * STUBENG_BLOCK, &crc, &blank)) {
			status(s, STUB_ST_FLASHERR);
			return;
		}
		put32(o + i * 5u, crc);
		o[i * 5u + 4u] = blank ? 1u : 0u;
	}
	reply(s, STUB_ST_OK, count * 5u);
}

// One frame per block. Compression is not offered (USB full speed is fast enough): a compress request is answered
// raw, which the record's method byte tells the host
static void op_read(stubeng_t *s, const uint8_t *p, uint16_t len)
{
	int mem = mem_arg(p, len, 6);
	if (mem < 0) {
		status(s, STUB_ST_BADARG);
		return;
	}
	uint32_t addr = get32(p);
	uint8_t count = p[4];
	if (!blocks_ok(s, mem, addr, count)) {
		status(s, STUB_ST_BADARG);
		return;
	}
	uint8_t *o = s->out + STUBENG_FRAME_HDR;
	for (uint8_t i = 0; i < count; i++) {
		uint32_t a = addr + (uint32_t)i * STUBENG_BLOCK;
		if (!mem_read(s, mem, a, o + READ_REC_HDR, STUBENG_BLOCK)) {
			status(s, STUB_ST_FLASHERR);
			return;
		}
		bool blank = true;
		for (uint32_t k = 0; k < STUBENG_BLOCK && blank; k++) {
			blank = o[READ_REC_HDR + k] == 0xFFu;
		}
		put32(o, stubeng_crc32(0, o + READ_REC_HDR, STUBENG_BLOCK));
		o[4] = blank ? STUB_METHOD_BLANK : STUB_METHOD_RAW;
		uint16_t n = blank ? 0u : (uint16_t)STUBENG_BLOCK;
		put16(o + 5, n);
		reply(s, STUB_ST_OK, READ_REC_HDR + n);
	}
}

static void op_xerase(stubeng_t *s, const uint8_t *p, uint16_t len)
{
	if (len != 8) {
		status(s, STUB_ST_BADARG);
		return;
	}
	uint32_t addr = get32(p);
	if (get32(p + 4) != s->port->magic) {
		status(s, STUB_ST_REFUSED);
		return;
	}
	if (!blocks_ok(s, STUB_MEM_SPI, addr, 1)) {
		status(s, STUB_ST_BADARG);
		return;
	}
	status(s, s->port->xerase(s->port->ctx, addr) ? STUB_ST_OK : STUB_ST_FLASHERR);
}

// Bytes inside one 4 KB sector, all reading erased first, read back after
static void op_xprogram(stubeng_t *s, const uint8_t *p, uint16_t len)
{
	if (len < 5 || len - 4u > STUBENG_BLOCK) {
		status(s, STUB_ST_BADARG);
		return;
	}
	uint32_t addr = get32(p);
	uint32_t n = len - 4u;
	if (!spi_present(s) || !in(addr, n, 0, s->cfg.xsize) || addr / STUBENG_BLOCK != (addr + n - 1u) / STUBENG_BLOCK) {
		status(s, STUB_ST_BADARG);
		return;
	}
	uint8_t *buf = (uint8_t *)s->words;
	if (!s->port->xread(s->port->ctx, addr, buf, n)) {
		status(s, STUB_ST_FLASHERR);
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (buf[i] != 0xFFu) {
			status(s, STUB_ST_REFUSED);	// not erased: nothing written
			return;
		}
	}
	if (!s->port->xprogram(s->port->ctx, addr, p + 4, n) || !s->port->xread(s->port->ctx, addr, buf, n) ||
	    memcmp(buf, p + 4, n) != 0) {
		status(s, STUB_ST_FLASHERR);
		return;
	}
	status(s, STUB_ST_OK);
}

static void op_erase(stubeng_t *s, const uint8_t *p, uint16_t len)
{
	if (len != 5) {
		status(s, STUB_ST_BADARG);
		return;
	}
	unsigned sector = p[0];
	if (get32(p + 1) != s->port->magic) {
		status(s, STUB_ST_REFUSED);
		return;
	}
	if (sector >= stubeng_sector_count(geo(s))) {
		status(s, STUB_ST_BADARG);
		return;
	}
	if (stubeng_sector_addr(geo(s), sector) < s->cfg.app_base) {
		status(s, STUB_ST_REFUSED);	// the bootloader
		return;
	}
	if ((int)sector != stubeng_sector_of(geo(s), s->cfg.app_base) && app_committed(s)) {
		status(s, STUB_ST_REFUSED);	// the interlock: erase the commit sector first
		return;
	}
	status(s, s->port->erase(s->port->ctx, sector) ? STUB_ST_OK : STUB_ST_FLASHERR);
}

static void op_program(stubeng_t *s, const uint8_t *p, uint16_t len)
{
	uint32_t unit = geo(s)->prog_unit;
	if (len < 4u + unit || (len - 4u) % unit != 0 || len - 4u > STUBENG_BLOCK) {
		status(s, STUB_ST_BADARG);
		return;
	}
	uint32_t addr = get32(p);
	uint32_t n = (len - 4u) / 4u;
	if (addr % unit != 0 || !in(addr, n * 4u, geo(s)->base, flash_end(s))) {
		status(s, STUB_ST_BADARG);
		return;
	}
	if (addr < s->cfg.app_base) {
		status(s, STUB_ST_REFUSED);	// the bootloader
		return;
	}
	unsigned commit = (unsigned)stubeng_sector_of(geo(s), s->cfg.app_base);
	uint32_t commit_end = stubeng_sector_addr(geo(s), commit) + stubeng_sector_size(geo(s), commit);
	if (addr + n * 4u > commit_end && app_committed(s)) {
		status(s, STUB_ST_REFUSED);	// the interlock: the application must not look valid while this is written
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (read32(s, addr + i * 4u) != 0xFFFFFFFFu) {
			status(s, STUB_ST_REFUSED);	// not erased: nothing written
			return;
		}
		memcpy(&s->words[i], p + 4u + i * 4u, 4);
	}
	if (!s->port->program(s->port->ctx, addr, s->words, n)) {
		status(s, STUB_ST_FLASHERR);
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (read32(s, addr + i * 4u) != s->words[i]) {
			status(s, STUB_ST_FLASHERR);
			return;
		}
	}
	status(s, STUB_ST_OK);
}

static void dispatch(stubeng_t *s, uint8_t op, const uint8_t *p, uint16_t len)
{
	s->requests++;
	bool write = op == STUB_OP_ERASE || op == STUB_OP_PROGRAM || op == STUB_OP_XERASE || op == STUB_OP_XPROGRAM;
	if (write && !s->configured) {
		status(s, STUB_ST_NOCONFIG);
		return;
	}
	switch (op) {
	case STUB_OP_HELLO:
		op_hello(s);
		break;
	case STUB_OP_CONFIG:
		op_config(s, p, len);
		break;
	case STUB_OP_PEEK:
		op_peek(s, p, len);
		break;
	case STUB_OP_SUMS:
		op_sums(s, p, len);
		break;
	case STUB_OP_READ:
		op_read(s, p, len);
		break;
	case STUB_OP_ERASE:
		op_erase(s, p, len);
		break;
	case STUB_OP_PROGRAM:
		op_program(s, p, len);
		break;
	case STUB_OP_XERASE:
		op_xerase(s, p, len);
		break;
	case STUB_OP_XPROGRAM:
		op_xprogram(s, p, len);
		break;
	case STUB_OP_RESET:
		if (len != 4 || get32(p) != s->port->magic) {
			status(s, STUB_ST_REFUSED);
			break;
		}
		status(s, STUB_ST_OK);
		s->port->reset(s->port->ctx);
		break;
	default:
		status(s, STUB_ST_BADOP);
		break;
	}
}

void stubeng_init(stubeng_t *s, const stubeng_port_t *port)
{
	memset(s, 0, sizeof(*s));
	s->port = port;
}

void stubeng_idle(stubeng_t *s)
{
	if (s->have != 0) {
		s->bad_frames++;
	}
	s->have = 0;
}

// Drop the first byte of what has been collected and look for the next 0x02 (a frame that didn't check out)
static void resync(stubeng_t *s)
{
	uint32_t k = 1;
	while (k < s->have && s->frame[k] != 0x02) {
		k++;
	}
	memmove(s->frame, s->frame + k, s->have - k);
	s->have -= k;
	s->bad_frames++;
}

void stubeng_feed(stubeng_t *s, const uint8_t *buf, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		if (s->have == 0 && buf[i] != 0x02) {
			s->bad_frames++;
			continue;
		}
		s->frame[s->have++] = buf[i];
		// A resync may leave a shorter candidate that is already complete or wrong: settle it before the next byte
		while (s->have >= STUBENG_FRAME_HDR) {
			if (s->frame[5] != 0x03) {
				resync(s);
				continue;
			}
			uint16_t len = (uint16_t)((s->frame[2] << 8) | s->frame[3]);
			if (len > STUBENG_MAX_PAYLOAD) {
				status(s, STUB_ST_TOOLONG);
				s->have = 0;
				break;
			}
			if (s->have < STUBENG_FRAME_HDR + len) {
				break;
			}
			uint8_t sum = 0;
			for (uint16_t k = 0; k < len; k++) {
				sum = (uint8_t)(sum + s->frame[STUBENG_FRAME_HDR + k]);
			}
			s->have = 0;
			if (sum != s->frame[4]) {
				status(s, STUB_ST_BADSUM);
			} else {
				dispatch(s, s->frame[1], s->frame + STUBENG_FRAME_HDR, len);
			}
			break;
		}
	}
}
