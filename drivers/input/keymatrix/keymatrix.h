// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Key matrix and rotary encoder decoding, independent of the board's pins. The board drives one row at a time and
// reads the columns (active low); keys wired from a column straight to ground (no row) show up as columns that are
// already low before any row is driven. Those columns can't be scanned while held, so their matrix keys are ignored
// then (DM-32 SK1 / SK2, docs/spec/hardware-dm32.md 6.1, 6.2).
#ifndef KEYMATRIX_H
#define KEYMATRIX_H

#include <stdint.h>
#include <stdbool.h>

#define KEYMATRIX_NONE	0xFFu
#define KEYMATRIX_MAX_COLS	8u

typedef struct {
	uint8_t rows, cols;
	const uint8_t *map;		// rows x cols key bit numbers (row major), KEYMATRIX_NONE for no key
	const uint8_t *direct;		// cols entries: key bit of a column-to-ground key, KEYMATRIX_NONE for none
} keymatrix_t;

// idle_low: column bits low with no row driven. row_low[r]: column bits low while row r was driven.
// Returns the bit mask of keys down
uint32_t keymatrix_decode(const keymatrix_t *m, uint8_t idle_low, const uint8_t *row_low);

// Rotary encoder, one count per edge of B: -1 when A equals B after the edge, else +1
static inline int8_t keymatrix_encoder_step(bool a, bool b)
{
	return a == b ? -1 : 1;
}

#endif
