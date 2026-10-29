// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT1141 local oscillator (at1141.h). The tables and sequences are the working firmware's, in its order
#include "at1141.h"

#include <stddef.h>

#define REG_BANK		0x7Fu
#define REG_CTRL		0x30u
#define REG_LEVEL		0x26u
#define REG_GPIO_HI		0x2Au		// GPIO 4..7, a nibble each (0xF = high)
#define REG_GPIO_LO		0x2Bu		// GPIO 0..3
#define REG_MUL			0x15u
#define REG_DIV			0x16u
#define REG_REF			0x19u		// bit 6: reference divided by 2
#define REG_21			0x21u
#define REG_MATCH		0x3Fu
#define REG_PLL_LO		0x62u
#define REG_PLL_HI		0x63u

#define CTRL_STANDBY		0x4006u
#define CTRL_ON			0x4046u
#define CTRL_OFF		0x4002u
#define RESTART_MS		10u

#define REF_HZ			26000000u
#define MATCH_DIV		406250u		// REF_HZ / 64
#define WAIT			0xFFu		// a table entry that waits val ms

typedef struct {
	uint8_t reg;
	uint16_t val;
} regval_t;

static const regval_t table_a[] = {
	{ 0x02, 0x0012 }, { 0x03, 0x0242 }, { 0x05, 0x0040 }, { 0x06, 0x0000 }, { 0x07, 0x01FF }, { 0x08, 0xE000 },
	{ 0x09, 0x0112 }, { 0x0E, 0x23F1 }, { 0x12, 0x0031 }, { 0x14, 0x101C }, { 0x15, 0x020E }, { 0x19, 0x88E1 },
	{ 0x1A, 0x2813 }, { 0x1B, 0x110F }, { 0x1C, 0xFCCF }, { 0x1D, 0xA8C4 }, { 0x20, 0x0998 }, { 0x21, 0x0C13 },
	{ 0x23, 0x003B }, { 0x24, 0xC208 }, { 0x26, 0x4140 }, { 0x33, 0x0180 }, { 0x34, 0x03B1 }, { 0x3E, 0x0AED },
	{ 0x4F, 0x1FFF }, { 0x5F, 0x3F8A }, { 0x65, 0x8000 }, { 0x66, 0x001F }, { 0x68, 0xFFFF }, { 0x6B, 0x00CB },
	{ REG_BANK, 0x0001 },
	{ 0x03, 0xB692 }, { 0x04, 0x00C0 }, { 0x05, 0x04FB }, { 0x06, 0x12ED }, { 0x07, 0x7F80 }, { 0x08, 0x2341 },
	{ 0x09, 0x2F81 }, { 0x0A, 0x0B22 }, { 0x0B, 0x0000 }, { 0x0C, 0x0100 }, { 0x0D, 0x1003 }, { 0x0E, 0x01FC },
	{ 0x0F, 0x053E }, { 0x10, 0x0500 }, { 0x11, 0x2024 }, { 0x12, 0x04FB }, { 0x13, 0x12ED }, { 0x14, 0x7F80 },
	{ 0x15, 0x1110 }, { 0x16, 0x00FE }, { 0x17, 0x89C4 }, { 0x18, 0x8408 }, { 0x19, 0x1102 }, { 0x1A, 0x04FB },
	{ 0x1B, 0x05FA }, { 0x1C, 0x7F80 }, { 0x1D, 0x103F }, { 0x1E, 0x053C }, { 0x1F, 0x018C }, { 0x20, 0x0011 },
	{ 0x21, 0x0024 }, { 0x22, 0x04FB }, { 0x23, 0x0CE7 }, { 0x24, 0xFF80 }, { 0x25, 0x1033 }, { 0x26, 0x51E0 },
	{ 0x27, 0x0AC5 }, { 0x30, 0x0005 }, { 0x32, 0x0000 }, { 0x33, 0xF9B2 }, { 0x34, 0xE9F1 }, { 0x35, 0x0973 },
	{ 0x37, 0x0002 }, { 0x38, 0xFFE0 }, { 0x39, 0x001E }, { 0x41, 0x0037 }, { 0x44, 0x3D20 }, { 0x45, 0x1020 },
	{ 0x46, 0x2040 }, { 0x47, 0x0800 }, { 0x48, 0x0000 }, { 0x4A, 0x0814 }, { 0x4B, 0x0F20 }, { 0x4C, 0x0200 },
	{ 0x4D, 0x097F }, { 0x52, 0x1010 }, { 0x53, 0x1428 }, { 0x54, 0x8000 }, { 0x56, 0x0005 },
	{ REG_BANK, 0x0000 },
};

// The VCO's start-up and calibration
static const regval_t table_b[] = {
	{ REG_BANK, 0x0001 }, { 0x00, 0x81D2 }, { REG_BANK, 0x0000 }, { 0x0B, 0x1C34 }, { 0x30, 0x0000 }, { WAIT, 2 },
	{ 0x30, 0x0004 }, { 0x30, 0x0104 }, { 0x2F, 0xFFFF }, { 0x31, 0x07F9 }, { WAIT, 5 }, { 0x30, 0x0124 }, { WAIT, 5 },
	{ 0x30, 0x0126 }, { WAIT, 350 }, { REG_BANK, 0x0001 }, { 0x00, 0x01D2 }, { REG_BANK, 0x0000 },
};

static const regval_t table_c[] = {
	{ 0x30, 0x0006 }, { REG_BANK, 0x0001 }, { 0x4D, 0x007F }, { 0x49, 0x00FF }, { WAIT, 5 }, { REG_BANK, 0x0000 },
	{ 0x30, 0x0046 },
};

static const regval_t table_d[] = {
	{ REG_BANK, 0x0001 }, { 0x40, 0x8510 }, { 0x36, 0x0101 }, { REG_BANK, 0x0000 }, { 0x0B, 0x0C14 },
};

#define N(t)			(sizeof(t) / sizeof((t)[0]))

static void wr(at1141_t *c, uint8_t reg, uint16_t val)
{
	c->bus->write(reg, val);
	c->writes++;
}

static void table(at1141_t *c, const regval_t *t, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (t[i].reg == WAIT) {
			c->bus->delay_ms(t[i].val);
		} else {
			wr(c, t[i].reg, t[i].val);
		}
	}
}

void at1141_attach(at1141_t *c, const at1141_bus_t *bus)
{
	c->bus = bus;
	c->c15 = c->c16 = c->c19 = 0;
	c->lo_hz = 0;
	c->writes = 0;
}

uint16_t at1141_id(at1141_t *c)
{
	return c->bus->read(0x00);
}

// The output divider and multiplier for a frequency: each divider covers an octave, the upper part of it with x2,
// the lower with x3, so the VCO stays in its range
void at1141_plan(uint32_t lo_hz, uint16_t c19, at1141_plan_t *p)
{
	static const uint32_t edge[] = { 550400000u, 384000000u, 275200000u, 192000000u, 137600000u, 96000000u,
					 68800000u, 48000000u, 34400000u, 24000000u };
	uint8_t div = 64;
	bool mul = lo_hz < 17200000u;
	for (unsigned i = 0; i < N(edge); i++) {
		if (lo_hz >= edge[i]) {
			div = (uint8_t)(2u << (i / 2u));
			mul = (i & 1u) != 0;
			break;
		}
	}
	uint64_t vco = (uint64_t)div * lo_hz * (mul ? 3u : 2u);
	p->div = div;
	p->mul = mul;
	p->r3f = (uint16_t)(vco / MATCH_DIV);
	// vco / 2 / (reference, halved when 0x19 bit 6) in 6.26 fixed point
	uint32_t ref = REF_HZ * ((c19 & 0x40u) ? 2u : 1u);
	p->pll = (uint32_t)((vco << 25) / ref);
}

static void band(at1141_t *c, const at1141_plan_t *p)
{
	c->c15 = (uint16_t)((c->c15 & 0xFFFEu) | (p->mul ? 1u : 0u));
	c->c16 = (uint16_t)((c->c16 & 0xFF00u) | p->div);
	wr(c, REG_MUL, c->c15);
	wr(c, REG_DIV, c->c16);
}

void at1141_init(at1141_t *c)
{
	table(c, table_a, N(table_a));
	c->c15 = c->bus->read(REG_MUL);
	c->c16 = c->bus->read(REG_DIV);
	c->c19 = c->bus->read(REG_REF);
	at1141_plan_t p;
	at1141_plan(400000000u, c->c19, &p);
	band(c, &p);
	table(c, table_b, N(table_b));
	table(c, table_c, N(table_c));
	uint16_t r21 = c->bus->read(REG_21);
	wr(c, REG_21, (uint16_t)((r21 & ~(0x3Fu << 5)) | (10u << 5)));
	table(c, table_d, N(table_d));
	wr(c, REG_BANK, 0x0001);
	wr(c, 0x55, 0x1000);
	wr(c, REG_BANK, 0x0000);
	at1141_level(c, 2);
}

void at1141_enable(at1141_t *c)
{
	wr(c, REG_CTRL, CTRL_STANDBY);
	wr(c, REG_CTRL, CTRL_ON);
}

void at1141_disable(at1141_t *c)
{
	wr(c, REG_CTRL, CTRL_STANDBY);
}

void at1141_tune(at1141_t *c, uint32_t lo_hz)
{
	at1141_plan_t p;
	at1141_plan(lo_hz, c->c19, &p);
	band(c, &p);
	wr(c, REG_MATCH, p.r3f);
	wr(c, REG_PLL_LO, (uint16_t)p.pll);
	wr(c, REG_PLL_HI, (uint16_t)(p.pll >> 16));
	at1141_enable(c);
	c->lo_hz = lo_hz;
}

void at1141_off(at1141_t *c)
{
	wr(c, REG_CTRL, CTRL_STANDBY);
	wr(c, REG_CTRL, CTRL_OFF);
}

void at1141_restart(at1141_t *c)
{
	wr(c, REG_CTRL, CTRL_STANDBY);
	c->bus->delay_ms(RESTART_MS);
	at1141_enable(c);
}

// Level 0 is a value of its own; 1..7 move a single bit down from bit 15; 8 and above leave only the base bits
void at1141_level(at1141_t *c, uint8_t level)
{
	uint16_t v;
	if (level == 0) {
		v = 0x7F40u;
	} else if (level < 8) {
		v = (uint16_t)(((0x200u << (7u - level)) & 0xFE00u) | 0x0140u);
	} else {
		v = 0x0140u;
	}
	wr(c, REG_LEVEL, v);
}

void at1141_gpio(at1141_t *c, uint8_t pin, bool high)
{
	if (pin > 7) {
		return;
	}
	uint8_t reg = pin < 4 ? REG_GPIO_LO : REG_GPIO_HI;
	unsigned shift = (pin & 3u) * 4u;
	uint16_t v = c->bus->read(reg);
	v = (uint16_t)((v & ~(0xFu << shift)) | ((high ? 0xFu : 0u) << shift));
	wr(c, reg, v);
}
