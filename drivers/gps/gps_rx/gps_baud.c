// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GPS module baud rate hunt (gps_baud.h)
#include "gps_baud.h"

void gps_baud_init(gps_baud_t *b, const uint32_t *rates, uint8_t n, uint32_t window_ms, uint32_t now_ms,
		   uint32_t activity)
{
	b->rates = rates;
	b->n = n;
	b->idx = 0;
	b->window_ms = window_ms;
	b->t0 = now_ms;
	b->activity0 = activity;
	b->done = n == 0;
	b->last = n == 0 ? GPS_BAUD_ABSENT : GPS_BAUD_LISTEN;
}

uint32_t gps_baud_rate(const gps_baud_t *b)
{
	return b->n != 0 ? b->rates[b->idx] : 0;
}

gps_baud_step_t gps_baud_step(gps_baud_t *b, gps_rx_state_t rx, uint32_t now_ms, uint32_t activity)
{
	if (b->done) {
		return b->last;
	}
	if (rx == GPS_RX_FOUND || rx == GPS_RX_PRESENT) {
		b->done = true;
		b->last = GPS_BAUD_FOUND;
		return b->last;
	}
	if ((uint32_t)(now_ms - b->t0) < b->window_ms) {
		return GPS_BAUD_LISTEN;
	}
	if (activity == b->activity0 || b->idx + 1u >= b->n) {
		// A silent line (no module), or every rate tried
		b->done = true;
		b->last = GPS_BAUD_ABSENT;
		return b->last;
	}
	b->idx++;
	b->t0 = now_ms;
	b->activity0 = activity;
	return GPS_BAUD_SWITCH;
}
