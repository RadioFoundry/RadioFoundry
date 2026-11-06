// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Channel switch position (chswitch.h)
#include "input/chswitch/chswitch.h"

void chswitch_init(chswitch_t *s, const int8_t *position, uint8_t codes, uint16_t settle_ms, uint8_t code,
	uint32_t now_ms)
{
	s->position = position;
	s->codes = codes;
	s->settle_ms = settle_ms;
	s->cand = code;
	s->cand_ms = now_ms;
	s->pos = -1;
}

int8_t chswitch_poll(chswitch_t *s, uint8_t code, uint32_t now_ms)
{
	if (code != s->cand) {
		s->cand = code;
		s->cand_ms = now_ms;
	} else if (now_ms - s->cand_ms >= s->settle_ms && code < s->codes && s->position[code] >= 0) {
		s->pos = s->position[code];
	}
	return s->pos;
}
