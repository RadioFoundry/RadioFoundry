// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Sub-audio math shared by the RF chip drivers (AT1846S, FD6818)
#ifndef SUBAUDIO_H
#define SUBAUDIO_H

#include <stdint.h>

// The 23-bit DCS word for a 9-bit code: Golay(23,12) parity bits 22:12, the marker 100 in 11:9, the code in 8:0
// (code 023 -> 0x763813, the example in both the AT1846S and the FD6818 datasheets)
uint32_t subaudio_dcs_word(uint16_t code);

#endif
