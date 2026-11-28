// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

// CRC-CCITT as P25's trunking blocks and YSF's channels append it: x^16 + x^12 + x^5 + 1, the register from 0, the
// bytes most significant bit first, the remainder inverted. Reentrant, no table.

#ifndef FEC_CRC16_H
#define FEC_CRC16_H

#include <stdint.h>

uint16_t fec_crc_ccitt(const uint8_t *p, uint16_t len);

#endif
