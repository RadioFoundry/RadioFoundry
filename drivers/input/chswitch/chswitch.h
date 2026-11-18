// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A channel switch: a knob with a fixed number of positions that gives a code on a few input lines. The board reads
// the lines as a number and its table turns the code into a position (the order is the switch's own, measured on the
// radio). Between two positions the contacts don't all change at once, so the code can pass through a third one,
// itself a position elsewhere on the knob: a code counts only once it has read the same for settle_ms.
//
// The position is absolute, so a radio without a screen picks its channel by where the knob points, also at power-on
// (hal_input_t.position, docs/spec/ui.md 3).
#ifndef CHSWITCH_H
#define CHSWITCH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	const int8_t *position;	// position by code, 0-based; -1: a code no position gives
	uint8_t codes;		// entries in position[]
	uint16_t settle_ms;
	uint8_t cand;		// the code read last
	uint32_t cand_ms;	// since when it has read the same
	int8_t pos;		// the settled position, -1 until one settles
} chswitch_t;

void chswitch_init(chswitch_t *s, const int8_t *position, uint8_t codes, uint16_t settle_ms, uint8_t code,
	uint32_t now_ms);
// One reading of the lines; returns the settled position (-1 until the first one settles)
int8_t chswitch_poll(chswitch_t *s, uint8_t code, uint32_t now_ms);

#endif
