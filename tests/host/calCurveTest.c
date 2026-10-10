// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The shared calibration curve (core/radio/cal_curve.c): edges, both roundings, extension past the last point, the
// even-grid form, and equality with the per-board interpolators it replaced (the MD-380's and the DM-1701's, kept
// here as references)
#include <stdio.h>
#include "radio/cal_curve.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// The MD-380's: clamped at both ends, truncated
static int32_t ref_md380(const uint32_t *freq, const uint8_t *p, uint32_t n, uint32_t f, int32_t mul)
{
	if (f <= freq[0]) {
		return p[0] * mul;
	}
	if (f >= freq[n - 1]) {
		return p[n - 1] * mul;
	}
	uint32_t i = 0;
	while (i < n - 2 && f >= freq[i + 1]) {
		i++;
	}
	int32_t a = p[i] * mul, b = p[i + 1] * mul;
	int32_t span = (int32_t)(freq[i + 1] - freq[i]);
	int32_t d = (int32_t)(f - freq[i]);
	return a + (int32_t)((int64_t)(b - a) * d / span);
}

// The DM-1701's: an even grid, clamped below, extended above, truncated
static int32_t ref_dm1701(const uint8_t *p, uint8_t n, uint32_t first, uint32_t step, uint32_t f, int32_t mul)
{
	if (f <= first) {
		return p[0] * mul;
	}
	uint32_t i = (f - first) / step;
	if (i > (uint32_t)n - 2u) {
		i = (uint32_t)n - 2u;
	}
	int64_t a = (int64_t)p[i] * mul;
	int64_t b = (int64_t)p[i + 1] * mul;
	int64_t d = (int64_t)f - (int64_t)(first + i * step);
	return (int32_t)(a + (b - a) * d / (int64_t)step);
}

static void test_edges(void)
{
	static const uint32_t pt[3] = { 100, 200, 400 };
	static const int32_t v[3] = { 10, 20, 0 };
	CHECK(cal_curve_at(pt, v, 3, 0, 0) == 10);
	CHECK(cal_curve_at(pt, v, 3, 100, 0) == 10);
	CHECK(cal_curve_at(pt, v, 3, 150, 0) == 15);
	CHECK(cal_curve_at(pt, v, 3, 200, 0) == 20);
	CHECK(cal_curve_at(pt, v, 3, 400, 0) == 0);
	CHECK(cal_curve_at(pt, v, 3, 500, 0) == 0);
	CHECK(cal_curve_at(pt, v, 3, 500, CAL_CURVE_EXTEND) == -10);
	CHECK(cal_curve_at(pt, v, 1, 500, CAL_CURVE_EXTEND) == 10);
	CHECK(cal_curve_at(pt, v, 0, 500, 0) == 0);
	// Falling segment 20 -> 0 over 200: at 210 it's 19.0, at 205 19.5, at 206 19.4, at 215 18.5
	CHECK(cal_curve_at(pt, v, 3, 205, 0) == 20);	// the step truncates toward zero, as the boards did
	CHECK(cal_curve_at(pt, v, 3, 205, CAL_CURVE_ROUND) == 20);
	CHECK(cal_curve_at(pt, v, 3, 206, CAL_CURVE_ROUND) == 19);
	CHECK(cal_curve_at(pt, v, 3, 215, CAL_CURVE_ROUND) == 19);
	// Rising: 10 -> 20 over 100, at 105 10.5 rounds up, at 104 10.4 down
	CHECK(cal_curve_at(pt, v, 3, 105, CAL_CURVE_ROUND) == 11);
	CHECK(cal_curve_at(pt, v, 3, 104, CAL_CURVE_ROUND) == 10);
	// Two equal points don't divide by zero
	static const uint32_t dup[3] = { 100, 100, 200 };
	CHECK(cal_curve_at(dup, v, 3, 100, 0) == 10);
	CHECK(cal_curve_at(dup, v, 3, 150, 0) == 10);
}

static void test_matches_boards(void)
{
	static const uint8_t p[9] = { 120, 135, 90, 255, 0, 17, 200, 199, 60 };
	// MD-380 style: 9 uneven points in 10 Hz units, x16 and x1
	static const uint32_t freq[9] = { 40000000, 40500000, 41000000, 42100000, 43000000, 44000000, 45000000,
		46100000, 47000000 };
	int32_t v16[9], v1[9];
	for (int i = 0; i < 9; i++) {
		v16[i] = p[i] * 16;
		v1[i] = p[i];
	}
	int bad = 0;
	for (uint32_t f = 39000000; f <= 48000000; f += 1237) {
		bad += cal_curve_at(freq, v16, 9, f, 0) != ref_md380(freq, p, 9, f, 16);
		bad += cal_curve_at(freq, v1, 9, f, 0) != ref_md380(freq, p, 9, f, 1);
	}
	CHECK(bad == 0);
	// DM-1701 style: an even grid of 5 or 9 points, extended above
	static const uint8_t npts[2] = { 5, 9 };
	static const uint32_t first[2] = { 13500000, 40000000 }, step[2] = { 1000000, 1000000 };
	bad = 0;
	for (int b = 0; b < 2; b++) {
		uint32_t grid[9];
		for (int i = 0; i < npts[b]; i++) {
			grid[i] = first[b] + step[b] * (uint32_t)i;
		}
		for (uint32_t f = first[b] - 500000; f <= first[b] + step[b] * (npts[b] + 1u); f += 997) {
			bad += cal_curve_at(grid, v16, npts[b], f, CAL_CURVE_EXTEND) != ref_dm1701(p, npts[b], first[b], step[b], f, 16);
			bad += cal_curve_at(grid, v1, npts[b], f, CAL_CURVE_EXTEND) != ref_dm1701(p, npts[b], first[b], step[b], f, 1);
			bad += cal_curve_grid_at(first[b], step[b], v16, npts[b], f, CAL_CURVE_EXTEND) !=
				ref_dm1701(p, npts[b], first[b], step[b], f, 16);
			bad += cal_curve_grid_at(first[b], step[b], v1, npts[b], f, CAL_CURVE_EXTEND) !=
				ref_dm1701(p, npts[b], first[b], step[b], f, 1);
		}
	}
	CHECK(bad == 0);
}

// The grid form agrees with the point-list form everywhere, for every flag
static void test_grid(void)
{
	static const int32_t v[5] = { 300, -40, 1000, 1000, 7 };
	static const uint32_t pt[5] = { 1000, 1300, 1600, 1900, 2200 };
	int bad = 0;
	for (uint8_t flags = 0; flags < 4; flags++) {
		for (uint32_t x = 0; x < 3000; x += 7) {
			bad += cal_curve_grid_at(1000, 300, v, 5, x, flags) != cal_curve_at(pt, v, 5, x, flags);
		}
	}
	CHECK(bad == 0);
	CHECK(cal_curve_grid_at(1000, 300, v, 5, 2200, 0) == 7);
	CHECK(cal_curve_grid_at(1000, 300, v, 5, 2500, CAL_CURVE_EXTEND) == -986);
	CHECK(cal_curve_grid_at(1000, 0, v, 5, 2500, 0) == 300);
	CHECK(cal_curve_grid_at(1000, 300, v, 1, 2500, 0) == 300);
	CHECK(cal_curve_grid_at(1000, 300, v, 0, 2500, 0) == 0);
}

int main(void)
{
	test_edges();
	test_grid();
	test_matches_boards();
	if (failures) {
		printf("calCurveTest: %d failures\n", failures);
		return 1;
	}
	printf("calCurveTest: all passed\n");
	return 0;
}
