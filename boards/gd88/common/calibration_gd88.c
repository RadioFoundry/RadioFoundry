// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-88 factory calibration, the "TUNE" page (docs/spec/hardware-gd88.md 7): PA drive, RX front-end tuning, TX center
// and modulation, TX switching delays, squelch thresholds. Each SoC has its own page, in the DB25-D's layout.
//
// Two copies exist, one in NAND and one in program flash. The higher save counter wins, and nothing is ever written
// back. The chosen page is copied into RAM, so the lookups below cost no flash access. Only the
// NAND copy's header is read before the choice.
#include <string.h>
#include "gd88_cal.h"
#include "radio/cal_curve.h"

#define OFF_RX_TUNE		0x02Cu
#define OFF_TX_CENTER		0x43Fu		// VHF half only, used for both bands
#define OFF_PA_HIGH		0x045u		// power level 1 of the factory set-up
#define OFF_PA_LOW		0x056u		// every other level
#define OFF_MOD			0x067u		// (u8, u8) pairs, the first byte used
#define OFF_TX_DELAY		0x0AEu		// (u16 on, u16 off) pairs in us
#define OFF_SQUELCH		0x0CFu		// 8 segments x 9 levels x u16
#define HALF			0x400u		// the VHF half
#define HEADER_LEN		8u		// "TUNE" and the save counter

#define VHF_BELOW_HZ		280000000u
#define TX_DELAY_MAX_US		2000u		// a larger value counts as 0
#define DAC_MAX			4095u
#define SQUELCH_LEVEL0_DB	136u

static const uint32_t points_uhf[GD88_CAL_POINTS] = {
	403175000, 414175000, 425175000, 436625000, 447775000, 458775000, 469825000, 478825000,
};
static const uint32_t points_vhf[GD88_CAL_POINTS] = {
	136125000, 142125000, 146125000, 152125000, 156125000, 162125000, 166125000, 173125000,
};
// Upper edges of squelch segments 1..7; segment 8 is everything above
static const uint32_t segments_uhf[GD88_CAL_POINTS - 1] = {
	410000000, 420000000, 430000000, 440000000, 450000000, 460000000, 470000000,
};
static const uint32_t segments_vhf[GD88_CAL_POINTS - 1] = {
	142000000, 146000000, 152000000, 156000000, 162000000, 166000000, 173000000,
};

static uint8_t page[GD88_CAL_LEN];
static bool valid;
static gd88_cal_source_t source;
static uint32_t counter;

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool tagged(const uint8_t *p)
{
	return p != NULL && memcmp(p, "TUNE", 4) == 0;
}

gd88_cal_source_t gd88_cal_choose(const uint8_t *flash_copy, const uint8_t *nand_copy)
{
	bool f = tagged(flash_copy), n = tagged(nand_copy);
	if (f && n) {
		// Equal counters mean the same save, so either copy will do
		return rd32(flash_copy + 4) > rd32(nand_copy + 4) ? GD88_CAL_FLASH : GD88_CAL_NAND;
	}
	return f ? GD88_CAL_FLASH : n ? GD88_CAL_NAND : GD88_CAL_NONE;
}

bool gd88_cal_load(const uint8_t *flash_copy, gd88_cal_read_fn nand_read)
{
	uint8_t header[HEADER_LEN];
	bool nand = nand_read != NULL && nand_read(GD88_CAL_NAND_ADDR, header, sizeof(header));
	source = gd88_cal_choose(flash_copy, nand ? header : NULL);
	// The NAND page goes straight into RAM; if it can't be read whole, the flash copy if there is one
	if (source == GD88_CAL_NAND && !(nand_read(GD88_CAL_NAND_ADDR, page, GD88_CAL_LEN) && tagged(page))) {
		source = tagged(flash_copy) ? GD88_CAL_FLASH : GD88_CAL_NONE;
	}
	if (source == GD88_CAL_FLASH) {
		memcpy(page, flash_copy, GD88_CAL_LEN);
	}
	valid = source != GD88_CAL_NONE;
	counter = valid ? rd32(page + 4) : 0;
	return valid;
}

bool gd88_cal_valid(void)
{
	return valid;
}

gd88_cal_source_t gd88_cal_source(void)
{
	return source;
}

uint32_t gd88_cal_counter(void)
{
	return counter;
}

static bool is_vhf(uint32_t hz)
{
	return hz < VHF_BELOW_HZ;
}

// The page's half for hz plus off
static const uint8_t *at(uint32_t hz, uint32_t off)
{
	return page + off + (is_vhf(hz) ? HALF : 0);
}

// Value at point p (0-based) of a per-point table whose entries are size bytes (the first u16 of each entry)
static int32_t entry(const uint8_t *table, uint8_t p, uint8_t size, bool byte)
{
	const uint8_t *e = table + 1 + (uint32_t)size * p;
	return byte ? e[0] : rd16(e);
}

// Flat at and below point 1, linear between the two points around hz, rounded half up; point 8 above point 8
static int32_t interpolate(uint32_t hz, const uint8_t *table, uint8_t size, bool byte)
{
	int32_t v[GD88_CAL_POINTS];
	for (uint8_t p = 0; p < GD88_CAL_POINTS; p++) {
		v[p] = entry(table, p, size, byte);
	}
	return cal_curve_at(is_vhf(hz) ? points_vhf : points_uhf, v, GD88_CAL_POINTS, hz, CAL_CURVE_ROUND);
}

uint16_t gd88_cal_pa(uint32_t hz, bool high)
{
	if (!valid) {
		return 0;
	}
	int32_t v = interpolate(hz, at(hz, high ? OFF_PA_HIGH : OFF_PA_LOW), 2, false);
	// 0 or an erased 0xFFFF is no calibration; anything above the DAC's range isn't one either
	return v > 0 && v <= (int32_t)DAC_MAX ? (uint16_t)v : 0;
}

bool gd88_cal_tx_plan(uint32_t hz, bool high, uint8_t drive_pct, gd88_tx_plan_t *p)
{
	uint16_t pa = gd88_cal_pa(hz, high);
	if (pa == 0 || drive_pct == 0 || drive_pct > 100) {
		return false;
	}
	uint32_t scaled = ((uint32_t)pa * drive_pct + 50u) / 100u;
	p->pa = (uint16_t)(scaled > 0 ? scaled : 1u);
	gd88_cal_tx_delays(hz, &p->on_us, &p->off_us);
	return true;
}

uint16_t gd88_apc_level(uint16_t target, uint16_t step)
{
	if (step >= GD88_APC_STEPS) {
		return target;
	}
	// Smoothstep 3x^2 - 2x^3 of x = step / STEPS: no jump at either end
	uint64_t n = GD88_APC_STEPS;
	uint64_t x = step;
	return (uint16_t)((uint64_t)target * x * x * (3u * n - 2u * x) / (n * n * n));
}

uint16_t gd88_cal_rx_tune(uint32_t hz)
{
	if (!valid) {
		return 0;
	}
	int32_t v = interpolate(hz, at(hz, OFF_RX_TUNE), 2, false);
	return v > 0 && v <= (int32_t)DAC_MAX ? (uint16_t)v : 0;
}

uint8_t gd88_cal_tx_center(void)
{
#ifdef GD88_TX_CENTER_OVERRIDE
	return GD88_TX_CENTER_OVERRIDE;	// bench sweep of the reference trim: make GD88_TX_CENTER=116 (config.mk)
#else
	return valid ? page[OFF_TX_CENTER] : 0x80u;
#endif
}

uint8_t gd88_cal_mod(uint32_t hz)
{
	return valid ? (uint8_t)interpolate(hz, at(hz, OFF_MOD), 2, true) : 0;
}

void gd88_cal_tx_delays(uint32_t hz, uint16_t *on_us, uint16_t *off_us)
{
	int32_t on = 0, off = 0;
	if (valid) {
		on = interpolate(hz, at(hz, OFF_TX_DELAY), 4, false);
		off = interpolate(hz, at(hz, OFF_TX_DELAY) + 2, 4, false);
	}
	*on_us = on > 0 && on <= (int32_t)TX_DELAY_MAX_US ? (uint16_t)on : 0;
	*off_us = off > 0 && off <= (int32_t)TX_DELAY_MAX_US ? (uint16_t)off : 0;
}

uint8_t gd88_cal_squelch_db(uint32_t hz, uint8_t level)
{
	if (!valid || level == 0 || level > 9) {
		return SQUELCH_LEVEL0_DB;
	}
	const uint32_t *edge = is_vhf(hz) ? segments_vhf : segments_uhf;
	uint8_t seg = 0;
	while (seg < GD88_CAL_POINTS - 1 && hz >= edge[seg]) {
		seg++;
	}
	return at(hz, OFF_SQUELCH)[1 + 18u * seg + 2u * (level - 1u)];
}
