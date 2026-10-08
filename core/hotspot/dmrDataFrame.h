// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * Conversion between DMR data bursts (info bits) and 33 byte DMR frames, as used by MMDVM
 */

#ifndef ODR_DMRDATAFRAME_H
#define ODR_DMRDATAFRAME_H

#include "dmr/dmrData.h"

bool dmrDataFrameToBurst(uint8_t dataType, const uint8_t *frame, dmrBurst_t *burst);
bool dmrDataBurstToFrame(const dmrBurst_t *burst, uint8_t colorCode, uint8_t *frame);

#endif
