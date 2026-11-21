// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The receive tap's soft values (HR_C6000 and HR_C7000: modem-hrc6000.md 5.8, modem-hrc7000.md 5.4): one byte per bit,
// bits 5-0 two's complement, a 1 negative (the hard bit is 32 or more), the surer the larger. A dibit's first bit is
// the symbol's sign, its second the distance from the outer level, both on that one scale. As the int8 soft value
// dmrSoftComb takes: positive for a 1
#ifndef HRC_SOFT_H
#define HRC_SOFT_H

#include <stdint.h>

// After the burst's 48-bit field and its 20 slot type bits: the 196 info bits in burst order
#define HRC_SOFT_INFO_OFS	68u

static inline int8_t hrc_soft_value(uint8_t x)
{
	int v = x & 0x3F;
	return (int8_t)(v >= 32 ? 64 - v : -v);
}

#endif
