// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 RF: the synthesizer and IF receiver drivers (rf/pll1590, rf/lmx2571, rf/ak2365a, the SKY72310 programming
// sequence) and the board's revision and frequency plan (boards/md9600/rev_md9600.c, rfplan_md9600.c) against the
// words in docs/spec/hardware-md9600.md 5.3 / 5.4 and the LMX2571 and AK2365 data sheets. Built with every revision
// switched on, and once more with only V4A / V5 (md9600RfTestV5) to check what a single-revision image refuses.
#include <stdio.h>
#include <string.h>
#include "rf/sky72310/sky72310.h"
#include "rf/pll1590/pll1590.h"
#include "rf/lmx2571/lmx2571.h"
#include "rf/ak2365a/ak2365a.h"
#include "rfplan_md9600.h"
#include "config.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t absdiff(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

// --- A fake three-wire bus: every frame with its chip select and length ---

static struct {
	uint32_t word[64];
	uint8_t cs[64], bits[64];
	int n;
} bus_log;

static bool fake_write(void *ctx, uint8_t cs, uint32_t word, uint8_t bits)
{
	(void)ctx;
	if (bus_log.n < 64) {
		bus_log.word[bus_log.n] = word;
		bus_log.cs[bus_log.n] = cs;
		bus_log.bits[bus_log.n] = bits;
	}
	bus_log.n++;
	return true;
}

static const wire3_bus_t fake_bus = { fake_write, NULL };

#if CONFIG_MD9600_V4
static void test_pll1590(void)
{
	pll1590_ratio_t r;
	// 1.4 MHz steps (16.8 MHz / 12): 194.95 MHz = 139.25 steps, N 139 and a quarter
	CHECK(pll1590_ratio(16800000u, 12, 194950000u, &r));
	CHECK(r.n == 139);
	CHECK(r.frac == (1u << 16));
	CHECK(absdiff(pll1590_vco_hz(16800000u, 12, &r), 194950000u) <= 6u);
	// Above one half: N rounds up and the fraction is a negative offset (18-bit two's complement)
	CHECK(pll1590_ratio(16800000u, 12, 195650000u, &r));	// 139.75 steps
	CHECK(r.n == 140);
	CHECK(r.frac == (1u << 18) - (1u << 16));
	CHECK(absdiff(pll1590_vco_hz(16800000u, 12, &r), 195650000u) <= 6u);
	// Exactly on a step: no fraction
	CHECK(pll1590_ratio(16800000u, 8, 420000000u, &r));	// 2.1 MHz x 200
	CHECK(r.n == 200 && r.frac == 0);
	CHECK(!pll1590_ratio(16800000u, 0, 420000000u, &r));

	// Words: data on top, register in the low nibble, written 5, 6, 3, 4, 1, 2
	const pll1590_cfg_t vhf = { 16800000u, 12, 0x20000u, 0x1FF40u };
	uint32_t w[PLL1590_WORDS];
	CHECK(pll1590_words(&vhf, 194950000u, w) == PLL1590_WORDS);
	CHECK(w[0] == 0x000005u);
	CHECK(w[1] == 0x000006u);
	CHECK(w[2] == 0x0600C3u);
	CHECK(w[3] == 0x1FF404u);
	CHECK(w[4] == (((1u << 16) << 4) | 1u));
	CHECK(w[5] == (((0x20000u | 139u) << 4) | 2u));

	bus_log.n = 0;
	CHECK(pll1590_program(&fake_bus, 1, &vhf, 194950000u));
	CHECK(bus_log.n == 6);
	CHECK(bus_log.bits[0] == 24 && bus_log.cs[5] == 1 && bus_log.word[5] == w[5]);
}
#endif

#if CONFIG_MD9600_V5
static void test_lmx2571(void)
{
	// Data sheet 7.5: R/W 0, 7-bit address, 16 data bits
	CHECK(lmx2571_word(0x2A, 0x0108) == 0x2A0108u);
	CHECK(lmx2571_word(0x00, 0x2000) == 0x002000u);

	lmx2571_ratio_t r;
	// 4.2 MHz (16.8 / 4): 390.05 MHz = 92.869... steps
	CHECK(lmx2571_ratio(16800000u, 4, 390050000u, &r));
	CHECK(r.n == 92);
	CHECK(absdiff(lmx2571_vco_hz(16800000u, 4, &r), 390050000u) <= 1u);
	CHECK(!lmx2571_ratio(16800000u, 0, 390050000u, &r));
	CHECK(!lmx2571_ratio(1000000u, 1, 1100000000u, &r));	// N 1100 > 1023

	const lmx2571_cfg_t c = { 16800000u, 4, 3, 0x003E, 0x8581, 0x1024, 0x0430, 0x0230, 0x0108 };
	uint32_t w[LMX2571_WORDS];
	CHECK(lmx2571_words(&c, 390050000u, w) == LMX2571_WORDS);
	CHECK(w[0] == 0x002000u);			// reset
	CHECK(w[1] == 0x00003Eu);
	CHECK(w[2] == (0x040000u | 0x3000u | 92u));	// third order, N
	CHECK(w[3] == (0x010000u | (r.num >> 16)));	// DEN high byte 0
	CHECK(w[4] == (0x020000u | (r.num & 0xFFFFu)));
	CHECK(w[5] == 0x030000u);
	CHECK(w[6] == 0x050104u);			// PLL_R 1, pre-divider 4
	CHECK(w[7] == 0x068581u && w[8] == 0x071024u && w[9] == 0x080430u);
	CHECK(w[10] == 0x290230u && w[11] == 0x2A0108u);
	CHECK(w[12] == 0x00003Fu && w[13] == 0x00003Eu);	// calibrate, then normal

	bus_log.n = 0;
	CHECK(lmx2571_program(&fake_bus, 0, &c, 390050000u));
	CHECK(bus_log.n == (int)LMX2571_WORDS && bus_log.bits[0] == 24);
}
#endif

#if CONFIG_MD9600_V1V3 || CONFIG_MD9600_V4
static void test_ak2365a(void)
{
	CHECK(AK2365A_CTRL1(7, true, 0, 1) == 0xF1);
	CHECK(AK2365A_CTRL1(7, false, 2, 1) == 0xE9);
	// The board's set-up, 25 and 12.5 kHz (hardware-md9600.md 5.4)
	uint16_t w[MD9600_IF_WORDS];
	CHECK(md9600_if_words(true, w) == 6);
	CHECK(w[0] == 0x08AA && w[1] == 0x02F1 && w[2] == 0x041D && w[3] == 0x0600 && w[4] == 0x1601 && w[5] == 0x1880);
	CHECK(md9600_if_words(false, w) == 6);
	CHECK(w[1] == 0x02E9);

	const ak2365a_cfg_t c = { 0xF1, 0xE9, 0x1D, 0, 1, 0x80 };
	bus_log.n = 0;
	CHECK(ak2365a_setup(&fake_bus, 1, &c, false));
	CHECK(bus_log.n == 6 && bus_log.bits[0] == 16 && bus_log.cs[0] == 1 && bus_log.word[1] == 0x02E9);
}
#endif

#if CONFIG_MD9600_V1V3
static void test_sky72310_sequence(void)
{
	uint16_t w[SKY72310_PROGRAM_WORDS];
	// UHF receive of 446.0 MHz: LO 396.05 MHz at 4.2 MHz steps = 94.2976..., N_reg 62
	CHECK(sky72310_program_words(16800000u, 4, 396050000u, 31, 0x3D0, w) == 8);
	CHECK(w[0] == 0x8000 && w[1] == 0x9000);
	CHECK(w[2] == 0x5003);			// R - 1
	CHECK(w[3] == 62);			// N - 32
	CHECK((w[4] & 0xF000u) == 0x2000 && (w[5] & 0xF000u) == 0x1000);
	CHECK(w[6] == 0x601F && w[7] == 0x73D0);
	sky72310_ratio_t r = { (uint16_t)(w[3] & 0x1FFu), ((uint32_t)(w[5] & 0x3FFu) << 8) | (w[4] & 0xFFu) };
	CHECK(absdiff(sky72310_vco_hz(16800000u, 4, &r), 396050000u) <= 17u);
	CHECK(sky72310_program_words(16800000u, 0, 396050000u, 31, 0x3D0, w) == 0);
}
#endif

static void test_bands(void)
{
	uint8_t b;
	CHECK(md9600_band_of(136000000u, &b) && b == MD9600_VHF);
	CHECK(md9600_band_of(174000000u, &b) && b == MD9600_VHF);
	CHECK(md9600_band_of(400000000u, &b) && b == MD9600_UHF);
	CHECK(md9600_band_of(480000000u, &b) && b == MD9600_UHF);
	CHECK(!md9600_band_of(135999999u, &b));
	CHECK(!md9600_band_of(300000000u, &b));
	CHECK(!md9600_band_of(480000001u, &b));
	CHECK(md9600_if_lines_v5(MD9600_VHF, true) == 0x07);
	CHECK(md9600_if_lines_v5(MD9600_UHF, false) == 0x08);
}

static void test_plan(void)
{
	md9600_pll_plan_t p;
	CHECK(!md9600_pll_plan(MD9600_REV_UNKNOWN, 446000000u, false, &p));
#if CONFIG_MD9600_V1V3
	// V1-V3, VHF receive: high-side LO and the VCO at twice it
	CHECK(md9600_pll_plan(MD9600_REV_V1V3, 145000000u, false, &p));
	CHECK(p.band == MD9600_VHF && p.lo_hz == 194950000u && p.vco_hz == 389900000u && p.bits == 16 && p.count == 8);
	CHECK(p.words[2] == 0x5005);		// R 6
	// UHF transmit on the carrier, R 4
	CHECK(md9600_pll_plan(MD9600_REV_V1V3, 446000000u, true, &p));
	CHECK(p.band == MD9600_UHF && p.lo_hz == 446000000u && p.vco_hz == 446000000u && p.words[2] == 0x5003);
#endif
#if CONFIG_MD9600_V4
	// V4: no doubling; UHF receive R 8 with 0x1FF40, transmit R 4 with 0x19F40
	CHECK(md9600_pll_plan(MD9600_REV_V4, 145000000u, false, &p));
	CHECK(p.vco_hz == 194950000u && p.bits == 24 && p.count == 6 && p.words[2] == 0x0600C3u && p.words[3] == 0x1FF404u);
	CHECK(md9600_pll_plan(MD9600_REV_V4, 446000000u, false, &p));
	CHECK(p.lo_hz == 396050000u && p.words[2] == 0x060083u && p.words[3] == 0x1FF404u);
	CHECK((p.words[5] >> 4) == (0x38000u | 189u));		// 396.05 / 2.1 = 188.6: N 189, fraction negative
	CHECK(md9600_pll_plan(MD9600_REV_V4, 446000000u, true, &p));
	CHECK(p.words[2] == 0x060043u && p.words[3] == 0x19F404u);
	CHECK(md9600_pll_plan(MD9600_REV_V4, 174000000u, true, &p));
	CHECK(p.words[3] == 0x1FF404u && (p.words[5] >> 4) == (0x20000u | 124u));	// 174 / 1.4 = 124.29
#endif
#if CONFIG_MD9600_V5
	CHECK(md9600_pll_plan(MD9600_REV_V5, 145000000u, false, &p));
	CHECK(p.vco_hz == 194950000u && p.count == LMX2571_WORDS);
	CHECK(p.words[6] == 0x050108u);		// pre-divider 8 on VHF
	CHECK((p.words[2] & 0xFFFu) == 92u);	// 194.95 / 2.1 = 92.83
	CHECK(md9600_pll_plan(MD9600_REV_V5, 440000000u, true, &p));
	CHECK(p.words[6] == 0x050104u && (p.words[2] & 0xFFFu) == 104u);	// 440 / 4.2 = 104.76
#endif
	CHECK(!md9600_pll_plan(MD9600_REV_V5, 300000000u, false, &p));
	CHECK(!md9600_pll_plan(MD9600_REV_COUNT, 446000000u, false, &p));
}

static void test_record(void)
{
	uint8_t rec[MD9600_REV_RECORD_SIZE];
	for (int r = MD9600_REV_V1V3; r < MD9600_REV_COUNT; r++) {
		md9600_rev_record_encode((md9600_rev_t)r, rec);
		CHECK((int)md9600_rev_record_decode(rec) == r);
	}
	md9600_rev_record_encode(MD9600_REV_V4, rec);
	rec[6] ^= 1;					// a flipped bit
	CHECK(md9600_rev_record_decode(rec) == MD9600_REV_UNKNOWN);
	memset(rec, 0xFF, sizeof(rec));			// erased flash
	CHECK(md9600_rev_record_decode(rec) == MD9600_REV_UNKNOWN);
	md9600_rev_record_encode((md9600_rev_t)7, rec);	// no such revision
	CHECK(md9600_rev_record_decode(rec) == MD9600_REV_UNKNOWN);
	md9600_rev_record_encode(MD9600_REV_UNKNOWN, rec);
	CHECK(md9600_rev_record_decode(rec) == MD9600_REV_UNKNOWN);
}

static void test_resolve(void)
{
	md9600_rev_state_t s;
	// The stored record wins and is the only source that allows transmit
	s = md9600_rev_resolve(MD9600_REV_V5, MD9600_REV_UNKNOWN, MD9600_REV_V5);
	CHECK(s.rev == MD9600_REV_V5 && s.source == MD9600_REV_SRC_STORED && s.usable && s.confirmed);
	s = md9600_rev_resolve(MD9600_REV_UNKNOWN, MD9600_REV_UNKNOWN, MD9600_REV_V5);
	CHECK(s.rev == MD9600_REV_V5 && s.source == MD9600_REV_SRC_DEFAULT && s.usable && !s.confirmed);
#if CONFIG_MD9600_V4
	s = md9600_rev_resolve(MD9600_REV_UNKNOWN, MD9600_REV_V4, MD9600_REV_V5);
	CHECK(s.rev == MD9600_REV_V4 && s.source == MD9600_REV_SRC_PROBE && s.usable && !s.confirmed);
	s = md9600_rev_resolve(MD9600_REV_V4, MD9600_REV_V5, MD9600_REV_V5);
	CHECK(s.rev == MD9600_REV_V4 && s.source == MD9600_REV_SRC_STORED);
#else
	// A stored revision this image can't drive: kept (the receiver stays off), not replaced by the default
	s = md9600_rev_resolve(MD9600_REV_V4, MD9600_REV_UNKNOWN, MD9600_REV_V5);
	CHECK(s.rev == MD9600_REV_V4 && s.source == MD9600_REV_SRC_STORED && !s.usable && s.confirmed);
	// A probe result for an unbuilt revision is ignored; a default that isn't built falls to one that is
	s = md9600_rev_resolve(MD9600_REV_UNKNOWN, MD9600_REV_V4, MD9600_REV_V1V3);
	CHECK(s.rev == MD9600_REV_V5 && s.source == MD9600_REV_SRC_DEFAULT && s.usable);
#endif
}

static void test_probe_pick(void)
{
	const md9600_rev_t cand[3] = { MD9600_REV_V5, MD9600_REV_V4, MD9600_REV_V1V3 };
	const uint16_t clear[3] = { 2010, 1400, 1995 };		// V4 moved the noise reading by 600
	CHECK(md9600_rev_probe_pick(2000, clear, cand, 3, 200) == MD9600_REV_V4);
	const uint16_t weak[3] = { 2010, 1900, 1995 };		// below the threshold
	CHECK(md9600_rev_probe_pick(2000, weak, cand, 3, 200) == MD9600_REV_UNKNOWN);
	const uint16_t two[3] = { 2500, 1400, 2000 };		// two candidates moved it: no verdict
	CHECK(md9600_rev_probe_pick(2000, two, cand, 3, 200) == MD9600_REV_UNKNOWN);
	CHECK(md9600_rev_probe_pick(2000, clear, cand, 0, 200) == MD9600_REV_UNKNOWN);
}

int main(void)
{
#if CONFIG_MD9600_V4
	test_pll1590();
#endif
#if CONFIG_MD9600_V5
	test_lmx2571();
#endif
#if CONFIG_MD9600_V1V3 || CONFIG_MD9600_V4
	test_ak2365a();
#endif
#if CONFIG_MD9600_V1V3
	test_sky72310_sequence();
#endif
	test_bands();
	test_plan();
	test_record();
	test_resolve();
	test_probe_pick();
	printf("md9600RfTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
