// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Power tuning: the table applied at key-up, the names, the limits and the preview (docs/spec/power-tune.md). The
// record behind the table is power_tune_store.c's
#include <string.h>
#include "board.h"
#include "hal_time.h"
#include "radio/power_tune.h"

#if CONFIG_POWER_TUNE

// One applied point in a word: frequency in 25 kHz units (bits 0-15), level (16-18), offset (19-31, signed)
#define P_F(w)		((uint32_t)((w) & 0xFFFFu) * 25000u)
#define P_LEVEL(w)	((uint8_t)(((w) >> 16) & 7u))
#define P_DELTA(w)	((int32_t)(w) >> 19)

typedef struct {
	uint8_t band_level;	// band << 4 | level
	char name[POWER_TUNE_LABEL_LEN + 1];
} label_t;

static uint32_t points[POWER_TUNE_POINTS];
static uint8_t n_points;
static label_t labels[POWER_TUNE_LABELS];
static uint8_t n_labels;
// The preview: level + 1 in bits 24-31 (0 none), offset in bits 0-15; set_ms when it was set
static volatile uint32_t preview;
static volatile uint32_t preview_ms;

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t level_drive(uint32_t hz, uint8_t level)
{
	return level < board.power_count ? board.radio->tx_drive_level(hz, level) : 0u;
}

static bool drives(void)
{
	return board.radio != NULL && board.radio->tx_drive_level != NULL && board.power_count != 0u;
}

// The range a tuned drive may reach: never above the top level's factory drive (the radio's rating), never below where
// the PA keeps control: the board's floor, else the lowest level's factory drive, which the calibration procedure put
// above it with margin. Always includes drive
static void bounds(uint32_t hz, uint8_t level, uint16_t drive, uint16_t *lo, uint16_t *hi)
{
	uint8_t top = (uint8_t)(board.power_count - 1u);
	uint16_t floor = board.radio->tx_drive_floor != NULL ? board.radio->tx_drive_floor(hz) : 0u;
	*hi = level == top ? drive : level_drive(hz, top);
	*lo = floor != 0u ? floor : level == 0u ? drive : level_drive(hz, 0);
	if (*lo > drive || *lo == 0u) {
		*lo = drive;
	}
	if (*hi < drive) {
		*hi = drive;
	}
}

void power_tune_limits(uint32_t hz, uint8_t level, power_tune_limits_t *l)
{
	memset(l, 0, sizeof(*l));
	if (!drives() || board_tx_band(hz) < 0 || (l->factory = level_drive(hz, level)) == 0u) {
		return;
	}
	bounds(hz, level, l->factory, &l->lo, &l->hi);
	l->tuned = power_tune_drive(hz, level, l->factory);
}

// The stored offset of level at hz: interpolated between the points of that level in hz's band, flat past the ends
static bool offset_at(uint32_t hz, int band, uint8_t level, int32_t *delta)
{
	int lo = -1, hi = -1;
	for (uint8_t i = 0; i < n_points; i++) {
		uint32_t w = points[i], f = P_F(w);
		if (P_LEVEL(w) != level || board_tx_band(f) != band) {
			continue;
		}
		if (f <= hz && (lo < 0 || f > P_F(points[lo]))) {
			lo = i;
		}
		if (f >= hz && (hi < 0 || f < P_F(points[hi]))) {
			hi = i;
		}
	}
	if (lo < 0 && hi < 0) {
		return false;
	}
	if (lo < 0 || hi < 0 || P_F(points[lo]) == P_F(points[hi])) {
		*delta = P_DELTA(points[lo >= 0 ? lo : hi]);
		return true;
	}
	int32_t f0 = (int32_t)(P_F(points[lo]) / 1000u), f1 = (int32_t)(P_F(points[hi]) / 1000u);
	int32_t d0 = P_DELTA(points[lo]), d1 = P_DELTA(points[hi]);
	int32_t num = (d1 - d0) * ((int32_t)(hz / 1000u) - f0), den = f1 - f0;
	*delta = d0 + (num + (num < 0 ? -den / 2 : den / 2)) / den;	// kHz: within 32 bits for any band
	return true;
}

uint16_t power_tune_drive(uint32_t hz, uint8_t level, uint16_t drive)
{
	if (drive == 0u || (n_points == 0u && preview == 0u)) {
		return drive;
	}
	int band = board_tx_band(hz);
	uint32_t pv = preview;
	int32_t delta;
	if (band < 0 || !drives()) {
		return drive;
	}
	if (pv != 0u && (pv >> 24) == (uint32_t)level + 1u && hal_millis() - preview_ms < POWER_TUNE_PREVIEW_MS) {
		delta = (int16_t)(pv & 0xFFFFu);
	} else if (!offset_at(hz, band, level, &delta)) {
		return drive;
	}
	uint16_t lo, hi;
	int32_t d = (int32_t)drive + delta;
	bounds(hz, level, drive, &lo, &hi);
	return (uint16_t)(d > hi ? hi : d < lo ? lo : d);
}

const char *power_tune_name(uint8_t level, uint32_t tx_hz)
{
	int band = board_tx_band(tx_hz);
	for (uint8_t i = 0; band >= 0 && i < n_labels; i++) {
		if (labels[i].band_level == (uint8_t)(band << 4 | level)) {
			return labels[i].name;
		}
	}
	return board_power_name(level, tx_hz);
}

bool power_tune_preview(uint8_t level, int16_t delta, uint32_t now_ms)
{
	if (level == 0xFFu) {
		preview = 0u;
		return true;
	}
	if (level >= board.power_count || level >= POWER_TUNE_LEVELS_MAX || delta > POWER_TUNE_DELTA_MAX || delta < -POWER_TUNE_DELTA_MAX) {
		return false;
	}
	preview = 0u;		// the time first: never a new offset with an old time
	preview_ms = now_ms;
	preview = ((uint32_t)level + 1u) << 24 | (uint16_t)delta;
	return true;
}

uint8_t power_tune_state(const uint8_t *p)
{
	uint32_t hz = get32(p) * 1000u;
	if (p[4] >= board.power_count || p[4] >= POWER_TUNE_LEVELS_MAX || board_tx_band(hz) < 0 || get32(p) / 25u > 0xFFFFu) {
		return POWER_TUNE_GONE;
	}
	return level_drive(hz, p[4]) == get16(p + 6) ? POWER_TUNE_APPLIED : POWER_TUNE_STALE;
}

// The applied table from a record (0 length: empty)
void power_tune_take(const uint8_t *r, size_t n)
{
	uint8_t np = 0, nl = 0;
	if (n != 0u && drives()) {
		for (uint8_t i = 0; i < r[9]; i++) {
			const uint8_t *p = r + POWER_TUNE_HDR + i * POWER_TUNE_POINT_SIZE;
			int16_t d = (int16_t)get16(p + 8);
			if (power_tune_state(p) == POWER_TUNE_APPLIED && d <= POWER_TUNE_DELTA_MAX && d >= -POWER_TUNE_DELTA_MAX) {
				points[np++] = (get32(p) + 12u) / 25u | (uint32_t)p[4] << 16 | (uint32_t)(d & 0x1FFF) << 19;
			}
		}
		for (uint8_t i = 0; i < r[10]; i++) {
			const uint8_t *p = r + POWER_TUNE_HDR + r[9] * POWER_TUNE_POINT_SIZE + i * POWER_TUNE_LABEL_SIZE;
			int band = board_tx_band(get32(p) * 1000u);
			if (power_tune_state(p) == POWER_TUNE_APPLIED && band >= 0 && band < 16 && p[12] != 0) {
				labels[nl].band_level = (uint8_t)(band << 4 | p[4]);
				memcpy(labels[nl].name, p + 12, POWER_TUNE_LABEL_LEN);
				labels[nl].name[POWER_TUNE_LABEL_LEN] = 0;
				nl++;
			}
		}
	}
	// One task writes; a reader between these sees a shorter table, never a torn entry
	n_points = 0;
	n_labels = 0;
	n_labels = nl;
	n_points = np;
}

bool power_tune_previewing(uint32_t now_ms)
{
	return preview != 0u && now_ms - preview_ms < POWER_TUNE_PREVIEW_MS;
}

void power_tune_counts(uint8_t *np, uint8_t *nl)
{
	*np = n_points;
	*nl = n_labels;
}

#endif
