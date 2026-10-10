// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A factory calibration curve: values measured at a few frequencies, linear in between. Every radio's calibration
// reader uses it for PA drive, front-end tuning and the like; what differs between radios is only the rounding and what
// happens past the last point, so those are flags.
#ifndef RADIO_CAL_CURVE_H
#define RADIO_CAL_CURVE_H
#include <stdint.h>

#define CAL_CURVE_MAX_POINTS	16u

// Round to the nearest, halves up (floor(x + 0.5)). Without it the result truncates toward zero, as C division
#define CAL_CURVE_ROUND		0x01u
// Past the last point, continue the last segment. Without it the last value holds
#define CAL_CURVE_EXTEND	0x02u

// The curve at x. points ascending, n from 2 to CAL_CURVE_MAX_POINTS; at or below the first point the first value
int32_t cal_curve_at(const uint32_t *points, const int32_t *values, uint8_t n, uint32_t x, uint8_t flags);

#endif
