// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT1141 LO driver (drivers/rf/at1141): the divider / multiplier table, the PLL word against a floating-point
// reference, the init order (bank restored, cache read, level 2), a tuning's write order, the level and GPIO
// encodings. The chip is a register file
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "rf/at1141/at1141.h"

static int failures, checks;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		checks++;                                                                                          \
		if (!(cond)) {                                                                                     \
			failures++;                                                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
		}                                                                                                  \
	} while (0)

// --- fake chip: two banks of 128 registers, a write log ---

static uint16_t regs[2][128];
static unsigned bank;
static struct {
	uint8_t reg;
	uint16_t val;
} log_[256];
static unsigned nlog;
static uint32_t waited_ms;

static void w(rfbus_t *b, uint8_t reg, uint16_t val)
{
	(void)b;
	if (nlog < sizeof log_ / sizeof log_[0]) {
		log_[nlog].reg = reg;
		log_[nlog].val = val;
	}
	nlog++;
	if (reg == 0x7F) {
		bank = val & 1u;
	} else {
		regs[bank][reg & 0x7F] = val;
	}
}

static uint16_t r(rfbus_t *b, uint8_t reg) { (void)b; return reg == 0 ? AT1141_ID : regs[bank][reg & 0x7F]; }
static void d(uint32_t ms) { waited_ms += ms; }

static rfbus_t bus = { .write = w, .read = r, .delay_ms = d };

static void reset(void)
{
	memset(regs, 0, sizeof regs);
	bank = 0;
	nlog = 0;
	waited_ms = 0;
}

// --- tests ---

static void test_plan(void)
{
	static const struct {
		uint32_t hz;
		uint8_t div;
		bool mul;
	} t[] = {
		{ 600000000u, 2, false }, { 550400000u, 2, false }, { 550399999u, 2, true }, { 491512500u, 2, true },
		{ 384000000u, 2, true }, { 383999999u, 4, false }, { 355000000u, 4, false }, { 275200000u, 4, false },
		{ 264000000u, 4, true }, { 192000000u, 4, true }, { 191999999u, 8, false }, { 137600000u, 8, false },
		{ 100000000u, 8, true }, { 70000000u, 16, false }, { 50000000u, 16, true }, { 40000000u, 32, false },
		{ 30000000u, 32, true }, { 20000000u, 64, false }, { 17000000u, 64, true },
	};
	for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
		at1141_plan_t p;
		at1141_plan(t[i].hz, 0, &p);
		CHECK(p.div == t[i].div && p.mul == t[i].mul);
		double vco = (double)t[i].div * t[i].hz * (t[i].mul ? 3 : 2);
		CHECK(p.r3f == (uint16_t)(uint32_t)(vco / 406250.0));
		double n = vco * 0.5 / 26e6 * 67108864.0;	// 6.26 fixed point
		CHECK(fabs((double)p.pll - floor(n)) <= 1.0);
		CHECK((uint64_t)p.pll < 64ull * 67108864u);
		at1141_plan_t h;
		at1141_plan(t[i].hz, 0x40, &h);			// reference halved
		CHECK(h.div == p.div && h.r3f == p.r3f && p.pll - 2u * h.pll <= 1u);
	}
	// VHF and UHF receive with a 45 MHz IF, either side: the VCO stays between 2.2 and 3.3 GHz
	for (uint32_t hz = 136000000u + 45000000u; hz <= 174000000u + 45000000u; hz += 1000000u) {
		at1141_plan_t p;
		at1141_plan(hz, 0, &p);
		double vco = (double)p.div * hz * (p.mul ? 3 : 2);
		CHECK(vco >= 2.2e9 && vco <= 3.31e9);
	}
	for (uint32_t hz = 400000000u - 45000000u; hz <= 520000000u + 45000000u; hz += 1000000u) {
		at1141_plan_t p;
		at1141_plan(hz, 0, &p);
		double vco = (double)p.div * hz * (p.mul ? 3 : 2);
		CHECK(vco >= 2.2e9 && vco <= 3.31e9);
	}
}

static void test_init(void)
{
	reset();
	at1141_t c;
	at1141_attach(&c, &bus);
	CHECK(at1141_id(&c) == AT1141_ID);
	at1141_init(&c);
	CHECK(log_[0].reg == 0x02 && log_[0].val == 0x0012);
	CHECK(bank == 0);					// every table returns to the first bank
	CHECK(waited_ms == 2 + 5 + 5 + 350 + 5);
	CHECK(log_[nlog - 1].reg == 0x26 && log_[nlog - 1].val == 0x4140);	// level 2 last
	CHECK((c.c15 & 0xFFFEu) == 0x020E && c.c19 == 0x88E1);	// read back after table A (bank 0), then the band
	CHECK((regs[0][0x15] & 1u) == 1u && (regs[0][0x16] & 0xFFu) == 2u);	// band for 400 MHz: d 2, x3
	CHECK(((regs[0][0x21] >> 5) & 0x3Fu) == 10u && (regs[0][0x21] & 0x1Fu) == (0x0C13u & 0x1Fu));
	CHECK(regs[1][0x55] == 0x1000 && regs[1][0x40] == 0x8510);
	CHECK(regs[0][0x30] == 0x0046);				// table C leaves it running
}

static void test_tune(void)
{
	reset();
	at1141_t c;
	at1141_attach(&c, &bus);
	at1141_init(&c);
	nlog = 0;
	at1141_tune(&c, 491512500u);
	static const uint8_t order[] = { 0x15, 0x16, 0x3F, 0x62, 0x63, 0x30, 0x30 };
	CHECK(nlog == sizeof order);
	for (unsigned i = 0; i < sizeof order && i < nlog; i++) {
		CHECK(log_[i].reg == order[i]);
	}
	at1141_plan_t p;
	at1141_plan(491512500u, c.c19, &p);
	CHECK(regs[0][0x62] == (uint16_t)p.pll && regs[0][0x63] == (uint16_t)(p.pll >> 16) && regs[0][0x3F] == p.r3f);
	CHECK(log_[5].val == 0x4006 && log_[6].val == 0x4046 && c.lo_hz == 491512500u);
	CHECK((regs[0][0x15] & 0xFFFEu) == (0x020Eu & 0xFFFEu));	// the rest of 0x15 kept

	at1141_tune(&c, 355000000u);			// VHF side: d 4, x2
	CHECK((regs[0][0x15] & 1u) == 0 && (regs[0][0x16] & 0xFFu) == 4u);

	nlog = 0;
	at1141_off(&c);
	CHECK(nlog == 2 && log_[0].val == 0x4006 && log_[1].val == 0x4002);
	nlog = 0;
	waited_ms = 0;
	at1141_restart(&c);
	CHECK(nlog == 3 && log_[0].val == 0x4006 && log_[2].val == 0x4046 && waited_ms == 10);
	nlog = 0;
	at1141_disable(&c);
	CHECK(nlog == 1 && regs[0][0x30] == 0x4006);
}

static void test_level_gpio(void)
{
	reset();
	at1141_t c;
	at1141_attach(&c, &bus);
	static const uint16_t lv[] = { 0x7F40, 0x8140, 0x4140, 0x2140, 0x1140, 0x0940, 0x0540, 0x0340, 0x0140, 0x0140 };
	for (uint8_t i = 0; i < sizeof lv / sizeof lv[0]; i++) {
		at1141_level(&c, i);
		CHECK(regs[0][0x26] == lv[i]);
	}
	at1141_level(&c, 15);
	CHECK(regs[0][0x26] == 0x0140);

	regs[0][0x2A] = 0x1234;
	at1141_gpio(&c, 4, true);
	CHECK(regs[0][0x2A] == 0x123F);
	at1141_gpio(&c, 5, false);
	CHECK(regs[0][0x2A] == 0x120F);
	at1141_gpio(&c, 1, true);
	CHECK(regs[0][0x2B] == 0x00F0);
	nlog = 0;
	at1141_gpio(&c, 8, true);			// no such pin
	CHECK(nlog == 0);
}

int main(void)
{
	test_plan();
	test_init();
	test_tune();
	test_level_gpio();
	printf("# at1141Test: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
