// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 calibration point for the radio code (cal_gd77.h) from the factory blocks (cal_blocks_gd77.h,
// docs/spec/hardware-gd77.md 6.4). None of the fields that would fill the point is named yet, so every frequency gets
// the safe values and false, whether or not its block loaded, and the transmitter stays refused. Once a field is
// confirmed on a radio, its conversion goes here and the point becomes valid where its block is.
#include "cal_gd77.h"
#include "cal_blocks_gd77.h"

bool gd77_cal_at(uint32_t hz, gd77_cal_point_t *out)
{
	(void)hz;
	gd77_cal_safe(out);
	return false;
}
