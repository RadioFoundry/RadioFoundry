// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DMR repeater roaming engine (docs/spec/dmr-roaming.md): the sites, the current site's average, the triggers,
// the search sequence and the choice. No hardware and no codeplug here: the radio core (radio_sm.c) builds the site
// list, tunes, applies the block list and tells the engine what it hears. CONFIG_DMR_ROAMING.
//
// Use: roam_set_sites() when the site list or the user's channel changes; roam_sample() every poll; roam_poll() every
// poll, acting on what it returns (ROAM_ACT_TUNE: tune site r->visit and call roam_tuned(); ROAM_ACT_PROBE: send a
// wake probe on the visited site and say whether it went with roam_probe_sent(); ROAM_ACT_DONE: the search is over,
// tune site r->cur if the radio isn't on it). roam_abort() when the user does anything during a search.
#ifndef RADIO_ROAM_H
#define RADIO_ROAM_H

#include <stdint.h>
#include <stdbool.h>

#define ROAM_SITES_MAX		16
#define ROAM_SETTLE_MS		400	// after a retune: longer than the call stack's repeater hold (300 ms)
#define ROAM_LISTEN_MS		1500	// listening per visited site
#define ROAM_PROBE_MS		2500	// ... when a wake probe was sent (three 600 ms wake-ups and the measurement)
#define ROAM_DWELL_MS		30000	// on a site at least this long before an automatic search
#define ROAM_WEAK_MS		5000	// below the threshold this long ...
#define ROAM_WEAK_SAMPLES	20	// ... with at least this many readings: the site is weak
#define ROAM_QUIET_MS		3000	// no search this soon after a reception or transmission ended (radio_sm.c)
#define ROAM_FRESH_MS		60000	// an average older than this is unknown
#define ROAM_RETRY_MS		30000	// back-off after a search that stayed, doubled each time ...
#define ROAM_RETRY_MAX_MS	240000	// ... up to this
#define ROAM_STALE_MS		600000	// wake probing: a site not heard this long is checked
#define ROAM_UNKNOWN_DBM	(-200)	// a signal not known, in the choice

#define ROAM_RSSI_DEFAULT	(-105)
#define ROAM_HYST_DEFAULT	6

typedef enum { ROAM_OFF, ROAM_WATCH, ROAM_SEARCH, ROAM_LOCKED } roam_state_t;
typedef enum { ROAM_WHY_NONE, ROAM_WHY_WEAK, ROAM_WHY_TX_FAIL, ROAM_WHY_MANUAL, ROAM_WHY_CHECK, ROAM_WHY_STALE } roam_why_t;
typedef enum { ROAM_PROBE_PASSIVE, ROAM_PROBE_WAKE } roam_probe_t;
typedef enum { ROAM_ACT_NONE, ROAM_ACT_TUNE, ROAM_ACT_PROBE, ROAM_ACT_DONE } roam_act_t;

typedef struct {
	int8_t rssi_dbm;	// threshold
	uint8_t hyst_db;	// hysteresis
	uint8_t probe;		// roam_probe_t
	uint16_t check_s;	// beacon check interval, 0 = off
} roam_cfg_t;

typedef struct {
	uint16_t pos;		// zone position
	uint16_t number;	// codeplug channel number
	int16_t rssi_dbm;	// the current site: its average; the others: their last measurement; 0 = none
	uint32_t heard_ms;	// when its outbound was last heard with our color code, 0 = never
} roam_site_t;

typedef struct {
	uint8_t state;		// roam_state_t
	uint8_t why;		// the search running, or the last one (roam_why_t)
	uint8_t pending;	// a latched trigger (roam_why_t)
	roam_cfg_t cfg;
	roam_site_t site[ROAM_SITES_MAX];
	uint8_t count;
	uint8_t cur;		// the site the radio is on (the origin during a search)
	uint8_t home;		// the site the user picked
	// Watching the current site
	int32_t avg_x16;	// average x 16
	bool have_avg;
	uint32_t avg_ms;	// time of its last reading
	uint32_t arrived_ms;	// on this site since
	uint32_t check_ms;	// last beacon check (or arrival)
	uint32_t weak_since;	// below the threshold since, 0 = not
	uint16_t weak_samples;
	uint32_t retry_until;	// no weak / stale search before this, 0 = none
	uint32_t retry_ms;	// the next back-off
	// Searching
	uint8_t visit;		// the site being visited
	uint16_t todo;		// sites still to visit (bits)
	uint16_t heard;		// sites heard during this search (bits)
	uint32_t tuned_ms;
	bool probed;		// a wake probe was asked for on this visit
	bool probe_sent;	// ... and it went out
	int16_t best;		// the visit's strongest reading, 0 = none
	// Counters (since boot)
	uint32_t searches, roams, stays, aborts, probes, triggers;
} roam_t;

void roam_init(roam_t *r, const roam_cfg_t *cfg);
void roam_set_cfg(roam_t *r, const roam_cfg_t *cfg);

// A new site list, the radio on site cur (-1: not on a site), home the user's site (-1: = cur). Off with fewer than
// two sites or not on one, else watching. A lock and the average survive when cur names the same channel as before
void roam_set_sites(roam_t *r, const roam_site_t *sites, uint8_t count, int cur, int home, uint32_t now_ms);

// Every poll: heard = the site's outbound is heard with our color code now, rssi_dbm the receiver's reading
void roam_sample(roam_t *r, uint32_t now_ms, bool heard, int16_t rssi_dbm);
// Latch a trigger (ROAM_WHY_TX_FAIL, ROAM_WHY_MANUAL; the others come from the samples and the timers). MANUAL
// clears a lock. False when roaming is off
bool roam_trigger(roam_t *r, uint8_t why);
// Lock to the current site (on) or unlock. False when roaming is off or searching
bool roam_lock(roam_t *r, bool on, uint32_t now_ms);

// Every poll. blocked: a block list reason (radio_sm.c), 0 = none; only starting a search looks at it. call: a call
// that passes the filters is playing (a qualifying candidate ends the search there)
roam_act_t roam_poll(roam_t *r, uint32_t now_ms, uint8_t blocked, bool call);
// The radio core tuned site r->visit
void roam_tuned(roam_t *r, uint32_t now_ms);
// After ROAM_ACT_PROBE: whether the probe went out (the visit then listens ROAM_PROBE_MS)
void roam_probe_sent(roam_t *r, bool sent);
// End a search now: back to the origin (r->cur, which the caller tunes). A weak, stale or failed-TX trigger stays
// latched for later
void roam_abort(roam_t *r, uint32_t now_ms);

static inline bool roam_searching(const roam_t *r) { return r->state == ROAM_SEARCH; }
// The current site's average, 0 when unknown (none yet, or older than ROAM_FRESH_MS)
int16_t roam_signal(const roam_t *r, uint32_t now_ms);

#endif
