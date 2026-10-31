// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV: frequency plans the stock firmware computes outside the RF chips: the air band LO (docs/spec/
// hardware-d578uv.md 5.4) and the PA5 device's trims from the calibration (5.5). Pure, so the host tests check them;
// radio_d578uv.c applies them.
#ifndef D578UV_RF_PLAN_H
#define D578UV_RF_PLAN_H

#include <stdbool.h>
#include <stdint.h>

// --- Air band: chip 3's LO for the KTMicro receiver's 29.25 MHz IF ---
// The stock "AM Offset" setting puts the LO 29.25 MHz below the signal (its default) or above it. Below keeps the LO
// (78.75-107.75 MHz for 108-137 MHz) clear of the air band itself. The VCO sub-band by the LO: 3 at or above 140 MHz,
// 4 at or above 85 MHz, else 5
#define AM_IF_HZ		29250000u

static inline uint32_t am_lo_hz(uint32_t hz, bool below)
{
	return below ? hz - AM_IF_HZ : hz + AM_IF_HZ;
}

static inline uint8_t am_lo_subband(uint32_t lo_hz)
{
	return lo_hz >= 140000000u ? 3u : lo_hz >= 85000000u ? 4u : 5u;
}

// --- PA5 device trims (5.5). The stock firmware interpolates each trim from three calibration bytes p0, p1, p2 into a
// table of byte cells, then a frequency's value between two cells. Its arithmetic is reproduced exactly: steps are
// signed divisions truncated toward zero, cells wrap modulo 256, and the fraction is taken of the difference as an
// unsigned 16-bit value (so a falling table doesn't interpolate downward, as in the stock firmware). Frequencies are in
// 10 Hz units here, as the stock firmware keeps them ---
typedef struct {
	uint8_t mid, end;	// cells of p1 and p2 (p0 is cell 0)
	uint8_t div1, div2;	// the two segments' divisors
	bool down;		// the second segment is filled down from p2, not up from p1
} trim_shape_t;

// UHF: 400-520 MHz in 10 MHz cells. VHF: 136-174 MHz in 1 MHz cells, then 220-225 MHz (cells 39-44); p1 is the
// value at 174 MHz, p2 at 225 MHz. Air band: 108-145 MHz in 1 MHz cells, p1 at 127 MHz
#define TRIM_UHF	((trim_shape_t){ 6, 12, 6, 6, false })
#define TRIM_VHF	((trim_shape_t){ 38, 44, 38, 51, true })
#define TRIM_AM		((trim_shape_t){ 19, 37, 19, 18, true })

static inline uint8_t trim_cell(trim_shape_t sh, const uint8_t p[3], uint8_t i)
{
	if (i <= sh.mid) {
		if (i == sh.mid) {
			return p[1];
		}
		int s1 = ((int)p[1] - (int)p[0]) / (int)sh.div1;
		return (uint8_t)(p[0] + i * s1);
	}
	int s2 = ((int)p[2] - (int)p[1]) / (int)sh.div2;
	if (i >= sh.end) {
		return p[2];
	}
	return sh.down ? (uint8_t)(p[2] - (sh.end - i) * s2) : (uint8_t)(p[1] + (i - sh.mid) * s2);
}

// Cell i plus m tenths of the way to cell i + 1, by the stock firmware's unsigned arithmetic
static inline uint8_t trim_between(trim_shape_t sh, const uint8_t p[3], uint8_t i, uint8_t m)
{
	uint8_t a = trim_cell(sh, p, i), b = trim_cell(sh, p, (uint8_t)(i + 1u));
	uint16_t diff = (uint16_t)((int)b - (int)a);
	return (uint8_t)(a + (uint16_t)(diff * m) / 10u);
}

#define F10(mhz)	((uint32_t)(mhz) * 100000u)	// MHz in 10 Hz units

// Register 0xA's value for side A at f (10 Hz units) from the band's block bytes 0x0A..0x0C (register 0xC uses the
// same lookup with bytes 0x0D..0x0F)
static inline uint8_t trim_lookup(const uint8_t p[3], uint32_t f, bool vhf220)
{
	if (f > F10(300)) {
		uint32_t d = f - F10(400);
		uint8_t i = (uint8_t)(d / 1000000u), m = (uint8_t)((uint8_t)(d / 100000u) % 10u);
		return i < 12u ? trim_between(TRIM_UHF, p, i, m) : p[2];
	}
	uint32_t d = f - (vhf220 ? F10(220) : F10(136));
	uint8_t i = (uint8_t)(d / 100000u), m = (uint8_t)((uint8_t)(d / 10000u) % 10u);
	if (vhf220) {
		return i < 5u ? trim_between(TRIM_VHF, p, (uint8_t)(39u + i), m) : p[2];
	}
	return i < 38u ? trim_between(TRIM_VHF, p, i, m) : trim_cell(TRIM_VHF, p, 38);
}

// Register 0xC in AM, from the UHF block's bytes 0x1B..0x1D
static inline uint8_t trim_am(const uint8_t p[3], uint32_t f)
{
	uint32_t d = f - F10(108);
	uint8_t i = (uint8_t)(d / 100000u), m = (uint8_t)((uint8_t)(d / 10000u) % 10u);
	return i < 37u ? trim_between(TRIM_AM, p, i, m) : p[2];
}

// Register 4 at a TX start: power level 0-3 (Low..Turbo); turbo takes the band's block byte 0x16
static inline uint8_t trim_tx(uint32_t f, uint8_t level, uint8_t uhf_16, uint8_t vhf_16)
{
	if (f >= F10(300)) {
		return level <= 1u ? 0x78u : level == 2u ? 0x82u : uhf_16;
	}
	return level <= 2u ? 0x78u : vhf_16;
}

#endif
