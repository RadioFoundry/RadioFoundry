// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Temperature readings from the board's sensors (temp.h). No RTOS calls: the host tests run it
#include <stddef.h>
#include "temp.h"

bool temp_curve_dc(const temp_point_t *curve, uint8_t len, uint16_t mv, int16_t *dc)
{
	if (curve == NULL || len == 0) {
		return false;
	}
	bool rising = len < 2 || curve[len - 1u].mv >= curve[0].mv;
	const temp_point_t *lo = rising ? &curve[0] : &curve[len - 1u];
	const temp_point_t *hi = rising ? &curve[len - 1u] : &curve[0];
	if (mv <= lo->mv) {
		*dc = lo->dc;
		return true;
	}
	if (mv >= hi->mv) {
		*dc = hi->dc;
		return true;
	}
	for (uint8_t i = 0; i + 1u < len; i++) {
		const temp_point_t *a = &curve[i], *b = &curve[i + 1u];
		uint16_t amin = a->mv < b->mv ? a->mv : b->mv, amax = a->mv < b->mv ? b->mv : a->mv;
		if (mv < amin || mv > amax) {
			continue;
		}
		if (a->mv == b->mv) {
			*dc = a->dc;
			return true;
		}
		int32_t num = ((int32_t)mv - a->mv) * ((int32_t)b->dc - a->dc);
		int32_t den = (int32_t)b->mv - a->mv;
		// Rounded to the nearest 0.1 degree, either sign
		int32_t q = (num + (((num < 0) != (den < 0)) ? -den / 2 : den / 2)) / den;
		*dc = (int16_t)(a->dc + q);
		return true;
	}
	return false;
}

bool temp_read(const hal_temp_t *t, uint8_t i, temp_reading_t *r)
{
	if (t == NULL || i >= t->count || t->sensors == NULL) {
		return false;
	}
	const hal_temp_sensor_t *s = &t->sensors[i];
	r->kind = s->kind;
	r->has_mv = s->read_mv != NULL;
	r->mv = r->has_mv ? s->read_mv() : 0;
	r->has_dc = false;
	r->dc = 0;
	if (s->read_dc != NULL) {
		r->has_dc = s->read_dc(&r->dc);
	} else if (r->has_mv) {
		r->has_dc = temp_curve_dc(s->curve, s->curve_len, r->mv, &r->dc);
	}
	return true;
}
