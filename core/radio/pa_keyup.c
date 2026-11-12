// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR PA key-up from the power cal table (pa_keyup.h, docs/spec/pa-keyup.md)
#include "radio/pa_keyup.h"
#include "config.h"
#include <stddef.h>

pa_keyup_t pa_keyup_drives(const pa_keyup_cfg_t *c, uint16_t d, uint16_t top, uint16_t max)
{
	pa_keyup_t k = { .boost = 0, .hold = 0, .drive = d, .hold_us = 0 };
	if (c == NULL || c->boost_us == 0u || d == 0u) {
		return k;
	}
	// 32-bit integer math: drives <= 4095 or so, knee and slope_us 16 bits, so every product stays below 2^32
	uint32_t boost = (uint32_t)top * c->boost_pct / 100u;
	if (boost > max) {
		boost = max;
	}
	if (boost <= (uint32_t)c->dead + 1u || boost < d) {
		return k;	// a boost that would do nothing, or one under the step's drive: key at d
	}
	uint32_t over = d > c->dead ? (uint32_t)d - c->dead : 0u;
	uint32_t t = ((uint32_t)c->knee * 1000u + (uint32_t)c->slope_us * over) / (boost - c->dead);
	uint32_t room = (uint32_t)c->boost_us - c->drive_us;	// the boost ends by the point the drive goes to d
	if (t > room) {
		t = room;
	}
	k.boost = (uint16_t)boost;
	// The hold stays above the dead zone: under it the loop loses the boost's charge (pa-keyup.md 2). A step whose
	// own drive is in the dead zone holds at d
	uint32_t hold = (uint32_t)d * c->hold_pct / 100u;
	if (hold <= c->dead) {
		hold = d > c->dead ? (uint32_t)c->dead + 1u : d;
	}
	k.hold = (uint16_t)hold;
	k.hold_us = (uint16_t)(c->boost_us - t);
	return k;
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
	} else if (!pa_keyup_valid(c, ahead_limit)) {
		return false;
	}
	uint8_t n = (uint8_t)(cur ^ 1u);
	cfgs[n] = *c;
	cur = n;
	return true;
}

pa_keyup_t pa_keyup_plan(uint16_t d, uint16_t top, pa_keyup_cfg_t *used)
{
	pa_keyup_cfg_t c = cfgs[cur];
	if (used != NULL && on) {
		*used = c;
	}
	last = pa_keyup_drives(on ? &c : NULL, d, top, drive_limit);
	return last;
}

pa_keyup_t pa_keyup_last(void)
{
	return last;
}
#endif
