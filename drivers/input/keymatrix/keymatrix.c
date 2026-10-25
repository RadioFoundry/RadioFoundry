// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Key matrix decoding (keymatrix.h)
#include <stddef.h>
#include "keymatrix.h"

uint32_t keymatrix_decode(const keymatrix_t *m, uint8_t idle_low, const uint8_t *row_low)
{
	uint32_t keys = 0;
	for (uint8_t c = 0; c < m->cols && c < KEYMATRIX_MAX_COLS; c++) {
		uint8_t bit = (uint8_t)(1u << c);
		if (idle_low & bit) {
			if (m->direct != NULL && m->direct[c] != KEYMATRIX_NONE) {
				keys |= 1u << m->direct[c];
			}
			continue;	// held low by a direct key: its matrix keys can't be told apart
		}
		for (uint8_t r = 0; r < m->rows; r++) {
			uint8_t k = m->map[r * m->cols + c];
			if ((row_low[r] & bit) && k != KEYMATRIX_NONE) {
				keys |= 1u << k;
			}
		}
	}
	return keys;
}
