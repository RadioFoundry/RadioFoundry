// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Beacon triggers and SmartBeaconing (aprs_beacon.h)
#include <string.h>
#include "aprs/aprs_beacon.h"
#include "aprs/aprs_fmt.h"

#define PTT_SETTLE_MS	1500	// after our transmission: give the channel (and a repeater's tail) a moment

void aprs_beacon_init(aprs_beacon_t *b, const aprs_beacon_cfg_t *cfg, uint32_t now_ms)
{
	memset(b, 0, sizeof(*b));
	b->last_ms = now_ms;	// time based beacons count from power-up
	b->last_course = 0xFFFFu;
	b->boot_pending = cfg->boot;
}

void aprs_beacon_manual(aprs_beacon_t *b, uint32_t now_ms)
{
	(void)now_ms;
	b->manual = true;
}

void aprs_beacon_ptt_end(aprs_beacon_t *b, const aprs_beacon_cfg_t *cfg, uint32_t now_ms)
{
	if (!cfg->after_ptt || (b->sent_once && now_ms - b->last_ms < (uint32_t)cfg->ptt_gap_s * 1000u)) {
		return;
	}
	b->ptt_end = true;
	b->ptt_end_ms = now_ms;
}

uint16_t aprs_smart_rate_s(const aprs_beacon_cfg_t *cfg, uint16_t speed_kmh)
{
	if (speed_kmh <= cfg->slow_kmh || speed_kmh == 0) {
		return cfg->slow_s;
	}
	if (speed_kmh >= cfg->fast_kmh) {
		return cfg->fast_s;
	}
	uint32_t r = (uint32_t)cfg->fast_s * cfg->fast_kmh / speed_kmh;
	return (uint16_t)(r > cfg->slow_s ? cfg->slow_s : r);
}

uint16_t aprs_smart_turn_deg(const aprs_beacon_cfg_t *cfg, uint16_t speed_kmh)
{
	uint32_t t = cfg->turn_deg + (speed_kmh != 0 ? (uint32_t)cfg->turn_slope / speed_kmh : 180u);
	return (uint16_t)(t > 180u ? 180u : t);
}

static uint16_t turn(uint16_t a, uint16_t b)
{
	uint16_t d = (uint16_t)(a > b ? a - b : b - a);
	return d > 180u ? (uint16_t)(360u - d) : d;
}

static uint32_t min_ms(const aprs_beacon_cfg_t *cfg)
{
	uint16_t m = cfg->min_s < APRS_MIN_INTERVAL_FLOOR_S ? APRS_MIN_INTERVAL_FLOOR_S : cfg->min_s;
	return (uint32_t)m * 1000u;
}

static bool smart_active(const aprs_beacon_cfg_t *cfg, const aprs_beacon_pos_t *pos)
{
	return cfg->smart && pos->speed_kmh != 0xFFFFu && cfg->slow_s != 0 && cfg->fast_s != 0;
}

uint8_t aprs_beacon_due(const aprs_beacon_t *b, const aprs_beacon_cfg_t *cfg, const aprs_beacon_pos_t *pos,
			uint32_t now_ms)
{
	if (b->manual && (b->manual_ms == 0 || now_ms - b->manual_ms >= APRS_MANUAL_FLOOR_S * 1000u)) {
		return APRS_WHY_MANUAL;
	}
	if (!cfg->auto_on || !pos->valid) {
		return APRS_WHY_NONE;
	}
	uint32_t since = now_ms - b->last_ms;	// from power-up until the first beacon
	if (cfg->boot && b->boot_pending && !b->sent_once) {
		return APRS_WHY_BOOT;
	}
	if (since < min_ms(cfg)) {
		return APRS_WHY_NONE;
	}
	if (cfg->after_ptt && b->ptt_end && now_ms - b->ptt_end_ms >= PTT_SETTLE_MS) {
		return APRS_WHY_PTT;
	}
	if (smart_active(cfg, pos)) {
		if (since >= (uint32_t)aprs_smart_rate_s(cfg, pos->speed_kmh) * 1000u) {
			return APRS_WHY_SMART;
		}
		if (pos->speed_kmh > cfg->slow_kmh && pos->course != 0xFFFFu && b->last_course != 0xFFFFu &&
		    since >= (uint32_t)cfg->turn_s * 1000u &&
		    turn(pos->course, b->last_course) > aprs_smart_turn_deg(cfg, pos->speed_kmh)) {
			return APRS_WHY_TURN;
		}
	} else if (cfg->interval_s != 0 && since >= (uint32_t)cfg->interval_s * 1000u) {
		return APRS_WHY_INTERVAL;
	}
	if (cfg->distance_m != 0 && b->have_last_pos &&
	    aprs_distance_m(b->last_lat, b->last_lon, pos->lat, pos->lon) >= cfg->distance_m) {
		return APRS_WHY_DISTANCE;
	}
	return APRS_WHY_NONE;
}

void aprs_beacon_sent(aprs_beacon_t *b, uint8_t why, const aprs_beacon_pos_t *pos, uint32_t now_ms)
{
	if (why == APRS_WHY_MANUAL) {
		b->manual = false;
		b->manual_ms = now_ms != 0 ? now_ms : 1;
	}
	b->sent_once = true;
	b->boot_pending = false;
	b->ptt_end = false;
	b->last_ms = now_ms;
	if (pos != NULL && pos->valid) {
		b->have_last_pos = true;
		b->last_lat = pos->lat;
		b->last_lon = pos->lon;
		b->last_course = pos->course;
	}
}

uint16_t aprs_beacon_next_s(const aprs_beacon_t *b, const aprs_beacon_cfg_t *cfg, const aprs_beacon_pos_t *pos,
			    uint32_t now_ms)
{
	if (!cfg->auto_on || !pos->valid) {
		return 0xFFFFu;
	}
	uint32_t period;
	if (smart_active(cfg, pos)) {
		period = aprs_smart_rate_s(cfg, pos->speed_kmh);
	} else if (cfg->interval_s != 0) {
		period = cfg->interval_s;
	} else {
		return 0xFFFFu;
	}
	uint32_t m = min_ms(cfg) / 1000u;
	if (period < m) {
		period = m;
	}
	uint32_t since_s = (now_ms - b->last_ms) / 1000u;
	return since_s >= period ? 0 : (uint16_t)(period - since_s);
}
