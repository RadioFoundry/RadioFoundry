// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR PA key-up from the power cal table (pa_keyup.h, docs/spec/pa-keyup.md)
#include "radio/pa_keyup.h"
#include "config.h"
#include <stddef.h>

pa_keyup_t pa_keyup_drives(const pa_keyup_cfg_t *c, uint16_t d, uint16_t lowest, uint16_t top, uint16_t max)
{
	pa_keyup_t k = { .boost = 0, .hold = 0, .drive = d };
	if (c == NULL || c->boost_us == 0u || d == 0u) {
		return k;
	}
	// 32-bit integer math: top <= 4095 or so, so every product stays far below 2^32
	uint32_t target = (uint32_t)top * c->top_pct / 100u;
	uint32_t boost = d;
	if (target > d) {
		boost += (target - d) * c->blend_pct / 100u;
	}
	if (boost > max) {
		boost = max;
	}
	uint32_t hold = (uint32_t)d * c->hold_pct / 100u;
	if (hold < lowest) {
		hold = lowest;
	}
	if (hold > d) {
		hold = d;
	}
	k.boost = (uint16_t)(boost < d ? d : boost);
	k.hold = (uint16_t)hold;
	return k;
}

bool pa_keyup_valid(const pa_keyup_cfg_t *c, uint16_t max_ahead_us)
{
	if (c == NULL || c->blend_pct > 100u || c->hold_pct > 100u || c->top_pct < 50u ||
	    c->top_pct > PA_KEYUP_TOP_MAX_PCT) {
		return false;
	}
	if (c->pa_us > max_ahead_us) {
		return false;
	}
	return c->boost_us == 0u || (c->boost_us <= max_ahead_us && c->hold_us <= c->boost_us && c->pa_us <= c->hold_us);
}

#if CONFIG_DMR
static const pa_keyup_cfg_t *board_cfg;	// the board's, kept by pa_keyup_init's caller
static pa_keyup_cfg_t cfg;
static uint16_t ahead_limit, drive_limit;
static bool on;
static pa_keyup_t last;

void pa_keyup_init(const pa_keyup_cfg_t *defaults, uint16_t max_ahead_us, uint16_t max_drive)
{
	if (!pa_keyup_valid(defaults, max_ahead_us)) {
		return;
	}
	board_cfg = defaults;
	cfg = *defaults;
	ahead_limit = max_ahead_us;
	drive_limit = max_drive;
	on = true;
}

const pa_keyup_cfg_t *pa_keyup_cfg(void)
{
	return on ? &cfg : NULL;
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
	cfg = *c;
	return true;
}

pa_keyup_t pa_keyup_plan(uint16_t d, uint16_t lowest, uint16_t top)
{
	last = pa_keyup_drives(on ? &cfg : NULL, d, lowest, top, drive_limit);
	return last;
}

pa_keyup_t pa_keyup_last(void)
{
	return last;
}
#endif
