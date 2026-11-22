// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Battery monitor (docs/spec/radio-features.md 5): the board's battery voltage against its low and critical levels,
// with hysteresis and repeat warnings, and the backlight level that follows from idle time and battery state. Pure
// logic: the radio core feeds it readings (never during or just after a transmission, whose load pulls the voltage
// down) and acts on what it returns.
#ifndef RADIO_BATTERY_H
#define RADIO_BATTERY_H

#include <stdint.h>
#include <stdbool.h>

typedef enum { BATT_OK, BATT_LOW, BATT_CRITICAL } batt_level_t;
typedef enum { BATT_EV_NONE, BATT_EV_LOW, BATT_EV_CRITICAL } batt_event_t;

#define BATT_HYST_MV		200	// back to a better level only this far above the threshold
#define BATT_CONFIRM		3	// readings in a row below a threshold before the level drops
#define BATT_LOW_REPEAT_MS	120000	// warning again while low
#define BATT_CRIT_REPEAT_MS	30000	// ... and while critical
#define BATT_DIM_PCT		30	// backlight cap while the battery is low or critical
#define BATT_SPAN_MV		400	// empty and full with the owner's offsets at least this far apart, else without

typedef struct {
	uint8_t level;		// batt_level_t
	uint8_t below;		// readings in a row below the next level's threshold
	uint32_t warned_ms;	// last warning, 0 = none at this level
	uint32_t critical_ms;	// when the level became critical, 0 = not critical
} batt_t;

void batt_init(batt_t *b);
// One reading (mV, 0 = none: ignored). low_mv / crit_mv from the board, 0 = that warning off. Returns a warning to
// sound now (a new level, or the repeat interval elapsed)
batt_event_t batt_update(batt_t *b, uint16_t mv, uint16_t low_mv, uint16_t crit_mv, uint32_t now_ms);
// How long the battery has been critical (0 = not)
uint32_t batt_critical_for(const batt_t *b, uint32_t now_ms);

// Charge percent: a Li-ion discharge curve (resting voltage) stretched between empty_mv (0) and full_mv (100), each
// moved by the owner's offset (d_empty, d_full: radio_tune_batt) unless that brings them closer than BATT_SPAN_MV
uint8_t batt_pct(uint16_t mv, uint16_t empty_mv, uint16_t full_mv, int16_t d_empty, int16_t d_full);
// A reading corrected by the owner's ADC gain trim, permille (radio_tune_batt)
static inline uint16_t batt_scale(uint16_t mv, int16_t gain)
{
	return (uint16_t)((int32_t)mv + (int32_t)mv * gain / 1000);
}

// Backlight percent: brightness while active, a fifth of it (at least 5) after dim_s without activity, off after off_s
// (0 = never dims / never goes off), capped at BATT_DIM_PCT when the battery is low or critical
uint8_t batt_backlight_pct(uint8_t brightness, uint16_t dim_s, uint16_t off_s, uint32_t idle_ms, uint8_t level);

#endif
