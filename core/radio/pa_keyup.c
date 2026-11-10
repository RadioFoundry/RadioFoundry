// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR PA key-up from the power cal table (pa_keyup.h, docs/spec/pa-keyup.md)
#include "radio/pa_keyup.h"
#include "config.h"
#include <stddef.h>

pa_keyup_t pa_keyup_drives(const pa_keyup_cfg_t *c, uint16_t d, uint16_t top, uint16_t floor, uint16_t max)
{
	pa_keyup_t k = { .boost = 0, .hold = 0, .drive = d, .hold_us = 0 };
	if (c == NULL || c->boost_us == 0u || d == 0u || d < floor) {
		return k;
	}
	// 32-bit integer math: drives <= 4095 or so, knee and slope_us 16 bits, so every product stays below 2^32
	uint32_t boost = (uint32_t)top * c->boost_pct / 100u;
	if (boost > max) {
		boost = max;
	}
	uint32_t dead = (uint32_t)floor + c->dead;
	if (boost <= dead + 1u || boost < d) {
		return k;	// a boost that would do nothing, or one under the step's drive: key at d
	}
	uint32_t over = d > dead ? (uint32_t)d - dead : 0u;
	uint32_t t = ((uint32_t)c->knee * 1000u + (uint32_t)c->slope_us * over) / (boost - dead);
	uint32_t room = (uint32_t)c->boost_us - c->drive_us;	// the boost ends by the point the drive goes to d
	if (t > room) {
		t = room;
	}
	k.boost = (uint16_t)boost;
	// The hold stays above the dead zone: under it the loop loses the boost's charge (pa-keyup.md 2). A step whose
	// own drive is in the dead zone holds at d. On a board with a floor both count from it
	uint32_t hold = floor + (uint32_t)(d - floor) * c->hold_pct / 100u;
	if (hold <= dead) {
		hold = d > dead ? dead + 1u : d;
	}
	k.hold = (uint16_t)hold;
	k.hold_us = (uint16_t)(c->boost_us - t);
	return k;
}

uint8_t pa_keyup_points(const pa_keyup_cfg_t *c, const pa_keyup_t *k, pa_keyup_point_t out[PA_KEYUP_POINTS])
{
	if (k->boost == 0u) {
		out[0] = (pa_keyup_point_t){ .us = c->pa_us, .kind = PA_KEYUP_PT_PA };
		return 1u;
	}
	const uint16_t us[PA_KEYUP_POINTS] = { c->boost_us, c->pa_us, k->hold_us, c->drive_us };	// in the enum's order
	for (uint8_t i = 0; i < PA_KEYUP_POINTS; i++) {
		uint8_t at = i;	// insertion keeps the enum's order among equal times
		while (at > 0u && out[at - 1u].us < us[i]) {
			out[at] = out[at - 1u];
			at--;
		}
		out[at] = (pa_keyup_point_t){ .us = us[i], .kind = i };
	}
	return PA_KEYUP_POINTS;
}

bool pa_keyup_valid(const pa_keyup_cfg_t *c, uint16_t max_ahead_us)
{
	if (c == NULL || c->hold_pct > 100u || c->boost_pct < 50u || c->boost_pct > PA_KEYUP_BOOST_MAX_PCT) {
		return false;
	}
	if (c->pa_us > max_ahead_us) {
		return false;
	}
	return c->boost_us == 0u || (c->boost_us <= max_ahead_us && c->pa_us <= c->boost_us && c->drive_us <= c->boost_us);
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

pa_keyup_t pa_keyup_plan(uint16_t d, uint16_t top, uint16_t floor, pa_keyup_cfg_t *used)
{
	pa_keyup_cfg_t c = cfgs[cur];
	if (used != NULL && on) {
		*used = c;
	}
	last = pa_keyup_drives(on ? &c : NULL, d, top, floor, drive_limit);
	return last;
}

pa_keyup_t pa_keyup_last(void)
{
	return last;
}
#endif
