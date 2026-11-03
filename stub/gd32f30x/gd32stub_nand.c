// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The GD32F30x stub's SPI NAND reader (docs/spec/stub-gd32f30x.md 5): a bit-banged mode 0 bus on any four GPIO pins
// the board record names, read only. A page is read to the chip's cache (0x13), then its main area out of the cache
// (0x03, or 0x3B in dual SPI with IO0 turned input for the data). The chip's ECC result is not checked: a page that
// fails ECC is passed on as read, as a backup wants it. Kioxia and the other vendors take the cache read with a
// two-byte column; GigaDevice with a three-byte address (the column in the low 16 bits).
#include "gd32stub.h"

#define GPIO_BASE(port)		(0x40010800u + 0x400u * (port))
#define GPIO_CTL0(port)		(*(volatile uint32_t *)(uintptr_t)(GPIO_BASE(port) + 0x00u))
#define GPIO_ISTAT(port)	(*(volatile uint32_t *)(uintptr_t)(GPIO_BASE(port) + 0x08u))
#define GPIO_OCTL(port)		(*(volatile uint32_t *)(uintptr_t)(GPIO_BASE(port) + 0x0Cu))
#define GPIO_BOP(port)		(*(volatile uint32_t *)(uintptr_t)(GPIO_BASE(port) + 0x10u))
#define RCU_APB2EN		(*(volatile uint32_t *)(uintptr_t)0x40021018u)
#define MODE_OUT		0x3u	// push-pull, 50 MHz
#define MODE_IN_PULL		0x8u

#define CMD_PAGE_READ		0x13u
#define CMD_GET_FEATURE		0x0Fu
#define CMD_READ_CACHE		0x03u
#define CMD_READ_CACHE_X2	0x3Bu
#define CMD_READ_ID		0x9Fu
#define FEATURE_STATUS		0xC0u
#define STATUS_OIP		0x01u
#define MFR_GIGADEVICE		0xC8u
#define BUSY_POLLS		20000u	// a page read takes at most about 100 us; each poll is a few us

typedef struct {
	uint8_t port;
	uint32_t mask;
} pin_t;

static pin_t pins[4];		// CS, CLK, IO0, IO1 (the record's order)
#define cs	pins[0]
#define clk	pins[1]
#define io0	pins[2]
#define io1	pins[3]
static bool dual, gigadevice;

static void mode(const pin_t *p, uint32_t m)
{
	unsigned n = 31u - (unsigned)__builtin_clz(p->mask);
	volatile uint32_t *ctl = &GPIO_CTL0(p->port) + (n >> 3);
	unsigned sh = (n & 7u) * 4u;
	*ctl = (*ctl & ~(0xFu << sh)) | (m << sh);
}

static inline void hi(const pin_t *p) { GPIO_BOP(p->port) = p->mask; }
static inline void lo(const pin_t *p) { GPIO_BOP(p->port) = p->mask << 16; }
static inline uint32_t rd(const pin_t *p) { return GPIO_ISTAT(p->port) & p->mask; }

static void pin_of(pin_t *p, uint8_t b)
{
	p->port = b >> 4;
	p->mask = 1u << (b & 0x0Fu);
	RCU_APB2EN |= 1u << (2u + p->port);
}

static uint8_t xfer(uint8_t out)
{
	uint8_t in = 0;
	for (uint8_t m = 0x80u; m != 0; m >>= 1) {
		if (out & m) {
			hi(&io0);
		} else {
			lo(&io0);
		}
		hi(&clk);
		if (rd(&io1)) {
			in |= m;
		}
		lo(&clk);
	}
	return in;
}

// Two bits a clock, IO1 the higher (the chip drives both from the clock's falling edge)
static uint8_t xfer_x2(void)
{
	uint8_t in = 0;
	for (unsigned i = 0; i < 4u; i++) {
		hi(&clk);
		in = (uint8_t)(in << 2 | (rd(&io1) ? 2u : 0u) | (rd(&io0) ? 1u : 0u));
		lo(&clk);
	}
	return in;
}

static void cmd(const uint8_t *b, unsigned n)
{
	lo(&cs);
	while (n-- > 0) {
		(void)xfer(*b++);
	}
}

static bool wait_ready(void)
{
	for (uint32_t i = 0; i < BUSY_POLLS; i++) {
		const uint8_t c[2] = { CMD_GET_FEATURE, FEATURE_STATUS };
		cmd(c, 2);
		uint8_t st = xfer(0xFFu);
		hi(&cs);
		if (!(st & STATUS_OIP)) {
			return true;
		}
	}
	return false;
}

static bool load(uint32_t page)
{
	const uint8_t c[4] = { CMD_PAGE_READ, (uint8_t)(page >> 16), (uint8_t)(page >> 8), (uint8_t)page };
	cmd(c, 4);
	hi(&cs);
	return wait_ready();
}

static void read_cache(uint16_t col, uint8_t *dst, uint32_t n)
{
	uint8_t c[5];
	unsigned k = 0;
	c[k++] = dual ? CMD_READ_CACHE_X2 : CMD_READ_CACHE;
	if (gigadevice) {
		c[k++] = 0;
	}
	c[k++] = (uint8_t)(col >> 8);
	c[k++] = (uint8_t)col;
	c[k++] = 0;		// the dummy byte
	cmd(c, k);
	if (dual) {
		mode(&io0, MODE_IN_PULL);
		hi(&io0);	// OCTL 1: the pull is up
		while (n-- > 0) {
			*dst++ = xfer_x2();
		}
		mode(&io0, MODE_OUT);
	} else {
		while (n-- > 0) {
			*dst++ = xfer(0xFFu);
		}
	}
	hi(&cs);
}

bool gd32nand_init(const gd32_board_t *b)
{
	// CS idles high, CLK low, IO0 an output, IO1 an input with its pull up (OCTL 1)
	for (unsigned i = 0; i < 4u; i++) {
		pin_of(&pins[i], b->nand_pin[i]);
		if (i == 1u) {
			lo(&pins[i]);
		} else {
			hi(&pins[i]);
		}
		mode(&pins[i], i == 3u ? MODE_IN_PULL : MODE_OUT);
	}
	dual = (b->nand_flags & GD32_NAND_DUAL) != 0;
	// The ID: some parts answer the command's dummy byte with the maker, some after it: the first byte that isn't 0xFF
	const uint8_t c[1] = { CMD_READ_ID };
	cmd(c, 1);
	uint8_t mfr = 0xFFu;
	for (unsigned i = 0; i < 3u && mfr == 0xFFu; i++) {
		mfr = xfer(0x00u);
	}
	hi(&cs);
	gigadevice = mfr == MFR_GIGADEVICE;
	return mfr != 0xFFu && mfr != 0x00u;
}

bool gd32nand_read(uint32_t addr, void *dst, uint32_t n)
{
	uint8_t *p = dst;
	while (n > 0) {
		uint32_t page = addr / GD32_NAND_PAGE;
		uint16_t col = (uint16_t)(addr % GD32_NAND_PAGE);
		uint32_t k = GD32_NAND_PAGE - col;
		if (k > n) {
			k = n;
		}
		if (!load(page)) {
			return false;
		}
		read_cache(col, p, k);
		addr += k;
		p += k;
		n -= k;
	}
	return true;
}
