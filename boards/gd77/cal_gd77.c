// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 calibration point for the radio code (cal_gd77.h) from the factory blocks (cal_blocks_gd77.h,
// docs/spec/hardware-gd77.md 6.4). Pure conversion: no platform code, so the host tests run it on synthetic blocks.
// Unverified on a radio.
#include <stddef.h>
#include "cal_gd77.h"
#include "cal_blocks_gd77.h"

// Frequencies here are in the header's unit, 100 kHz
#define VHF_HI		1740u	// the one VHF range the node table knows (136.0-174.0 MHz)
#define VHF_NODE_LO	1350u	// its nodes count from 135.0 MHz ...
#define VHF_FIRST	1360u	// ... but the first one stands at 136.0
#define DRIVE_STEP	50u	// id 0x41 nodes every 5 MHz

// Node spacing per TX (or RX) range. lo 0: any low end. Ranges not listed have no node layout: no calibration
typedef struct {
	uint16_t hi, lo;
	uint8_t drive_nodes;	// id 0x41: every DRIVE_STEP from lo, the last one at hi
	uint8_t step;		// ids 0x01 and 0x43: nodes every step from lo, the last value carried on up to hi
	uint8_t nodes;
} range_row_t;

static const range_row_t ranges[] = {
	{ 5200, 0, 24, 150, 8 },
	{ 4800, 0, 16, 100, 8 },
	{ 4000, 3200, 16, 100, 8 },
	{ 4700, 0, 14, 100, 7 },
	{ 4000, 3000, 20, 150, 7 },
	{ VHF_HI, 0, 8, 50, 8 },
};

typedef struct {
	uint16_t lo, hi, step;
	uint8_t n;
	bool last_at_hi;	// the last node is hi itself (id 0x41)
} nodes_t;

static const range_row_t *range_row(uint16_t lo, uint16_t hi)
{
	for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
		if (ranges[i].hi == hi && (ranges[i].lo == 0 || ranges[i].lo == lo)) {
			return &ranges[i];
		}
	}
	return NULL;
}

static bool make_nodes(uint16_t lo, uint16_t hi, bool drive, nodes_t *nd)
{
	const range_row_t *row = range_row(lo, hi);
	if (row == NULL) {
		return false;
	}
	nd->lo = hi == VHF_HI ? VHF_NODE_LO : lo;
	nd->hi = hi;
	nd->step = drive ? DRIVE_STEP : row->step;
	nd->n = drive ? row->drive_nodes : row->nodes;
	nd->last_at_hi = drive;
	return true;
}

static uint16_t node_at(const nodes_t *nd, uint8_t k)
{
	if (nd->last_at_hi && k == nd->n - 1u) {
		return nd->hi;
	}
	if (k == 0 && nd->lo == VHF_NODE_LO) {
		return VHF_FIRST;
	}
	return (uint16_t)(nd->lo + nd->step * k);
}

// Where x falls: 0 between nodes *seg and *seg + 1, 1 above the last node (still at or below hi), -1 outside
static int locate(const nodes_t *nd, uint16_t x, uint8_t *seg)
{
	if (x < nd->lo || x > nd->hi) {
		return -1;
	}
	for (uint8_t k = 0; k + 1u < nd->n; k++) {
		uint16_t up = node_at(nd, k + 1u);
		if (x < up || (x == up && up == nd->hi)) {
			*seg = k;
			return 0;
		}
	}
	*seg = (uint8_t)(nd->n - 1u);
	return 1;
}

// The node values y[k * stride] at x: linear between nodes. Above the last node the line through the last two
// values over [last node, hi], moved up by their difference, saturated to a byte. False outside the nodes
static bool node_value(const nodes_t *nd, uint16_t x, const uint8_t *y, uint8_t stride, uint8_t *out)
{
	uint8_t k;
	int where = locate(nd, x, &k);
	if (where < 0) {
		return false;
	}
	if (where == 0) {
		*out = (uint8_t)cal_lerp(node_at(nd, k), node_at(nd, k + 1u), x, y[k * stride], y[(k + 1u) * stride]);
		return true;
	}
	uint8_t y0 = y[(k - 1u) * stride], y1 = y[k * stride];
	int32_t v = (int32_t)cal_lerp(node_at(nd, k), nd->hi, x, y0, y1) + y1 - y0;
	*out = (uint8_t)(v < 0 ? 0 : v > 0xFF ? 0xFF : v);
	return true;
}

static const uint8_t *field(const uint8_t *raw, uint8_t id)
{
	return raw + cal_field_offset(&gd77_cal_layout, id, NULL);
}

static uint8_t below(uint32_t v, uint32_t limit, uint8_t fallback)
{
	return v < limit ? (uint8_t)v : fallback;
}

// The frequency in the header's unit; a value that doesn't fit is outside every range
static uint16_t hz_units(uint32_t hz)
{
	uint32_t x = hz / 100000u;
	return x > 0xFFFFu ? 0xFFFFu : (uint16_t)x;
}

uint8_t gd77_cal_drive(const uint8_t *raw, uint32_t hz, uint8_t level)
{
	nodes_t nd;
	uint8_t v;
	uint16_t lo = cal_le16(&raw[4]), hi = cal_le16(&raw[6]), x = hz_units(hz);
	if (level >= GD77_CAL_POWER_LEVELS || x < lo || !make_nodes(lo, hi, true, &nd) ||
	    !node_value(&nd, x, field(raw, GD77_CAL_ID_41) + level, 2, &v)) {
		return GD77_CAL_DRIVE_OUTSIDE;
	}
	return v;
}

// The receive side: every value but sq_open is per block, not per frequency
static void convert_rx(const uint8_t *raw, uint16_t x, gd77_cal_point_t *out)
{
	const uint8_t *f51 = field(raw, GD77_CAL_ID_51), *f43 = field(raw, GD77_CAL_ID_43);
	static const uint16_t sq_default[3] = { GD77_CAL_DEF_SQ_48, GD77_CAL_DEF_SQ_60, GD77_CAL_DEF_SQ_3F };
	out->rx_valid = true;
	out->rx_dac_gain = below(f51[0], 16, GD77_CAL_DEF_DAC_GAIN);
	out->rx_volume = below(f51[1], 16, GD77_CAL_DEF_VOLUME);
	for (uint8_t bw = 0; bw < GD77_CAL_BWS; bw++) {
		// Three (high, low) pairs per bandwidth after the 8 node bytes: 25 kHz first, then 12.5 kHz
		const uint8_t *p = f43 + 8 + (bw == 1 ? 0 : 6);
		for (uint8_t i = 0; i < 3; i++) {
			uint8_t h = p[2 * i], l = p[2 * i + 1];
			out->sq_reg[bw][i] = h < 0x7F && l < 0x7F && h >= l ? (uint16_t)((h << 7) | l) : sq_default[i];
		}
	}
	// The squelch base: the node at or below the frequency on the RX range, no interpolation
	nodes_t nd;
	uint8_t k;
	uint16_t lo = cal_le16(&raw[0]), hi = cal_le16(&raw[2]);
	if (x >= lo && make_nodes(lo, hi, false, &nd) && locate(&nd, x, &k) >= 0) {
		out->sq_valid = true;
		out->sq_open = f43[k];
	}
}

bool gd77_cal_convert(const uint8_t *raw, uint32_t hz, gd77_cal_point_t *out)
{
	gd77_cal_safe(out);
	if (raw == NULL || !gd77_cal_check(raw)) {
		return false;
	}
	uint16_t x = hz_units(hz);
	convert_rx(raw, x, out);

	nodes_t drive, mod;
	uint16_t lo = cal_le16(&raw[4]), hi = cal_le16(&raw[6]);
	uint16_t bias = cal_le16(field(raw, GD77_CAL_ID_40));
	if (x < lo || x > hi || bias > GD77_CAL_BIAS_MAX || !make_nodes(lo, hi, true, &drive) ||
	    !make_nodes(lo, hi, false, &mod)) {
		return false;
	}
	gd77_cal_point_t p = *out;
	const uint8_t *f41 = field(raw, GD77_CAL_ID_41);
	for (uint8_t level = 0; level < GD77_CAL_POWER_LEVELS; level++) {
		if (!node_value(&drive, x, f41 + level, 2, &p.power_drive[level])) {
			return false;
		}
	}
	if (!node_value(&mod, x, field(raw, GD77_CAL_ID_01), 1, &p.c6k_mod1)) {
		return false;
	}
	p.c6k_bias = bias;
	p.c6k_offset = field(raw, GD77_CAL_ID_40)[2];

	const uint8_t *f50 = field(raw, GD77_CAL_ID_50);
	const uint8_t *f33 = field(raw, GD77_CAL_ID_33), *f34 = field(raw, GD77_CAL_ID_34);
	p.pa_drv = below(f50[0], 16, GD77_CAL_DEF_PA_DRV);
	p.mic_pga = below(f50[1], 0x20, GD77_CAL_DEF_MIC_PGA);
	p.voice_gain = below(f50[2], 0x80, GD77_CAL_DEF_VOICE_GAIN);
	p.gain_tx = below(f50[3], 16, GD77_CAL_DEF_GAIN_TX);
	for (uint8_t bw = 0; bw < GD77_CAL_BWS; bw++) {
		uint8_t sub = bw == 1 ? 0 : 1;	// the block keeps 25 kHz first, then 12.5 kHz
		uint16_t dev = cal_le16(f50 + 4 + 2 * sub);
		p.fm_dev[bw] = dev < 0x400u ? dev : GD77_CAL_DEF_FM_DEV;
		p.ctcss_dev[bw] = below(f33[sub], 0x40, GD77_CAL_DEF_TONE_DEV);
		p.dcs_dev[bw] = below(f34[sub], 0x40, GD77_CAL_DEF_TONE_DEV);
	}
	p.valid = true;
	*out = p;
	return true;
}

bool gd77_cal_at(uint32_t hz, gd77_cal_point_t *out)
{
	return gd77_cal_convert(gd77_cal_raw(gd77_cal_band(hz)), hz, out);
}
