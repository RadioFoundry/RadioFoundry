// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AFSK through an RF chip's tone generator (afsk_tonegen.h)
#include <stddef.h>
#include "afsk_tonegen.h"

static const afsk_tonegen_board_t *hw;
static const uint8_t *volatile tones;
static volatile uint16_t nbits;
static volatile uint16_t pos;		// next symbol
static uint16_t mark_hz, space_hz;
static volatile uint16_t cur_hz;
static volatile bool busy;

void afsk_tonegen_attach(const afsk_tonegen_board_t *board)
{
	hw = board;
}

static bool symbol(uint16_t i)
{
	return ((tones[i >> 3] >> (i & 7u)) & 1u) != 0;
}

static void tick(void)
{
	uint16_t i = pos;
	if (i >= nbits) {
		hw->timer_stop();	// the last symbol has had its full time
		busy = false;
		return;
	}
	uint16_t hz = symbol(i) ? mark_hz : space_hz;
	if (hz != cur_hz) {
		hw->tone(hz);
		cur_hz = hz;
	}
	pos = (uint16_t)(i + 1u);
}

static bool tx_start(const uint8_t *t, uint16_t n, uint16_t baud, uint16_t mark, uint16_t space, uint8_t level)
{
	if (hw == NULL || busy || n == 0 || baud == 0) {
		return false;
	}
	tones = t;
	nbits = n;
	mark_hz = mark;
	space_hz = space;
	cur_hz = symbol(0) ? mark : space;
	if (!hw->begin(cur_hz, level)) {
		return false;
	}
	pos = 1;
	busy = true;
	hw->timer_start((1000000u + baud / 2u) / baud, tick);
	return true;
}

static bool tx_busy(void)
{
	return busy;
}

static void tx_stop(void)
{
	if (hw == NULL) {
		return;
	}
	hw->timer_stop();
	busy = false;
	hw->end();
}

const hal_afsk_t afsk_tonegen = {
	.tx_start = tx_start,
	.tx_busy = tx_busy,
	.tx_stop = tx_stop,
};
