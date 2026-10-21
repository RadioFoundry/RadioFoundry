// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Factory calibration block reader (calblock.h)
#include <string.h>
#include "calblock.h"

static bool all_same(const uint8_t *p, uint32_t n, uint8_t v)
{
	for (uint32_t i = 0; i < n; i++) {
		if (p[i] != v) {
			return false;
		}
	}
	return true;
}

calblock_state_t calblock_check(calblock_t *b)
{
	if (b->data == NULL || b->size == 0) {
		b->state = CALBLOCK_READ_FAILED;
	} else if (all_same(b->data, b->size, 0xFFu) || all_same(b->data, b->size, 0x00u)) {
		b->state = CALBLOCK_BLANK;
	} else {
		b->state = CALBLOCK_OK;
	}
	return (calblock_state_t)b->state;
}

calblock_state_t calblock_load(calblock_t *b, calblock_read_t rd, const calblock_part_t *parts, uint8_t count)
{
	if (b->data == NULL) {
		b->state = CALBLOCK_READ_FAILED;
		return CALBLOCK_READ_FAILED;
	}
	memset(b->data, 0xFF, b->size);
	uint32_t at = 0;
	for (uint8_t i = 0; i < count; i++) {
		if (rd == NULL || parts[i].len > b->size - at || !rd(parts[i].addr, b->data + at, parts[i].len)) {
			// Nothing half read is kept: a value from a damaged read could drive a DAC anywhere
			memset(b->data, 0xFF, b->size);
			b->state = CALBLOCK_READ_FAILED;
			return CALBLOCK_READ_FAILED;
		}
		at += parts[i].len;
	}
	return calblock_check(b);
}

const char *calblock_state_name(const calblock_t *b)
{
	static const char *const names[] = { "none", "unreadable", "blank", "factory" };
	return b != NULL && b->state < 4 ? names[b->state] : "none";
}

bool calblock_row_blank(const calblock_t *b, const calblock_row_t *r)
{
	if (b == NULL || b->data == NULL || r->points == 0 || r->offset >= b->size ||
	    r->points > b->size - r->offset) {
		return true;
	}
	const uint8_t *p = b->data + r->offset;
	return all_same(p, r->points, 0xFFu) || all_same(p, r->points, 0x00u);
}

bool calblock_row_at(const calblock_t *b, const calblock_row_t *r, uint32_t hz, int32_t *out)
{
	if (!calblock_ok(b) || r->points > CAL_CURVE_MAX_POINTS || calblock_row_blank(b, r)) {
		return false;
	}
	int32_t v[CAL_CURVE_MAX_POINTS];
	for (uint8_t i = 0; i < r->points; i++) {
		v[i] = b->data[r->offset + i];
	}
	*out = cal_curve_grid_at(r->first_hz, r->step_hz, v, r->points, hz, r->flags);
	return true;
}

uint8_t calblock_byte(const calblock_t *b, uint16_t offset)
{
	if (b == NULL || b->data == NULL || b->state == CALBLOCK_NONE || b->state == CALBLOCK_READ_FAILED ||
	    offset >= b->size) {
		return 0xFFu;
	}
	return b->data[offset];
}

uint16_t calblock_u16le(const calblock_t *b, uint16_t offset)
{
	if (offset == 0xFFFFu) {
		return 0xFFFFu;
	}
	return (uint16_t)(calblock_byte(b, offset) | (calblock_byte(b, (uint16_t)(offset + 1u)) << 8));
}

bool calblock_bcd(uint32_t v, uint8_t digits, uint32_t *out)
{
	uint32_t r = 0;
	if (digits == 0 || digits > 8) {
		return false;
	}
	for (int i = digits - 1; i >= 0; i--) {
		uint32_t d = (v >> (4 * i)) & 0xFu;
		if (d > 9) {
			return false;
		}
		r = r * 10u + d;
	}
	*out = r;
	return true;
}
