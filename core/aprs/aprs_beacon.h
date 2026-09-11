// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// When to send our position (docs/spec/aprs.md 7): the trigger sources (a fixed interval, SmartBeaconing, distance
// moved, after our own transmission, a key press, the first fix after power-up) under one hard minimum interval.
// Pure: it takes the time, the position and the events, and says "send now, because ...". The caller (aprs.c) sends
// and reports back with aprs_beacon_sent().
//
// SmartBeaconing is the algorithm Tony Arnerich (KD7TA) and Steve Bragg (KA9MVA) published for the HamHUD tracker
// and released for anyone to use: the rate follows the speed between a slow and a fast setting, and a turn sharper
// than (min angle + slope / speed) sends at once after a minimum time ("corner pegging").
#ifndef ODR_APRS_BEACON_H
#define ODR_APRS_BEACON_H

#include <stdint.h>
#include <stdbool.h>

#define APRS_MIN_INTERVAL_FLOOR_S	10	// no configuration sends more often than this
#define APRS_MANUAL_FLOOR_S		5	// "send now" pressed again sooner is ignored

typedef enum {
	APRS_WHY_NONE, APRS_WHY_INTERVAL, APRS_WHY_SMART, APRS_WHY_TURN, APRS_WHY_DISTANCE, APRS_WHY_PTT,
	APRS_WHY_MANUAL, APRS_WHY_BOOT, APRS_WHY_COUNT,
} aprs_why_t;

typedef struct {
	bool auto_on;		// automatic beacons at all (the master "APRS TX" switch; manual works without it)
	uint16_t interval_s;	// fixed interval, 0 = off
	bool smart;		// SmartBeaconing (replaces the interval while a speed is known)
	uint16_t fast_kmh, slow_kmh;	// speeds for the fast and slow rates
	uint16_t fast_s, slow_s;	// rates at those speeds
	uint8_t turn_deg;	// minimum turn angle
	uint16_t turn_slope;	// degrees x km/h, divided by the speed and added to it (400 at 20 km/h adds 20 degrees)
	uint8_t turn_s;		// minimum time between turn beacons
	uint16_t distance_m;	// moved this far since the last beacon, 0 = off
	bool after_ptt;		// after our own transmission ends ...
	uint16_t ptt_gap_s;	// ... if the last beacon is at least this old
	bool boot;		// the first valid position after power-up
	uint16_t min_s;		// hard minimum between automatic beacons (at least APRS_MIN_INTERVAL_FLOOR_S)
} aprs_beacon_cfg_t;

typedef struct {
	bool valid;
	int32_t lat, lon;	// 1e-7 degree
	uint16_t speed_kmh;	// 0xFFFF unknown
	uint16_t course;	// 0..359, 0xFFFF unknown
} aprs_beacon_pos_t;

typedef struct {
	bool sent_once;
	uint32_t last_ms;	// last beacon
	bool have_last_pos;
	int32_t last_lat, last_lon;
	uint16_t last_course;	// course at the last beacon (turns), 0xFFFF unknown
	bool manual;		// a key press waiting
	bool ptt_end;		// our transmission ended ...
	uint32_t ptt_end_ms;	// ... at
	bool boot_pending;	// waiting for the first valid position
	uint32_t manual_ms;	// last manual press honored
} aprs_beacon_t;

void aprs_beacon_init(aprs_beacon_t *b, const aprs_beacon_cfg_t *cfg, uint32_t now_ms);
// Events
void aprs_beacon_manual(aprs_beacon_t *b, uint32_t now_ms);
// Our transmission ended: a beacon follows if after_ptt is set and the last one is at least ptt_gap_s old (else the
// event is dropped, not kept for later)
void aprs_beacon_ptt_end(aprs_beacon_t *b, const aprs_beacon_cfg_t *cfg, uint32_t now_ms);
// Due now? The reason (APRS_WHY_NONE: not yet). Call every tick or so; it doesn't change state
uint8_t aprs_beacon_due(const aprs_beacon_t *b, const aprs_beacon_cfg_t *cfg, const aprs_beacon_pos_t *pos,
			uint32_t now_ms);
// The beacon for why went out (or was handed to the radio) from pos
void aprs_beacon_sent(aprs_beacon_t *b, uint8_t why, const aprs_beacon_pos_t *pos, uint32_t now_ms);
// The SmartBeaconing rate in seconds at a speed (exposed for the tests and the view)
uint16_t aprs_smart_rate_s(const aprs_beacon_cfg_t *cfg, uint16_t speed_kmh);
// The turn threshold in degrees at a speed
uint16_t aprs_smart_turn_deg(const aprs_beacon_cfg_t *cfg, uint16_t speed_kmh);
// Seconds until the next time based beacon (interval or SmartBeaconing rate), 0 = due, 0xFFFF = none planned
uint16_t aprs_beacon_next_s(const aprs_beacon_t *b, const aprs_beacon_cfg_t *cfg, const aprs_beacon_pos_t *pos,
			    uint32_t now_ms);

#endif
