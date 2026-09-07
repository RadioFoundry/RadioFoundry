// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DMR repeater roaming engine (docs/spec/dmr-roaming.md): the sites, the current site's average, the triggers,
// the search sequence, the beacon survey and the choice. No hardware and no codeplug here: the radio core
// (radio_sm.c) builds the site list, tunes, applies the block list and tells the engine what it hears. CONFIG_DMR_ROAMING.
//
// Use: roam_set_sites() when the site list or the user's channel changes; roam_sample() every poll; roam_poll() every
// poll, acting on what it returns (ROAM_ACT_TUNE: tune site r->visit and call roam_tuned(); ROAM_ACT_PROBE: send a
// wake probe on the visited site and say whether it went with roam_probe_sent(); ROAM_ACT_DONE: the search or the
// survey visit is over, tune site r->cur if the radio isn't on it). roam_abort() when the user does anything while
// the radio is away from its site (a search or a survey visit).
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
#define ROAM_HOLD_MS		300	// "heard" lasts this long after the last burst (= DMR_CALL_REPEATER_HOLD_MS)

// Beacons and the survey (docs/spec/dmr-roaming.md 4.8 - 4.10)
#define ROAM_BEACON_DEFAULT_S	60	// the repeaters' beacon interval (must match them), 0 = no beacons
#define ROAM_BEACON_MIN_S	10
#define ROAM_BEACON_MAX_S	600
#define ROAM_BEACON_LEN_DEFAULT	4	// the beacon's duration in s (must match the repeaters)
#define ROAM_BEACON_LEN_MAX	10
#define ROAM_WIN_TOL_MS		1000	// a beacon this close to the expected time belongs to the learned schedule
#define ROAM_WIN_LEAD_MS	400	// in the window, stay on the current site this long first (its beacon starts)
#define ROAM_WIN_TAIL_MS	300	// ... and be back on it this long before the window ends
#define ROAM_WIN_STALE		30	// intervals without the beacon heard: its schedule is forgotten (crystals drift little)
#define ROAM_CLEAN_MS		200	// silence before a keyup for the keyup's start to count as the beacon's start
// Readings after a survey retune count from here: the call stack keeps the last site's outbound "up" for its hold
// time (same color code), then the PLL, the modem's sync and the RSSI filter (about 120 ms) are done
#define ROAM_SURVEY_SETTLE_MS	(ROAM_HOLD_MS + 120)
#define ROAM_SURVEY_LISTEN_MS	250	// then listen at most this long per site ...
#define ROAM_SURVEY_READINGS	4	// ... or until this many readings (one per 25 ms poll)
#define ROAM_SURVEY_VISIT_MS	(ROAM_SURVEY_SETTLE_MS + ROAM_SURVEY_LISTEN_MS)
#define ROAM_SURVEY_DUTY	10	// percent of the time away from the current site, at most
#define ROAM_SURVEY_HOLD_MS	10000	// no survey this soon after traffic on the current site (doubled per busy score)
#define ROAM_SURVEY_BUSY_MAX	4	// busy score cap (hold-off up to 160 s)
#define ROAM_CONFIRM_MS		800	// a confirm visit listens this long after its settle time

#define ROAM_RSSI_DEFAULT	(-105)
#define ROAM_HYST_DEFAULT	6

typedef enum { ROAM_OFF, ROAM_WATCH, ROAM_SEARCH, ROAM_LOCKED, ROAM_SURVEY } roam_state_t;	// = RROAM_STATE_* (radio.h)
typedef enum { ROAM_WHY_NONE, ROAM_WHY_WEAK, ROAM_WHY_TX_FAIL, ROAM_WHY_MANUAL, ROAM_WHY_CHECK, ROAM_WHY_STALE } roam_why_t;
typedef enum { ROAM_PROBE_PASSIVE, ROAM_PROBE_WAKE } roam_probe_t;
// The survey: off, the sites beacon together in one window (a synchronized network), or each on its own schedule
typedef enum { ROAM_SURVEY_OFF, ROAM_SURVEY_SYNC, ROAM_SURVEY_SITE } roam_survey_t;
typedef enum { ROAM_VISIT_SURVEY, ROAM_VISIT_CONFIRM, ROAM_VISIT_DISCOVER } roam_visit_t;
typedef enum { ROAM_ACT_NONE, ROAM_ACT_TUNE, ROAM_ACT_PROBE, ROAM_ACT_DONE } roam_act_t;

typedef struct {
	int8_t rssi_dbm;	// threshold
	uint8_t hyst_db;	// hysteresis
	uint8_t probe;		// roam_probe_t
	uint16_t check_s;	// beacon check interval, 0 = off
	uint16_t beacon_s;	// the repeaters' beacon interval, 0 = none (ROAM_BEACON_MIN_S .. ROAM_BEACON_MAX_S)
	uint8_t beacon_len_s;	// their beacon duration (1 .. ROAM_BEACON_LEN_MAX)
	uint8_t survey;		// roam_survey_t
} roam_cfg_t;

typedef struct {
	uint16_t pos;		// zone position
	uint16_t number;	// codeplug channel number
	int16_t rssi_dbm;	// the current site: its average; the others: their last measurement; 0 = none
	int16_t surv_dbm;	// the survey's average for it, 0 = not heard (when surv_ms is set)
	uint32_t heard_ms;	// when its outbound was last heard with our color code, 0 = never
	uint32_t surv_ms;	// when the survey last measured it (heard, or missed twice), 0 = never
	uint32_t phase_ms;	// the start of its last beacon seen, 0 = unknown
	uint8_t miss;		// survey visits in a live window that did not hear it, in a row
} roam_site_t;

typedef struct {
	uint8_t state;		// roam_state_t
	uint8_t why;		// the search running, or the last one (roam_why_t)
	uint8_t pending;	// a latched trigger (roam_why_t)
	roam_cfg_t cfg;
	roam_site_t site[ROAM_SITES_MAX];
	uint8_t count;
	uint8_t cur;		// the site the radio is on (the origin during a search or a survey visit)
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
	// Searching and survey visits
	uint8_t visit;		// the site being visited
	uint16_t todo;		// sites still to visit (bits)
	uint16_t heard;		// sites heard during this search (bits)
	uint32_t tuned_ms;
	bool probed;		// a wake probe was asked for on this visit
	bool probe_sent;	// ... and it went out
	int16_t best;		// the visit's strongest reading, 0 = none
	uint8_t vkind;		// roam_visit_t of a survey visit
	uint8_t vreadings;	// readings this survey visit
	int32_t vsum;		// their sum
	uint32_t vend;		// the survey visit (or the round) ends by then
	bool vlive;		// the beacons are on the air this round (a site was heard)
	bool vbeacon;		// a beacon of the visited site was seen whole (a discovery visit can end)
	uint32_t away_since;	// the survey round started
	uint32_t survey_after;	// no survey round before this (the duty cycle), 0 = none
	bool force;		// stale data for a waiting trigger: survey every window, past the duty cycle
	uint32_t force_ms;	// ... since
	// The beacon schedule: the shared window (ROAM_SURVEY_SYNC), learned from any site's beacon
	uint32_t win_ms;	// the start of a beacon window
	uint8_t win_conf;	// beacons that matched it, 0 = unknown, 2+ = known
	uint32_t win_alt;	// a beacon off the schedule, 0 = none (two in step move the schedule)
	uint32_t win_seen_ms;	// the last beacon that matched
	uint32_t win_done;	// the window surveyed last
	uint32_t learn_ms;	// on this site with an unknown schedule since (a discovery visit after a while)
	// The keyup on the tuned site (beacon detection)
	uint8_t edge_site;
	bool up;
	bool up_clean;		// silence before it: its start is the keyup's start
	bool up_busy;		// traffic during it: not a beacon
	uint32_t up_since;
	uint32_t quiet_since;	// not heard since, 0 = not known yet
	uint32_t valid_ms;	// readings after a retune count from here
	// Traffic on the current site
	uint32_t busy_ms;	// last traffic, 0 = none
	uint8_t busy_score;
	bool busy;
	// A move picked from the survey data, waiting for its confirm visit
	uint8_t confirm;	// site + 1, 0 = none
	uint32_t confirm_at;
	// Counters (since boot)
	uint32_t searches, roams, stays, aborts, probes, triggers;
	uint32_t surveys, visits, cuts, beacons, confirms;
} roam_t;

void roam_init(roam_t *r, const roam_cfg_t *cfg);
void roam_set_cfg(roam_t *r, const roam_cfg_t *cfg);

// A new site list, the radio on site cur (-1: not on a site), home the user's site (-1: = cur). Off with fewer than
// two sites or not on one, else watching. A lock, the average and the survey data survive when the list names the
// same channels as before
void roam_set_sites(roam_t *r, const roam_site_t *sites, uint8_t count, int cur, int home, uint32_t now_ms);

// Every poll: heard = the tuned site's outbound is heard with our color code now, rssi_dbm the receiver's reading,
// busy = a call or data is heard on it (traffic, not a beacon)
void roam_sample(roam_t *r, uint32_t now_ms, bool heard, int16_t rssi_dbm, bool busy);
// Latch a trigger (ROAM_WHY_TX_FAIL, ROAM_WHY_MANUAL; the others come from the samples and the timers). MANUAL
// clears a lock. False when roaming is off
bool roam_trigger(roam_t *r, uint8_t why);
// Lock to the current site (on) or unlock. False when roaming is off or away
bool roam_lock(roam_t *r, bool on, uint32_t now_ms);

// Every poll. blocked: a block list reason (radio_sm.c), 0 = none; it stops a search or a survey round from starting,
// and ends a survey visit. call: a call that passes the filters is playing (a qualifying candidate ends the search
// there)
roam_act_t roam_poll(roam_t *r, uint32_t now_ms, uint8_t blocked, bool call);
// The radio core tuned site r->visit
void roam_tuned(roam_t *r, uint32_t now_ms);
// After ROAM_ACT_PROBE: whether the probe went out (the visit then listens ROAM_PROBE_MS)
void roam_probe_sent(roam_t *r, bool sent);
// End a search or a survey visit now: back to the origin (r->cur, which the caller tunes). A weak, stale or
// failed-TX trigger stays latched for later
void roam_abort(roam_t *r, uint32_t now_ms);

// Away from the current site: a search or a survey visit
static inline bool roam_searching(const roam_t *r) { return r->state == ROAM_SEARCH || r->state == ROAM_SURVEY; }
static inline bool roam_surveying(const roam_t *r) { return r->state == ROAM_SURVEY; }
// The current site's average, 0 when unknown (none yet, or older than ROAM_FRESH_MS)
int16_t roam_signal(const roam_t *r, uint32_t now_ms);
// The beacon window schedule is known (ROAM_SURVEY_SYNC); *next_ms the next window's start
bool roam_window(const roam_t *r, uint32_t now_ms, uint32_t *next_ms);
// How old survey data may be and still count (it follows the interval and how many sites a window holds)
uint32_t roam_fresh_ms(const roam_t *r);
// Site i's survey data is fresh
bool roam_site_fresh(const roam_t *r, uint8_t i, uint32_t now_ms);

#endif
