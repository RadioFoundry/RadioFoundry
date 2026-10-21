// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Keys read as two analog levels: a keypad whose rows pull one ADC input and whose columns pull another to a level
// per position through resistor ladders (the MD-9600's hand microphone, docs/spec/hardware-md9600.md 9.3). One key at a
// time. Generic and pure: the board reads the two levels, this maps them to a key and holds it until it has read the
// same for a few scans, so a level passing through another key's window while a key goes down or up is never
// reported.
#ifndef ADCKEYS_H
#define ADCKEYS_H

#include <stdint.h>
#include <stdbool.h>

#define ADCKEYS_NONE	0xFFu

typedef struct {
	uint16_t lo, hi;	// raw ADC counts, both inclusive
} adckeys_window_t;

typedef struct {
	const adckeys_window_t *rows;	// windows of the row level, n_rows of them
	uint8_t n_rows;
	const adckeys_window_t *cols;	// windows of the column level
	uint8_t n_cols;
	const uint8_t *map;		// n_rows x n_cols key ids (row major), ADCKEYS_NONE where no key sits
	uint16_t idle_min;		// both levels at or above this: no key down
} adckeys_t;

// The key whose row and column windows hold the two levels, ADCKEYS_NONE for none
uint8_t adckeys_decode(const adckeys_t *k, uint16_t row_level, uint16_t col_level);

typedef struct {
	uint8_t seen;		// the decode of the last scan
	uint8_t count;		// scans in a row it has been the same
	uint8_t key;		// the key reported
} adckeys_state_t;

void adckeys_init(adckeys_state_t *s);
// One scan's decode in; the key reported out. A change (a key, or none) is taken once it has been decoded on `need`
// scans in a row (1 takes it at once)
uint8_t adckeys_debounce(adckeys_state_t *s, uint8_t decoded, uint8_t need);

#endif
