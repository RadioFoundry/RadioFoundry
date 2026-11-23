// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

// CRC-CCITT, register from 0, remainder inverted (crc16.h)

#include "fec/crc16.h"

uint16_t fec_crc_ccitt(const uint8_t *p, uint16_t len)
{
	uint16_t r = 0;

	while (len--) {
		r ^= (uint16_t)(*p++ << 8);
		for (uint8_t b = 0; b < 8; b++)
			r = (r & 0x8000u) ? (uint16_t)((r << 1) ^ 0x1021u) : (uint16_t)(r << 1);
	}
	return (uint16_t)~r;
}
