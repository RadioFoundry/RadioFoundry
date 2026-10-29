// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Generic factory calibration blocks: a radio keeps one block per band in its own storage, each a fixed header and
// then fields packed in table order. The radio's glue describes the layout (field ids and sizes) and the bands; this
// code reads the blocks through a read callback, checks they are readable and not blank, picks the block for a
// frequency and finds fields by id. It never writes, and keeps every byte raw: what a field means and how it is
// applied to the chips stays with the radio (a field whose meaning isn't known is still there to read).
#ifndef CAL_BLOCK_H
#define CAL_BLOCK_H

#include <stdint.h>
#include <stdbool.h>

#define CAL_BLOCK_MAX	256u	// the largest block the loader holds

typedef struct {
	uint8_t id;		// the radio's id for the field
	uint8_t size;		// bytes; 0 = an id with no storage (skipped in the offset sum)
} cal_field_t;

typedef struct {
	uint16_t size;		// bytes read per block (header, fields and any spare)
	uint16_t header;	// bytes before the first field
	const cal_field_t *fields;
	uint8_t count;
} cal_layout_t;

typedef struct {
	uint32_t lo_hz, hi_hz;	// frequencies the block serves, inclusive
	uint32_t addr;		// where the block is in the radio's storage
} cal_band_t;

typedef bool (*cal_read_fn)(void *ctx, uint32_t addr, void *buf, uint32_t len);

typedef enum {
	CAL_BLOCK_NONE,		// not read yet
	CAL_BLOCK_OK,
	CAL_BLOCK_READ_FAILED,	// the read failed, or two reads disagreed (a glitch on a shared bus)
	CAL_BLOCK_BLANK,	// all 0xFF or all 0x00: never written
	CAL_BLOCK_REJECTED,	// read fine, refused by the radio's own check
} cal_block_state_t;

typedef struct {
	uint8_t raw[CAL_BLOCK_MAX];
	cal_block_state_t state;
} cal_block_t;

// Read one block (layout->size bytes at addr) twice and compare. Sets and returns the state; check (optional) is the
// radio's sanity check on the raw bytes, run on a block that is neither blank nor unreadable
cal_block_state_t cal_block_load(cal_block_t *b, const cal_layout_t *layout, cal_read_fn rd, void *ctx, uint32_t addr,
				 bool (*check)(const uint8_t *raw));
// The band whose range holds hz, -1 if none
int cal_band_index(const cal_band_t *bands, uint8_t count, uint32_t hz);
// Offset of field id from the start of the block (header included), -1 if the layout has no such field or it has no
// bytes. size gets the field's size (may be NULL)
int cal_field_offset(const cal_layout_t *layout, uint8_t id, uint8_t *size);
// Copy field id of a loaded block into out (at most max bytes). Returns the bytes copied, 0 if the block isn't OK or
// the field is unknown
uint8_t cal_field_get(const cal_block_t *b, const cal_layout_t *layout, uint8_t id, uint8_t *out, uint8_t max);

// Straight line from (x0, y0) to (x1, y1) at x, truncated toward y0 (integer only, no overflow for 16-bit y and x
// spans below 65536). Outside [x0, x1] the nearer end value; x1 <= x0 gives y0
uint16_t cal_lerp(uint32_t x0, uint32_t x1, uint32_t x, uint16_t y0, uint16_t y1);

static inline uint16_t cal_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint16_t cal_be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

#endif
