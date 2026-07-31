// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 factory calibration reader: STUB, to be replaced
#include "dm1701.h"

bool dm1701_cal_load(void) { return false; }
bool dm1701_cal_at(uint32_t hz, dm1701_cal_point_t *out)
{
	(void)hz;
	*out = (dm1701_cal_point_t){ .valid = false, .power_dac = { 0, 0, 0, 0 }, .fm_i = 0x80, .fm_q = 0x80, .dmr_i = 0x80, .dmr_q = 0x80, .ref_offset = 0 };
	return false;
}
