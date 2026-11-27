// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Battery voltage (hal_power_t.battery_mv): a running average of the board's readings, so TX load and ADC noise don't
// make the number jump. The radio core turns it into a charge percentage.
//
// battery_avg_mv() takes a new reading at most every BATTERY_AVG_MS and averages over about 2^BATTERY_AVG_SHIFT of
// them; the first reading fills the average. Header only: the board keeps one battery_avg_t and its own scale.
#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>
#include <stdbool.h>

#define BATTERY_AVG_MS		500u
#define BATTERY_AVG_SHIFT	3u

typedef struct {
	uint32_t sum;		// the average << BATTERY_AVG_SHIFT
	uint32_t ms;		// when the last reading was taken
	bool valid;
} battery_avg_t;

static inline uint16_t battery_avg_mv(battery_avg_t *a, uint32_t now_ms, uint32_t (*read_mv)(void))
{
	if (!a->valid || now_ms - a->ms >= BATTERY_AVG_MS) {
		uint32_t mv = read_mv();
		if (!a->valid) {
			a->sum = mv << BATTERY_AVG_SHIFT;
			a->valid = true;
		} else {
			a->sum += mv - (a->sum >> BATTERY_AVG_SHIFT);
		}
		a->ms = now_ms;
	}
	return (uint16_t)(a->sum >> BATTERY_AVG_SHIFT);
}


#endif
