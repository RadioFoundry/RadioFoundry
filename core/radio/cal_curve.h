// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A factory calibration curve: values measured at a few frequencies, linear in between. Every radio's calibration
// reader uses it for PA drive, front-end tuning and the like; what differs between radios is only the rounding and what
// happens past the last point, so those are flags.
//
// Inline on purpose: each reader calls it from one place with constant flags, so the compiler keeps only the paths
// that reader uses (the rounding and its 64-bit floor cost an STM32 build about 100 bytes as a shared function).
#ifndef RADIO_CAL_CURVE_H
#define RADIO_CAL_CURVE_H
#include <stdint.h>

#define CAL_CURVE_MAX_POINTS	16u

// Round to the nearest, halves up (floor(x + 0.5)). Without it the result truncates toward zero, as C division
#define CAL_CURVE_ROUND		0x01u
// Past the last point, continue the last segment. Without it the last value holds
#define CAL_CURVE_EXTEND	0x02u

// v0 plus (v1 - v0) x d / span, rounded as flags say
static inline int32_t cal_curve_segment(int32_t v0, int32_t v1, uint32_t d, uint32_t span, uint8_t flags)
{
	if (span == 0) {
		return v1;
	}
	int64_t num = (int64_t)(v1 - v0) * d;
	int64_t q;
	if (flags & CAL_CURVE_ROUND) {
		// floor((2 num + span) / 2 span), a floor that holds for negative numbers too
		int64_t t = 2 * num + span, den = 2 * (int64_t)span;
		q = t / den;
		if (t < 0 && q * den != t) {
			q--;
		}
	} else {
		q = num / (int64_t)span;
	}
	return v0 + (int32_t)q;
}

// The curve at x. points ascending, n from 2 to CAL_CURVE_MAX_POINTS; at or below the first point the first value
static inline int32_t cal_curve_at(const uint32_t *points, const int32_t *values, uint8_t n, uint32_t x,
	uint8_t flags)
{
	if (n == 0) {
		return 0;
	}
	if (n == 1 || x <= points[0]) {
		return values[0];
	}
	if (x >= points[n - 1] && !(flags & CAL_CURVE_EXTEND)) {
		return values[n - 1];
	}
	// The segment ending at the first point at or above x, the last one past the end
	uint8_t k = 1;
	while (k < n - 1 && points[k] < x) {
		k++;
	}
	uint32_t span = points[k] > points[k - 1] ? points[k] - points[k - 1] : 0;
	return cal_curve_segment(values[k - 1], values[k], x - points[k - 1], span, flags);
}

// The same on evenly spaced points first, first + step, ...: no point list, and the segment comes from one division
static inline int32_t cal_curve_grid_at(uint32_t first, uint32_t step, const int32_t *values, uint8_t n, uint32_t x,
	uint8_t flags)
{
	if (n == 0) {
		return 0;
	}
	if (n == 1 || x <= first || step == 0) {
		return values[0];
	}
	uint32_t i = (x - first) / step;
	if (i >= n - 1u) {
		if (!(flags & CAL_CURVE_EXTEND)) {
			return values[n - 1];
		}
		i = n - 2u;
	}
	return cal_curve_segment(values[i], values[i + 1], x - first - i * step, step, flags);
}

#endif
