// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A factory calibration block: the bytes a radio keeps its calibration in, read once and kept whole, the parts
// nobody has decoded yet included, and rows of measured points read from it by frequency. The board says where the
// bytes are (one or more pieces behind its own reader: SPI flash, security registers, an EEPROM) and where each row
// is; this file only checks and reads. It never writes, so a wrong layout costs a wrong value, never the data.
#ifndef CALBLOCK_H
#define CALBLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include "radio/cal_curve.h"

// Reads len bytes at addr of the board's storage; false on a bus or device failure
typedef bool (*calblock_read_t)(uint32_t addr, void *buf, uint32_t len);

// One piece of the block, in block order: the pieces are read one after the other into the buffer
typedef struct {
	uint32_t addr;
	uint16_t len;
} calblock_part_t;

typedef enum {
	CALBLOCK_NONE,		// not read yet
	CALBLOCK_READ_FAILED,	// a piece could not be read: no values
	CALBLOCK_BLANK,		// read, but all 0xFF or all 0x00 (erased, or never calibrated): no values
	CALBLOCK_OK,		// read and not blank: rows are used where they aren't blank themselves
} calblock_state_t;

typedef struct {
	uint8_t *data;		// the board's buffer, size bytes
	uint16_t size;
	uint8_t state;		// calblock_state_t
} calblock_t;

// A row: points evenly spaced in frequency, one byte each. Values are the bytes as stored; the board applies the
// scale its hardware needs (some radios' DACs take byte x 16)
typedef struct {
	uint16_t offset;	// the first point's byte in the block
	uint8_t points;		// 1..CAL_CURVE_MAX_POINTS
	uint8_t flags;		// CAL_CURVE_ROUND, CAL_CURVE_EXTEND
	uint32_t first_hz;	// frequency of point 0
	uint32_t step_hz;	// spacing of the points
} calblock_row_t;

// Read the pieces (their lengths must add up to b->size at most; the rest of the buffer is set to 0xFF) and set the
// state. Returns the state
calblock_state_t calblock_load(calblock_t *b, calblock_read_t rd, const calblock_part_t *parts, uint8_t count);
// The same for a block the board already holds (a copy, a host test): the state from its content
calblock_state_t calblock_check(calblock_t *b);

static inline bool calblock_ok(const calblock_t *b)
{
	return b != NULL && b->state == CALBLOCK_OK;
}

// "factory" (usable), "blank", "unreadable" or "none"
const char *calblock_state_name(const calblock_t *b);

// True when the row is outside the block, or its points are all 0xFF or all 0x00
bool calblock_row_blank(const calblock_t *b, const calblock_row_t *r);
// The row's value at hz: linear between points, the first point's value below it, past the last point as the row's
// flags say. False (and *out untouched) when the block isn't usable or the row is blank
bool calblock_row_at(const calblock_t *b, const calblock_row_t *r, uint32_t hz, int32_t *out);

// A byte of the block, 0xFF past its end or when nothing was read (diagnostics, the parts not decoded yet)
uint8_t calblock_byte(const calblock_t *b, uint16_t offset);
// A little-endian halfword; 0xFFFF past the end
uint16_t calblock_u16le(const calblock_t *b, uint16_t offset);

// Binary-coded decimal: the digits of v (most significant nibble first), false when a nibble is above 9. Pure helper
// for the boards' frequency fields
bool calblock_bcd(uint32_t v, uint8_t digits, uint32_t *out);

#endif
