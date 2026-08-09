// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Battery monitor (battery.h)
#include <string.h>
#include "radio/battery.h"

void batt_init(batt_t *b)
{
	memset(b, 0, sizeof(*b));
}

static void set_level(batt_t *b, uint8_t level, uint32_t now_ms)
{
	b->level = level;
	b->below = 0;
	b->warned_ms = 0;
	b->critical_ms = level == BATT_CRITICAL ? (now_ms != 0 ? now_ms : 1) : 0;
}

batt_event_t batt_update(batt_t *b, uint16_t mv, uint16_t low_mv, uint16_t crit_mv, uint32_t now_ms)
{
	if (mv == 0) {
		return BATT_EV_NONE;
	}
	b->mv = mv;

	// Recovery (a charger, a fresh battery): straight up once clear of the hysteresis
	if (b->level == BATT_CRITICAL && (crit_mv == 0 || mv >= crit_mv + BATT_HYST_MV)) {
		set_level(b, (low_mv != 0 && mv < low_mv + BATT_HYST_MV) ? BATT_LOW : BATT_OK, now_ms);
	}
	if (b->level == BATT_LOW && (low_mv == 0 || mv >= low_mv + BATT_HYST_MV)) {
		set_level(b, BATT_OK, now_ms);
	}

	// Falling: BATT_CONFIRM readings in a row below the threshold (one sag is not a flat battery)
	uint8_t worse = b->level;
	if (crit_mv != 0 && mv < crit_mv) {
		worse = BATT_CRITICAL;
	} else if (low_mv != 0 && mv < low_mv) {
		worse = BATT_LOW;
	}
	if (worse > b->level) {
		if (++b->below >= BATT_CONFIRM) {
			set_level(b, worse, now_ms);
		}
	} else {
		b->below = 0;
	}

	if (b->level == BATT_OK) {
		return BATT_EV_NONE;
	}
	uint32_t repeat = b->level == BATT_CRITICAL ? BATT_CRIT_REPEAT_MS : BATT_LOW_REPEAT_MS;
	if (b->warned_ms == 0 || now_ms - b->warned_ms >= repeat) {
		b->warned_ms = now_ms != 0 ? now_ms : 1;
		return b->level == BATT_CRITICAL ? BATT_EV_CRITICAL : BATT_EV_LOW;
	}
	return BATT_EV_NONE;
}

uint32_t batt_critical_for(const batt_t *b, uint32_t now_ms)
{
	return b->critical_ms != 0 ? now_ms - b->critical_ms : 0;
}

uint8_t batt_backlight_pct(uint8_t brightness, uint16_t dim_s, uint16_t off_s, uint32_t idle_ms, uint8_t level)
{
	uint8_t pct = brightness > 100 ? 100 : brightness;
	if (off_s != 0 && idle_ms >= (uint32_t)off_s * 1000u) {
		return 0;
	}
	if (dim_s != 0 && idle_ms >= (uint32_t)dim_s * 1000u) {
		uint8_t dim = pct / 5;
		pct = dim < 5 ? 5 : dim;
	}
	if (level != BATT_OK && pct > BATT_DIM_PCT) {
		pct = BATT_DIM_PCT;
	}
	return pct;
}
