// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR PA key-up from the power cal table (pa_keyup.h, docs/spec/pa-keyup.md)
#include "radio/pa_keyup.h"
#include "config.h"
#include <stddef.h>

pa_keyup_t pa_keyup_drives(const pa_keyup_cfg_t *c, uint16_t d, uint16_t top, uint16_t ref, uint16_t max)
{
	pa_keyup_t k = { .boost = 0, .drive = d, .boost_us = 0, .pa_us = 0, .drive_us = 0 };
	if (c == NULL) {
		return k;
	}
	// 32-bit integer math: drives 12 bits (the DACs'), the constants 16, so every product stays below 2^32
	uint32_t dead = (uint32_t)ref * c->dead_pm / 1000u;
	uint32_t over = d > dead ? (uint32_t)d - dead : 0u;
	uint32_t span = top > dead ? (uint32_t)top - dead : 0u;
	uint32_t q = (uint32_t)c->knee_us * span + (uint32_t)c->slope_us * over;	// us x counts
	uint32_t lead = over != 0u ? q / over : c->window_us;	// a drive in the dead zone: nothing to time, the earliest
	uint32_t b = d;
	uint32_t ease = (uint32_t)ref * c->ease_pm / 1000u;
	if (ease != 0u && over > ease) {
		// Over the fastest approach: up on a = dead + ease as a level of that drive would be, then its own drive
		// for the time the law gives it from a's level to its own
		uint32_t step = (uint32_t)c->slope_us * (over - ease) / over;
		// Not a boost, so not the window's: the board's earliest point bounds it (pa_keyup_plan, pa_keyup_steps)
		uint32_t start = ((uint32_t)c->knee_us * span + (uint32_t)c->slope_us * ease) / ease + step;
		if (start > 0xFFFFu) {
			start = 0xFFFFu;
		}
		uint32_t on = c->pa_us < start ? c->pa_us : start;
		k.boost = (uint16_t)(dead + ease);
		k.boost_us = (uint16_t)start;
		k.pa_us = (uint16_t)on;
		k.drive_us = (uint16_t)(step < on ? step : on);
		return k;
	}
	if (lead > c->window_us) {
		lead = c->window_us;
		uint32_t room = (uint32_t)c->window_us - c->settle_us;	// pa_keyup_valid: above 0
		uint32_t ceiling = (uint32_t)top * PA_KEYUP_BOOST_MAX_PCT / 100u;
		if (ceiling > max) {
			ceiling = max;
		}
		if (ease != 0u && ceiling > dead + ease) {
			ceiling = dead + ease;	// d or over: this level is not over the fastest approach
		}
		b = dead + (q + room - 1u) / room;	// rounded up: the boost never short of the window
		if (b > ceiling) {
			b = ceiling;	// the level comes up late: the window is too short for it
		}
	}
	uint32_t pa = c->pa_us < lead ? c->pa_us : lead;
	k.pa_us = (uint16_t)pa;
	if (b > d || pa < lead) {	// a boost, or the drive ahead of the PA: the loop charged at d before the PA is on
		k.boost = (uint16_t)(b > d ? b : d);
		k.boost_us = (uint16_t)lead;
		k.drive_us = (uint16_t)(b > d ? c->settle_us : pa);
		if (k.drive_us > k.pa_us) {
			k.drive_us = k.pa_us;	// the PA no later than the drive's return: the points in the enum's order
		}
	}
	return k;
}

uint8_t pa_keyup_points(const pa_keyup_t *k, pa_keyup_point_t out[PA_KEYUP_POINTS])
{
	if (k->boost == 0u) {
		out[0] = (pa_keyup_point_t){ .us = k->pa_us, .kind = PA_KEYUP_PT_PA };
		return 1u;
	}
	out[0] = (pa_keyup_point_t){ .us = k->boost_us, .kind = PA_KEYUP_PT_BOOST };
	out[1] = (pa_keyup_point_t){ .us = k->pa_us, .kind = PA_KEYUP_PT_PA };
	out[2] = (pa_keyup_point_t){ .us = k->drive_us, .kind = PA_KEYUP_PT_DRIVE };
	return PA_KEYUP_POINTS;
}

uint8_t pa_keyup_insert(pa_keyup_point_t *pt, uint8_t n, uint16_t us, uint8_t kind)
{
	uint8_t at = n;
	while (at > 0u && pt[at - 1u].us <= us) {	// ahead of every point at its time or later
		pt[at] = pt[at - 1u];
		at--;
	}
	pt[at] = (pa_keyup_point_t){ .us = us, .kind = kind };
	return (uint8_t)(n + 1u);
}

uint8_t pa_keyup_steps(const pa_keyup_point_t *pt, uint8_t n, uint16_t ref_us, uint16_t min_us,
		       pa_keyup_step_t out[PA_KEYUP_POINTS_MAX])
{
	if (n == 0u) {
		return 0u;
	}
	// The first step no earlier than min_us after the reference
	uint16_t latest = ref_us > min_us ? (uint16_t)(ref_us - min_us) : (uint16_t)0u;
	uint16_t first = (uint32_t)pt[0].us + min_us < ref_us ? pt[0].us : latest;
	uint8_t m = 0u;
	out[0] = (pa_keyup_step_t){ .us = first, .end = 1u };
	for (uint8_t i = 1u; i < n; i++) {
		// Only points at one time share a step: whether a later one goes with it depends on when the handler runs,
		// and the timer counts from its last update (pa_keyup_step_wait), not from this step
		if (pt[i].us < out[m].us) {
			out[++m] = (pa_keyup_step_t){ .us = pt[i].us, .end = (uint8_t)(i + 1u) };
		} else {
			out[m].end = (uint8_t)(i + 1u);
		}
	}
	return (uint8_t)(m + 1u);
}

bool pa_keyup_valid(const pa_keyup_cfg_t *c, uint16_t max_ahead_us)
{
	return c != NULL && c->window_us <= max_ahead_us && (c->settle_us < c->window_us || c->window_us == 0u);
}

#if CONFIG_DMR
static const pa_keyup_cfg_t *board_cfg;	// the board's, kept by pa_keyup_init's caller
// Two copies: a set writes the one not in use, then switches, so a key-up reads one whole set (a set comes from the
// debug link at a person's pace, never twice within one key-up's copy)
static pa_keyup_cfg_t cfgs[2];
static volatile uint8_t cur;
static uint16_t ahead_limit, drive_limit;
static bool on;
static bool custom;	// the debug link's constants: a band's don't replace them
static pa_keyup_t last;
static uint16_t last_top, last_ref;

void pa_keyup_init(const pa_keyup_cfg_t *defaults, uint16_t max_ahead_us, uint16_t max_drive)
{
	if (!pa_keyup_valid(defaults, max_ahead_us)) {
		return;
	}
	board_cfg = defaults;
	cfgs[cur] = *defaults;
	ahead_limit = max_ahead_us;
	drive_limit = max_drive;
	on = true;
}

const pa_keyup_cfg_t *pa_keyup_cfg(void)
{
	return on ? &cfgs[cur] : NULL;
}

bool pa_keyup_set(const pa_keyup_cfg_t *c)
{
	if (!on) {
		return false;
	}
	if (c == NULL) {
		c = board_cfg;
		custom = false;
	} else if (!pa_keyup_valid(c, ahead_limit)) {
		return false;
	} else {
		custom = true;
	}
	uint8_t n = (uint8_t)(cur ^ 1u);
	cfgs[n] = *c;
	cur = n;
	return true;
}

void pa_keyup_select(const pa_keyup_cfg_t *defaults)
{
	if (!on || !pa_keyup_valid(defaults, ahead_limit)) {
		return;
	}
	board_cfg = defaults;
	if (!custom) {
		uint8_t n = (uint8_t)(cur ^ 1u);	// as pa_keyup_set: the copy not in use, then the switch
		cfgs[n] = *defaults;
		cur = n;
	}
}

pa_keyup_t pa_keyup_plan(uint16_t d, uint16_t top, uint16_t ref)
{
	pa_keyup_cfg_t c = cfgs[cur];
	last = pa_keyup_drives(on ? &c : NULL, d, top, ref, drive_limit);
	if (last.boost_us > ahead_limit) {	// an eased level over the board's earliest point: from there, and late
		last.boost_us = ahead_limit;
		if (last.pa_us > ahead_limit) {
			last.pa_us = ahead_limit;
		}
		if (last.drive_us > last.pa_us) {
			last.drive_us = last.pa_us;
		}
	}
	last_top = top;
	last_ref = ref;
	return last;
}

pa_keyup_t pa_keyup_last(uint16_t *top, uint16_t *ref)
{
	if (top != NULL) {
		*top = last_top;
	}
	if (ref != NULL) {
		*ref = last_ref;
	}
	return last;
}

void pa_keyup_limits(uint16_t *max_ahead_us, uint16_t *max_drive)
{
	*max_ahead_us = ahead_limit;
	*max_drive = drive_limit;
}
#endif
