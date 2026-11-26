// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A volume potentiometer read on an ADC (volume_pot.h)
#include "radio/volume_pot.h"
#include "radio/cal_curve.h"

int8_t volume_pot_percent(volume_pot_t *s, const volume_pot_cal_t *cal, int32_t raw)
{
	if (cal->mute != VOLUME_POT_NO_MUTE) {
		s->turned_up = raw > cal->min + cal->mute + (s->turned_up ? 0 : VOLUME_POT_HYSTERESIS);
		if (!s->turned_up) {
			return 0;
		}
	}
	const uint32_t points[] = { (uint32_t)cal->min, (uint32_t)cal->mid, (uint32_t)cal->max };
	static const int32_t percents[] = { 0, 50, 100 };
	int32_t pct = cal_curve_at(points, percents, 3, (uint32_t)raw, 0);
	return (int8_t)(pct < 1 ? 1 : pct);	// 0 is kept for silence
}
