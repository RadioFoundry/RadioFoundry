// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Volume knob position (hal_power_t.volume) from the knob's ADC reading. Most knobs are audio taper pots: the reading
// barely moves for the first half of the turn and climbs fast after it, so an even split of the ADC range puts most of
// the turn at the bottom. The board measures the reading at evenly spaced points of the turn (the stops and, say, the
// quarters) and knob_curve_pct() is linear between them, so each part of the turn moves the volume as much.
// Header only: the board keeps its table and any hysteresis or mute zone of its own.
#ifndef KNOB_H
#define KNOB_H

#include <stdint.h>

// 0..100: pts[0..n-1] are the raw readings at 0, 1/(n-1), ... 1 of the turn, rising; n >= 2. 0 at or below pts[0],
// 100 at or above pts[n-1]
static inline int32_t knob_curve_pct(int32_t raw, const uint16_t *pts, uint8_t n)
{
	if (raw <= (int32_t)pts[0]) {
		return 0;
	}
	for (uint8_t i = 0; i + 1u < n; i++) {
		int32_t a = pts[i], b = pts[i + 1u];
		if (raw < b || i + 2u == n) {
			int32_t lo = (int32_t)i * 100 / (n - 1), hi = (int32_t)(i + 1u) * 100 / (n - 1);
			int32_t pct = lo + (raw - a) * (hi - lo) / (b - a);
			return pct > 100 ? 100 : pct;
		}
	}
	return 100;
}

// The level (0..levels-1, step_pct each) whose band holds pct, kept while pct stays within hyst_pct of the current
// band's edges, so a knob resting on a boundary doesn't flicker between two. The stops always give the end levels: the
// hysteresis would otherwise ask for more than 100 % to reach the top
static inline uint8_t knob_level(int32_t pct, uint8_t level, uint8_t levels, int32_t step_pct, int32_t hyst_pct)
{
	int32_t lo = (int32_t)level * step_pct;
	if (pct <= 0 || pct >= 100 || pct + hyst_pct < lo || pct >= lo + step_pct + hyst_pct) {
		int32_t l = pct <= 0 ? 0 : pct / step_pct;
		return (uint8_t)(l >= (int32_t)levels ? levels - 1u : (uint32_t)l);
	}
	return level;
}

#endif
