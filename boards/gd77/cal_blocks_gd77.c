// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 factory calibration blocks (cal_blocks_gd77.h, docs/spec/hardware-gd77.md 6.4). No platform code here, so the host
// tests run it against synthetic blocks.
#include <stddef.h>
#include "cal_blocks_gd77.h"

// Field table in block order. Ids with no bytes (0x00, 0xFF, 0xF0, 0xF1, 0x02) take no room and are left out
static const cal_field_t fields[] = {
	{ GD77_CAL_ID_40, 3 },
	{ GD77_CAL_ID_41, 48 },
	{ GD77_CAL_ID_42, 4 },
	{ GD77_CAL_ID_43, 20 },
	{ GD77_CAL_ID_44, 2 },
	{ GD77_CAL_ID_01, 8 },
	{ GD77_CAL_ID_30, 1 },
	{ GD77_CAL_ID_31, 1 },
	{ GD77_CAL_ID_32, 1 },
	{ GD77_CAL_ID_33, 2 },
	{ GD77_CAL_ID_34, 2 },
	{ GD77_CAL_ID_50, 8 },
	{ GD77_CAL_ID_51, 3 },
};

const cal_layout_t gd77_cal_layout = {
	.size = GD77_CAL_BLOCK,
	.header = GD77_CAL_HEADER,
	.fields = fields,
	.count = sizeof(fields) / sizeof(fields[0]),
};

static const cal_band_t bands[GD77_CAL_BANDS] = {
	[GD77_CAL_UHF] = { GD77_CAL_SPLIT_HZ, UINT32_MAX, GD77_CAL_UHF_ADDR },
	[GD77_CAL_VHF] = { 0, GD77_CAL_SPLIT_HZ - 1u, GD77_CAL_VHF_ADDR },
};

static cal_block_t blocks[GD77_CAL_BANDS];

#define RANGE_MIN	0x550u		// 136.0 MHz in 100 kHz units
#define RANGE_MAX	0x1450u		// 520.0
#define GAP_LO		0xA29u		// 260.1
#define GAP_HI		0xF9Fu		// 399.9

static bool range_value_ok(uint16_t v)
{
	return v >= RANGE_MIN && v <= RANGE_MAX && (v < GAP_LO || v > GAP_HI);
}

bool gd77_cal_check(const uint8_t *raw)
{
	int f50 = cal_field_offset(&gd77_cal_layout, GD77_CAL_ID_50, NULL);
	if (f50 < 0 || raw[f50] >= 0x20u || raw[f50 + 1] >= 0x40u || raw[f50 + 2] >= 0x10u) {
		return false;
	}
	uint16_t lo = cal_le16(&raw[4]), hi = cal_le16(&raw[6]);
	return range_value_ok(lo) && range_value_ok(hi) && lo < hi;
}

void gd77_cal_load(cal_read_fn rd, void *ctx)
{
	for (unsigned b = 0; b < GD77_CAL_BANDS; b++) {
		cal_block_load(&blocks[b], &gd77_cal_layout, rd, ctx, bands[b].addr, gd77_cal_check);
	}
}

cal_block_state_t gd77_cal_state(gd77_cal_band_t band)
{
	return band < GD77_CAL_BANDS ? blocks[band].state : CAL_BLOCK_NONE;
}

gd77_cal_band_t gd77_cal_band(uint32_t hz)
{
	int i = cal_band_index(bands, GD77_CAL_BANDS, hz);
	return i < 0 ? GD77_CAL_VHF : (gd77_cal_band_t)i;
}

bool gd77_cal_ok(uint32_t hz)
{
	return blocks[gd77_cal_band(hz)].state == CAL_BLOCK_OK;
}

uint8_t gd77_cal_field(uint32_t hz, uint8_t id, uint8_t *out, uint8_t max)
{
	return cal_field_get(&blocks[gd77_cal_band(hz)], &gd77_cal_layout, id, out, max);
}

bool gd77_cal_range(gd77_cal_band_t band, uint8_t type, uint16_t *lo, uint16_t *hi)
{
	if (band >= GD77_CAL_BANDS || type > 1 || blocks[band].state != CAL_BLOCK_OK) {
		return false;
	}
	const uint8_t *p = &blocks[band].raw[type * 4u];
	*lo = cal_le16(p);
	*hi = cal_le16(p + 2);
	return true;
}

const uint8_t *gd77_cal_raw(gd77_cal_band_t band)
{
	return band < GD77_CAL_BANDS && blocks[band].state == CAL_BLOCK_OK ? blocks[band].raw : NULL;
}
