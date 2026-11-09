// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Two-level analog keypad decode (adckeys.h)
#include <stddef.h>
#include "adckeys.h"

static int window_of(const adckeys_window_t *w, uint8_t n, uint16_t level)
{
	for (uint8_t i = 0; i < n; i++) {
		if (level >= w[i].lo && level <= w[i].hi) {
			return i;
		}
	}
	return -1;
}

uint8_t adckeys_decode(const adckeys_t *k, uint16_t row_level, uint16_t col_level)
{
	if (row_level >= k->idle_min && col_level >= k->idle_min) {
		return ADCKEYS_NONE;
	}
	int r = window_of(k->rows, k->n_rows, row_level);
	int c = window_of(k->cols, k->n_cols, col_level);
	if (r < 0 || c < 0) {
		return ADCKEYS_NONE;
	}
	return k->map[r * k->n_cols + c];
}

void adckeys_init(adckeys_state_t *s)
{
	s->seen = ADCKEYS_NONE;
	s->count = 0;
	s->key = ADCKEYS_NONE;
}

uint8_t adckeys_debounce(adckeys_state_t *s, uint8_t decoded, uint8_t need)
{
	if (decoded != s->seen) {
		s->seen = decoded;
		s->count = 0;
	}
	if (s->count < 255u) {
		s->count++;
	}
	if (s->count >= need) {
		s->key = decoded;
	}
	return s->key;
}
