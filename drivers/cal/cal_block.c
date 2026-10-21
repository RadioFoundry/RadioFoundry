// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Generic factory calibration blocks (cal_block.h)
#include <string.h>
#include "cal_block.h"

static bool all(const uint8_t *p, uint32_t n, uint8_t v)
{
	for (uint32_t i = 0; i < n; i++) {
		if (p[i] != v) {
			return false;
		}
	}
	return true;
}

cal_block_state_t cal_block_load(cal_block_t *b, const cal_layout_t *layout, cal_read_fn rd, void *ctx, uint32_t addr,
				 bool (*check)(const uint8_t *raw))
{
	uint8_t again[CAL_BLOCK_MAX];
	uint16_t n = layout->size;

	memset(b->raw, 0xFF, sizeof(b->raw));
	if (n == 0 || n > CAL_BLOCK_MAX || !rd(ctx, addr, b->raw, n) || !rd(ctx, addr, again, n) ||
	    memcmp(b->raw, again, n) != 0) {
		b->state = CAL_BLOCK_READ_FAILED;
	} else if (all(b->raw, n, 0xFF) || all(b->raw, n, 0x00)) {
		b->state = CAL_BLOCK_BLANK;
	} else if (check != NULL && !check(b->raw)) {
		b->state = CAL_BLOCK_REJECTED;
	} else {
		b->state = CAL_BLOCK_OK;
	}
	return b->state;
}

int cal_band_index(const cal_band_t *bands, uint8_t count, uint32_t hz)
{
	for (uint8_t i = 0; i < count; i++) {
		if (hz >= bands[i].lo_hz && hz <= bands[i].hi_hz) {
			return i;
		}
	}
	return -1;
}

int cal_field_offset(const cal_layout_t *layout, uint8_t id, uint8_t *size)
{
	uint32_t off = layout->header;
	for (uint8_t i = 0; i < layout->count; i++) {
		const cal_field_t *f = &layout->fields[i];
		if (f->id == id && f->size != 0) {
			if (off + f->size > layout->size) {
				return -1;	// a table that doesn't fit its block
			}
			if (size != NULL) {
				*size = f->size;
			}
			return (int)off;
		}
		off += f->size;
	}
	return -1;
}

uint8_t cal_field_get(const cal_block_t *b, const cal_layout_t *layout, uint8_t id, uint8_t *out, uint8_t max)
{
	uint8_t size;
	int off = b->state == CAL_BLOCK_OK ? cal_field_offset(layout, id, &size) : -1;
	if (off < 0) {
		return 0;
	}
	uint8_t n = size < max ? size : max;
	memcpy(out, &b->raw[off], n);
	return n;
}
