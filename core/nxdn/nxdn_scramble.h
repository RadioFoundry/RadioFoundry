// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 dB-SPL
// NXDN TS 1-D 5.3.1 and 5.4.4.1: scramble the 49 vocoder bits of one EHR VCH.
#ifndef RFOUNDRY_NXDN_SCRAMBLE_H
#define RFOUNDRY_NXDN_SCRAMBLE_H

#include <stdbool.h>
#include <stdint.h>

// frame and vch are positions 0..3 in the superframe, including positions occupied by FACCH1.
// bits are 49 unpacked bits, in vocoder order. Invalid arguments leave them untouched.
bool nxdn_scramble_ehr(uint16_t key, uint8_t frame, uint8_t vch, uint8_t bits[49]);

// Transform an EHR frame around its FEC. Returns false for an uncorrectable frame or invalid key/position.
bool nxdn_scramble_vch(uint16_t key, uint8_t frame, uint8_t vch, uint8_t ehr[9]);

#endif
