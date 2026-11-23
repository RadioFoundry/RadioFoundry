// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Superhet chip detection (drivers/rf/superhet/superhet_chips): every main / LO pair a radio may have is found, set up
// and bound to its driver's ops; a part not driven leaves both chips alone; a chip that answers only after the soft
// reset is found; the sub-audio check goes to the right source. Each chip is a register file with a write log
#include <stdio.h>
#include <string.h>
#include "rf/superhet/superhet_chips.h"
#include "rf/bk4819/bk4819_superhet.h"
#include "rf/fd6818/fd6818sh_superhet.h"
#include "rf/at1141/at1141_superhet.h"

static int failures, checks;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		checks++;                                                                                          \
		if (!(cond)) {                                                                                     \
			failures++;                                                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
		}                                                                                                  \
	} while (0)

// --- fake chip: register 0 answers its ID once it has had resets_needed soft resets ---

typedef struct {
	rfbus_t bus;		// first: the driver gets &x.bus
	uint16_t regs[256];
	uint16_t id;
	unsigned resets_needed;
	unsigned writes, resets;
} chip_t;

static void w(rfbus_t *b, uint8_t reg, uint16_t val)
{
	chip_t *c = (chip_t *)b;
	c->writes++;
	if (reg == 0x00 && val == 0x8000u) {
		c->resets++;
		if (c->resets_needed > 0) {
			c->resets_needed--;
		}
	}
	c->regs[reg] = val;
}

static uint16_t r(rfbus_t *b, uint8_t reg)
{
	chip_t *c = (chip_t *)b;
	if (reg == 0x00) {
		return c->resets_needed == 0 ? c->id : 0x0000u;
	}
	return c->regs[reg];
}

static chip_t main_chip, lo_chip;

static void reset(uint16_t main_id, uint16_t lo_id)
{
	memset(&main_chip, 0, sizeof main_chip);
	memset(&lo_chip, 0, sizeof lo_chip);
	main_chip.bus = (rfbus_t){ .write = w, .read = r };
	lo_chip.bus = (rfbus_t){ .write = w, .read = r };
	main_chip.id = main_id;
	lo_chip.id = lo_id;
}

static bool css;
static bool css_line(void) { return css; }

static const superhet_chips_cfg_t cfg = {
	.bus_main = &main_chip.bus, .bus_lo = &lo_chip.bus, .css_line = css_line, .at1141_restart = true,
};

static const superhet_chips_cfg_t cfg_gated = {
	.bus_main = &main_chip.bus, .bus_lo = &lo_chip.bus, .css_line = NULL, .at1141_restart = false,
};

// --- tests ---

static void test_kind(void)
{
	CHECK(superhet_chip_kind(0x4819, false) == SUPERHET_CHIP_BK4819);
	CHECK(superhet_chip_kind(0x6818, false) == SUPERHET_CHIP_FD6818);
	CHECK(superhet_chip_kind(0x7813, false) == SUPERHET_CHIP_7813);
	CHECK(superhet_chip_kind(0x1100, false) == SUPERHET_CHIP_NONE);	// a PLL is never the transceiver
	CHECK(superhet_chip_kind(0x1846, false) == SUPERHET_CHIP_NONE);
	CHECK(superhet_chip_kind(0x4819, true) == SUPERHET_CHIP_BK4819);
	CHECK(superhet_chip_kind(0x6818, true) == SUPERHET_CHIP_FD6818);
	CHECK(superhet_chip_kind(0x7813, true) == SUPERHET_CHIP_7813);
	CHECK(superhet_chip_kind(0x1100, true) == SUPERHET_CHIP_AT1141);
	CHECK(superhet_chip_kind(0x0000, true) == SUPERHET_CHIP_NONE);
	CHECK(superhet_chip_letter(SUPERHET_CHIP_7813) == '4' && superhet_chip_letter(SUPERHET_CHIP_NONE) == '?');
	CHECK(strcmp(superhet_chip_name(SUPERHET_CHIP_AT1141), "AT1141") == 0);
}

// Every pair: found, both chips written, each bound to its driver's ops and device
static void test_pairs(void)
{
	static const uint16_t mains[] = { 0x4819, 0x6818, 0x7813 };
	static const uint16_t los[] = { 0x4819, 0x6818, 0x7813, 0x1100 };
	for (unsigned i = 0; i < sizeof mains / sizeof mains[0]; i++) {
		for (unsigned j = 0; j < sizeof los / sizeof los[0]; j++) {
			superhet_chips_t c;
			reset(mains[i], los[j]);
			CHECK(superhet_chips_init(&c, &cfg));
			CHECK(c.main_id == mains[i] && c.lo_id == los[j]);
			CHECK(main_chip.writes > 0 && lo_chip.writes > 0);
			if (mains[i] == 0x4819) {
				CHECK(c.main_kind == SUPERHET_CHIP_BK4819);
				CHECK(c.main.ops == &bk4819_superhet_main && c.main.chip == &c.m.bk);
				CHECK(main_chip.regs[BK4819_REG_PA] == BK4819_PA_MAX);
			} else {
				// The 0x7813 part as the main chip takes the FD6818's main set
				CHECK(c.main_kind == (mains[i] == 0x6818 ? SUPERHET_CHIP_FD6818 : SUPERHET_CHIP_7813));
				CHECK(c.main.ops == &fd6818sh_superhet_main && c.main.chip == &c.m.fd);
				CHECK(c.m.fd.role == FD6818SH_MAIN);
			}
			switch (los[j]) {
			case 0x4819:
				CHECK(c.lo_kind == SUPERHET_CHIP_BK4819);
				CHECK(c.lo.ops == &bk4819_superhet_lo && c.lo.chip == &c.lo_dev.bk);
				break;
			case 0x1100:
				CHECK(c.lo_kind == SUPERHET_CHIP_AT1141);
				CHECK(c.lo.ops == &at1141_superhet_lo_restart && c.lo.chip == &c.lo_dev.at);
				break;
			default:
				CHECK(c.lo.ops == &fd6818sh_superhet_lo && c.lo.chip == &c.lo_dev.fd);
				CHECK(c.lo_dev.fd.role == (los[j] == 0x7813 ? FD6818SH_LO_7813 : FD6818SH_LO));
				break;
			}
			// The bound ops work on the device: the main chip idles and receives, the LO tunes
			c.main.ops->idle(c.main.chip);
			c.main.ops->to_if(c.main.chip, 45000000u);
			c.main.ops->rx(c.main.chip);
			unsigned before = lo_chip.writes;
			c.lo.ops->tune(c.lo.chip, 491000000u);
			CHECK(lo_chip.writes > before);
		}
	}
}

// The AT1141 gated rather than restarted, by the board's choice
static void test_at1141_gated(void)
{
	superhet_chips_t c;
	reset(0x6818, 0x1100);
	CHECK(superhet_chips_init(&c, &cfg_gated));
	CHECK(c.lo.ops == &at1141_superhet_lo);
}

// A part not driven: false, ops left NULL, and nothing written but the soft resets of the detection
static void test_unsupported(void)
{
	static const uint16_t pairs[][2] = { { 0x1100, 0x4819 }, { 0x1846, 0x1100 }, { 0x4819, 0x1846 }, { 0, 0 } };
	for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
		superhet_chips_t c;
		reset(pairs[i][0], pairs[i][1]);
		CHECK(!superhet_chips_init(&c, &cfg));
		CHECK(c.main.ops == NULL && c.lo.ops == NULL);
		CHECK(c.main_id == pairs[i][0] && c.lo_id == pairs[i][1]);
		// Per try: page 0, reset on, reset off
		CHECK(main_chip.writes == main_chip.resets * 3u);
		CHECK(lo_chip.writes == lo_chip.resets * 3u);
		CHECK(c.main_kind == SUPERHET_CHIP_NONE || c.lo_kind == SUPERHET_CHIP_NONE);
		// 3 reads, a reset between them; an unknown main chip leaves the LO read once, never reset
		CHECK(main_chip.resets == (c.main_kind == SUPERHET_CHIP_NONE ? 2u : 0u));
		CHECK(lo_chip.resets == (c.main_kind != SUPERHET_CHIP_NONE ? 2u : 0u));
	}
}

// A chip that reads 0 until it has been reset is found on a later try
static void test_answers_after_reset(void)
{
	superhet_chips_t c;
	reset(0x6818, 0x4819);
	main_chip.resets_needed = 2;
	CHECK(superhet_chips_init(&c, &cfg));
	CHECK(c.main_kind == SUPERHET_CHIP_FD6818 && main_chip.resets >= 2);
	reset(0x4819, 0x1100);
	main_chip.resets_needed = 5;	// more than the tries
	CHECK(!superhet_chips_init(&c, &cfg));
	CHECK(c.main_id == 0 && main_chip.resets == 2);	// no reset after the last read
}

// The sub-audio check: none is always heard; a BK4819 by its status bits (DCS turned over with an inverted IF), an
// FD6818 by the board's line, never heard without one
static void test_tone_heard(void)
{
	superhet_chips_t c;
	reset(0x4819, 0x4819);
	CHECK(superhet_chips_init(&c, &cfg));
	CHECK(superhet_chips_tone_heard(&c, 0, SUPERHET_TONE_NONE));
	CHECK(superhet_chips_tone_heard(&c, 1u << 10, SUPERHET_TONE_CTCSS));
	CHECK(!superhet_chips_tone_heard(&c, 0, SUPERHET_TONE_CTCSS));
	superhet_chips_rx_inverted(&c, false);
	CHECK(superhet_chips_tone_heard(&c, 1u << 14, SUPERHET_TONE_DCS));
	superhet_chips_rx_inverted(&c, true);
	CHECK(c.m.bk.rx_inverted);
	CHECK(superhet_chips_tone_heard(&c, 1u << 15, SUPERHET_TONE_DCS));
	CHECK(!superhet_chips_tone_heard(&c, 1u << 14, SUPERHET_TONE_DCS));

	reset(0x6818, 0x1100);
	CHECK(superhet_chips_init(&c, &cfg));
	superhet_chips_rx_inverted(&c, true);
	CHECK(c.m.fd.rx_inverted);
	css = false;
	CHECK(!superhet_chips_tone_heard(&c, 0xFFFF, SUPERHET_TONE_CTCSS));
	css = true;
	CHECK(superhet_chips_tone_heard(&c, 0, SUPERHET_TONE_DCS));
	CHECK(superhet_chips_tone_heard(&c, 0, SUPERHET_TONE_NONE));

	reset(0x6818, 0x1100);
	CHECK(superhet_chips_init(&c, &cfg_gated));
	CHECK(!superhet_chips_tone_heard(&c, 0xFFFF, SUPERHET_TONE_CTCSS));
	CHECK(superhet_chips_tone_heard(&c, 0, SUPERHET_TONE_NONE));
}

// Status from the main chip's registers, the GPIO word from its driver's copy
static void test_status_gpio(void)
{
	superhet_chips_t c;
	reset(0x6818, 0x4819);
	CHECK(superhet_chips_init(&c, &cfg));
	main_chip.regs[0x67] = 0x0123;
	main_chip.regs[0x65] = 0x0045;
	main_chip.regs[0x0C] = 0x4000;
	lo_chip.regs[0x67] = 0x0FFF;
	uint16_t r67, r65, r0c;
	superhet_chips_status(&c, &r67, &r65, &r0c);
	CHECK(r67 == 0x0123 && r65 == 0x0045 && r0c == 0x4000);
	r0c = 0xBEEF;
	superhet_chips_status(&c, &r67, &r65, NULL);	// 0x0C not wanted: not read
	CHECK(r67 == 0x0123 && r0c == 0xBEEF);
	// A pin the init gave no function is made a plain output (its select nibble 0) on the first drive
	main_chip.regs[0x27] = 0x00F0;
	superhet_main_gpio(&c.main, 5, true);
	CHECK(superhet_chips_main_gpio_word(&c) == c.m.fd.r25 && (c.m.fd.r25 & (1u << 5)) != 0);
	CHECK((main_chip.regs[0x27] & 0x00F0u) == 0);
	superhet_main_gpio(&c.main, 0xFF, true);	// a pin not wired: nothing
	CHECK(superhet_chips_main_gpio_word(&c) == c.m.fd.r25);

	reset(0x4819, 0x4819);
	CHECK(superhet_chips_init(&c, &cfg));
	superhet_main_gpio(&c.main, 1, true);
	CHECK(superhet_chips_main_gpio_word(&c) == c.m.bk.gpio && (c.m.bk.gpio & (0x40u >> 1)) != 0);
}

int main(void)
{
	test_kind();
	test_pairs();
	test_at1141_gated();
	test_unsupported();
	test_answers_after_reset();
	test_tone_heard();
	test_status_gpio();
	printf("superhetChipsTest: %d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
