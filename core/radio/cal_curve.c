// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Factory calibration curve (cal_curve.h)
#include "cal_curve.h"

int32_t cal_curve_at(const uint32_t *points, const int32_t *values, uint8_t n, uint32_t x, uint8_t flags)
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
	int64_t num = (int64_t)(values[k] - values[k - 1]) * (int64_t)(x - points[k - 1]);
	int64_t den = (int64_t)(points[k] - points[k - 1]);
	if (den <= 0) {
		return values[k];
	}
	int64_t q;
	if (flags & CAL_CURVE_ROUND) {
		// floor((2 num + den) / 2 den) with a floor that holds for negative numbers too
		int64_t t = 2 * num + den;
		q = t / (2 * den);
		if (t < 0 && q * 2 * den != t) {
			q--;
		}
	} else {
		q = num / den;
	}
	return values[k - 1] + (int32_t)q;
}
