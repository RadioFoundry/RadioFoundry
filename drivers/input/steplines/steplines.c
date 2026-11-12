// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Step lines (steplines.h)
#include "steplines.h"

void steplines_init(steplines_t *s, uint8_t lines, uint8_t levels)
{
	s->lines = lines < STEPLINES_MAX ? lines : STEPLINES_MAX;
	s->last = s->level = levels;
	s->out = 0;
	for (uint8_t i = 0; i < STEPLINES_MAX; i++) {
		s->pending[i] = 0;
	}
}

uint8_t steplines_scan(steplines_t *s, uint8_t levels)
{
	if (levels == s->last) {
		uint8_t changed = (uint8_t)(levels ^ s->level);
		s->level = levels;
		for (uint8_t i = 0; i < s->lines; i++) {
			if ((changed & (1u << i)) && s->pending[i] < STEPLINES_BACKLOG) {
				s->pending[i]++;
			}
		}
	}
	s->last = levels;
	uint8_t out = 0;
	for (uint8_t i = 0; i < s->lines; i++) {
		uint8_t bit = (uint8_t)(1u << i);
		if (!(s->out & bit) && s->pending[i] > 0) {	// a scan up between presses, so each is a new press
			s->pending[i]--;
			out |= bit;
		}
	}
	s->out = out;
	return out;
}
