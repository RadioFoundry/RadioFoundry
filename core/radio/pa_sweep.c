// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The PA drive sweep (pa_sweep.h, docs/spec/pa-keyup.md 7)
#include "radio/pa_sweep.h"
#include <stddef.h>

bool pa_sweep_point(const pa_sweep_req_t *r, uint16_t top, uint16_t max, uint8_t i, uint8_t *over, uint32_t *ms,
		    pa_sweep_pt_t *pt)
{
	if (i >= r->count || r->on_ms == 0u || top == 0u) {
		return false;
	}
	int32_t d = (int32_t)r->start + (int32_t)r->step * i;
	uint32_t pct = r->ceiling_pct > PA_SWEEP_CEILING_MAX_PCT ? PA_SWEEP_CEILING_MAX_PCT : r->ceiling_pct;
	uint32_t ceiling = (uint32_t)top * pct / 100u;
	if (ceiling > max) {
		ceiling = max;
	}
	if (d <= 0 || (uint32_t)d > ceiling) {
		return false;
	}
	pt->drive = (uint16_t)d;
	pt->on_ms = r->on_ms;
	pt->off_ms = r->off_ms;
	if (pt->drive > top) {	// over the radio's own top level: short points, far apart, a few per sweep
		if (*over >= PA_SWEEP_OVER_MAX) {
			return false;
		}
		(*over)++;
		if (pt->on_ms > PA_SWEEP_OVER_ON_MAX_MS) {
			pt->on_ms = PA_SWEEP_OVER_ON_MAX_MS;
		}
		if (pt->off_ms < PA_SWEEP_OVER_OFF_MIN_MS) {
			pt->off_ms = PA_SWEEP_OVER_OFF_MIN_MS;
		}
	}
	uint32_t t = *ms + pt->on_ms + pt->off_ms;
	if (t > PA_SWEEP_MS_MAX) {
		return false;
	}
	*ms = t;
	return true;
}

uint8_t pa_sweep_run(const pa_sweep_req_t *r, uint16_t top, uint16_t max, const pa_sweep_io_t *io)
{
	uint8_t over = 0u, n = 0u;
	uint32_t ms = 0u;
	pa_sweep_pt_t pt;
	// Both marks inside the point, on full carrier: a mark on a rising or falling carrier wouldn't demodulate
	bool marks = io->mark != NULL && r->mark_ms != 0u;
	while (pa_sweep_point(r, top, max, n, &over, &ms, &pt)) {
		if (!io->drive(0u)) {
			break;
		}
		io->delay_ms(pt.off_ms);
		if (!io->drive(pt.drive)) {
			break;
		}
		if (marks && pt.on_ms > 2u * r->mark_ms) {
			io->delay_ms(r->mark_ms);
			io->mark((n & 1u) != 0u ? -1 : 1);
			io->delay_ms(pt.on_ms - 2u * r->mark_ms);
			io->mark(0);
			io->delay_ms(r->mark_ms);
		} else {
			io->delay_ms(pt.on_ms);
		}
		n++;
	}
	io->drive(0u);
	if (io->mark != NULL) {
		io->mark(0);
	}
	return n;
}
