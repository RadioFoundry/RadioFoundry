// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DMR repeater roaming engine (roam.h, docs/spec/dmr-roaming.md 4)
#include <string.h>
#include "radio/roam.h"
#include "config.h"

#if CONFIG_DMR_ROAMING

static bool since(uint32_t now, uint32_t t, uint32_t ms)
{
	return (uint32_t)(now - t) >= ms;
}

static bool before(uint32_t now, uint32_t t)
{
	return (int32_t)(now - t) < 0;
}

static int iabs(int v)
{
	return v < 0 ? -v : v;
}

// ---- Beacon schedule helpers ----

static uint32_t interval_ms(const roam_t *r)
{
	return (uint32_t)r->cfg.beacon_s * 1000u;
}

static uint32_t len_ms(const roam_t *r)
{
	return (uint32_t)r->cfg.beacon_len_s * 1000u;
}

// The survey runs: on, and the repeaters beacon
static bool beacons(const roam_t *r)
{
	return r->cfg.survey != ROAM_SURVEY_OFF && r->cfg.beacon_s != 0 && r->cfg.beacon_len_s != 0;
}

// t's offset from the schedule anchored at a with period p, in [-p/2, p/2)
static int32_t phase_off(uint32_t t, uint32_t a, uint32_t p)
{
	int32_t m = (int32_t)(t - a) % (int32_t)p;
	if (m >= (int32_t)p / 2) {
		m -= (int32_t)p;
	} else if (m < -(int32_t)p / 2) {
		m += (int32_t)p;
	}
	return m;
}

// The last occurrence at or before t of the schedule anchored at a
static uint32_t sched_last(uint32_t t, uint32_t a, uint32_t p)
{
	int32_t m = (int32_t)(t - a) % (int32_t)p;
	if (m < 0) {
		m += (int32_t)p;
	}
	return t - (uint32_t)m;
}

static bool window_known(const roam_t *r, uint32_t now_ms)
{
	return beacons(r) && r->cfg.survey == ROAM_SURVEY_SYNC && r->win_conf >= 2 &&
	       !since(now_ms, r->win_seen_ms, ROAM_WIN_STALE * interval_ms(r));
}

bool roam_window(const roam_t *r, uint32_t now_ms, uint32_t *next_ms)
{
	if (!window_known(r, now_ms)) {
		return false;
	}
	if (next_ms != NULL) {
		*next_ms = sched_last(now_ms, r->win_ms, interval_ms(r)) + interval_ms(r);
	}
	return true;
}

static bool site_phase_known(const roam_t *r, uint8_t i, uint32_t now_ms)
{
	return r->site[i].phase_ms != 0 && !since(now_ms, r->site[i].phase_ms, ROAM_WIN_STALE * interval_ms(r));
}

// How long the current site's average stays known: an idle site is heard only in its beacons
static uint32_t avg_fresh_ms(const roam_t *r)
{
	uint32_t f = 2u * interval_ms(r) + len_ms(r);
	return beacons(r) && f > ROAM_FRESH_MS ? f : ROAM_FRESH_MS;
}

uint32_t roam_fresh_ms(const roam_t *r)
{
	if (!beacons(r) || r->count < 2) {
		return ROAM_FRESH_MS;
	}
	uint32_t p = interval_ms(r), others = r->count - 1u, f;
	if (r->cfg.survey == ROAM_SURVEY_SYNC) {
		uint32_t usable = len_ms(r) > ROAM_WIN_LEAD_MS + ROAM_WIN_TAIL_MS ? len_ms(r) - ROAM_WIN_LEAD_MS - ROAM_WIN_TAIL_MS : 0;
		uint32_t per = usable / ROAM_SURVEY_VISIT_MS;
		if (per == 0) {
			per = 1;
		}
		uint32_t rounds = (others + per - 1u) / per;
		// Windows between rounds: the duty cycle may skip some
		uint32_t gap = 1u + (usable * (100u - ROAM_SURVEY_DUTY) / ROAM_SURVEY_DUTY) / p;
		f = (rounds * gap + 2u) * p;
	} else {
		f = (others + 2u) * p;
	}
	return f > ROAM_FRESH_MS ? f : ROAM_FRESH_MS;
}

bool roam_site_fresh(const roam_t *r, uint8_t i, uint32_t now_ms)
{
	return i < r->count && r->site[i].surv_ms != 0 && !since(now_ms, r->site[i].surv_ms, roam_fresh_ms(r));
}

// ---- Setup ----

void roam_init(roam_t *r, const roam_cfg_t *cfg)
{
	memset(r, 0, sizeof(*r));
	r->cfg = *cfg;
	r->retry_ms = ROAM_RETRY_MS;
}

void roam_set_cfg(roam_t *r, const roam_cfg_t *cfg)
{
	bool schedule = cfg->beacon_s != r->cfg.beacon_s || cfg->beacon_len_s != r->cfg.beacon_len_s ||
			cfg->survey != r->cfg.survey;
	r->cfg = *cfg;
	if (schedule) {
		r->win_conf = 0;	// a new interval or mode: learn the schedule again
		r->win_alt = 0;
		r->survey_after = 0;
		r->force = false;
	}
}

// Readings on the tuned site count from settle ms on; a keyup seen before that is not ours
static void edge_reset(roam_t *r, uint8_t site, uint32_t now_ms, uint32_t settle)
{
	r->edge_site = site;
	r->up = r->up_busy = r->up_clean = false;
	r->quiet_since = 0;
	r->valid_ms = now_ms + settle;
}

static void avg_seed(roam_t *r, int16_t dbm, uint32_t now_ms)
{
	r->avg_x16 = (int32_t)dbm * 16;
	r->have_avg = dbm != 0;
	r->avg_ms = now_ms;
	r->weak_since = 0;
	r->weak_samples = 0;
}

void roam_set_sites(roam_t *r, const roam_site_t *sites, uint8_t count, int cur, int home, uint32_t now_ms)
{
	if (count > ROAM_SITES_MAX) {
		count = ROAM_SITES_MAX;
	}
	bool same = cur >= 0 && cur < count && r->state != ROAM_OFF && r->cur < r->count &&
		    r->site[r->cur].number == sites[cur].number;
	bool locked = same && r->state == ROAM_LOCKED;
	uint16_t home_number = r->home < r->count ? r->site[r->home].number : 0;
	// The survey data follows the channels (a rebuild after a CPS write keeps what is known about them)
	roam_site_t old[ROAM_SITES_MAX];
	uint8_t old_count = r->count, kept = 0;
	memcpy(old, r->site, sizeof(old));
	memmove(r->site, sites, count * sizeof(sites[0]));
	for (uint8_t i = 0; i < count; i++) {
		for (uint8_t k = 0; k < old_count; k++) {
			if (old[k].number == r->site[i].number) {
				r->site[i].surv_dbm = old[k].surv_dbm;
				r->site[i].surv_ms = old[k].surv_ms;
				r->site[i].phase_ms = old[k].phase_ms;
				r->site[i].miss = old[k].miss;
				if (r->site[i].heard_ms == 0) {
					r->site[i].heard_ms = old[k].heard_ms;
				}
				kept++;
				break;
			}
		}
	}
	if (kept == 0) {
		r->win_conf = 0;	// other repeaters: another network, another schedule
		r->win_alt = 0;
	}
	r->count = count;
	r->todo = r->heard = 0;
	r->confirm = 0;
	r->force = false;
	if (cur < 0 || cur >= count || count < 2) {
		r->state = ROAM_OFF;
		r->pending = ROAM_WHY_NONE;
		r->cur = r->home = 0;
		return;
	}
	r->cur = (uint8_t)cur;
	r->home = (uint8_t)(home >= 0 && home < count ? home : cur);
	if (same && home < 0) {
		for (uint8_t i = 0; i < count; i++) {
			if (r->site[i].number == home_number) {
				r->home = i;	// a rebuild keeps home where it was
			}
		}
	}
	r->state = locked ? ROAM_LOCKED : ROAM_WATCH;
	edge_reset(r, r->cur, now_ms, ROAM_SURVEY_SETTLE_MS);
	if (!same) {
		avg_seed(r, 0, now_ms);
		r->arrived_ms = r->check_ms = r->learn_ms = now_ms;
		r->pending = ROAM_WHY_NONE;
		r->retry_until = 0;
		r->retry_ms = ROAM_RETRY_MS;
	}
}

int16_t roam_signal(const roam_t *r, uint32_t now_ms)
{
	if (!r->have_avg || since(now_ms, r->avg_ms, avg_fresh_ms(r))) {
		return 0;
	}
	return (int16_t)(r->avg_x16 / 16);
}

static void latch(roam_t *r, uint8_t why)
{
	if (r->pending == ROAM_WHY_NONE || why == ROAM_WHY_MANUAL || why == ROAM_WHY_TX_FAIL) {
		if (r->pending != why) {
			r->triggers++;
		}
		r->pending = why;
	}
}

// ---- Beacon learning ----

// A beacon started at s: the shared window follows it (two off the schedule in step move it)
static void win_learn(roam_t *r, uint32_t s, uint32_t now_ms)
{
	uint32_t p = interval_ms(r);
	if (r->win_conf == 0 || since(now_ms, r->win_seen_ms, ROAM_WIN_STALE * p)) {
		r->win_ms = r->win_seen_ms = s;
		r->win_conf = 1;
		r->win_alt = 0;
		return;
	}
	int32_t off = phase_off(s, r->win_ms, p);
	if (iabs(off) <= ROAM_WIN_TOL_MS) {
		r->win_ms = s - (uint32_t)off + (uint32_t)(off / 4);	// a little toward this one (jitter)
		r->win_seen_ms = s;
		r->win_alt = 0;
		if (r->win_conf < 3) {
			r->win_conf++;
		}
	} else if (r->win_alt != 0 && iabs(phase_off(s, r->win_alt, p)) <= ROAM_WIN_TOL_MS) {
		r->win_ms = r->win_seen_ms = s;	// the network's schedule moved (a restart)
		r->win_conf = 2;
		r->win_alt = 0;
	} else {
		r->win_alt = s;
	}
}

// A keyup on site i ended: a beacon when nothing was carried and it lasted the beacon duration. start: its first
// reading (clean: silence before it, so the beacon's start), end: its last burst
static void keyup_end(roam_t *r, uint8_t i, uint32_t start, uint32_t end, bool clean, bool busy, uint32_t now_ms)
{
	if (busy || !beacons(r) || i >= r->count) {
		return;
	}
	int32_t dur = (int32_t)(end - start);
	int32_t d = (int32_t)len_ms(r);
	uint32_t s;
	if (clean) {
		if (dur < d / 2 || dur > d + 1500) {
			return;
		}
		s = start;
	} else {
		if (dur > d + 500) {
			return;		// longer than a beacon heard from its middle: traffic and hang time
		}
		s = end - (uint32_t)d;	// tuned in during it: the start from the end
	}
	r->beacons++;
	r->site[i].phase_ms = s != 0 ? s : 1;
	if (r->cfg.survey == ROAM_SURVEY_SYNC) {
		win_learn(r, s, now_ms);
	}
	if (r->state == ROAM_SURVEY && i == r->visit) {
		r->vlive = r->vbeacon = true;
	}
}

// A keyup started cleanly on a visited site near its expected time: refine the schedule with the start
static void rise_learn(roam_t *r, uint8_t i, uint32_t now_ms)
{
	uint32_t p = interval_ms(r);
	if (r->cfg.survey == ROAM_SURVEY_SYNC) {
		if (window_known(r, now_ms) && iabs(phase_off(now_ms, r->win_ms, p)) <= ROAM_WIN_TOL_MS) {
			win_learn(r, now_ms, now_ms);
		}
	} else if (site_phase_known(r, i, now_ms) && iabs(phase_off(now_ms, r->site[i].phase_ms, p)) <= ROAM_WIN_TOL_MS) {
		r->site[i].phase_ms = now_ms;
	}
}

// The keyup tracker on the tuned site
static void edge(roam_t *r, uint32_t now_ms, bool heard, bool busy)
{
	if (before(now_ms, r->valid_ms)) {
		return;
	}
	if (heard) {
		if (!r->up) {
			r->up = true;
			r->up_since = now_ms;
			r->up_clean = r->quiet_since != 0 && since(now_ms, r->quiet_since, ROAM_CLEAN_MS);
			r->up_busy = false;
			if (r->up_clean && r->state == ROAM_SURVEY && beacons(r)) {
				rise_learn(r, r->edge_site, now_ms);
			}
		}
		if (busy) {
			r->up_busy = true;
		}
		return;
	}
	if (r->up) {
		r->up = false;
		keyup_end(r, r->edge_site, r->up_since, now_ms - ROAM_HOLD_MS, r->up_clean, r->up_busy, now_ms);
		r->quiet_since = now_ms;
	} else if (r->quiet_since == 0) {
		r->quiet_since = now_ms;
	}
}

// ---- Samples ----

static void weak_check(roam_t *r, uint32_t now_ms)
{
	if (r->state == ROAM_WATCH && r->weak_since != 0 && since(now_ms, r->weak_since, ROAM_WEAK_MS) &&
	    r->weak_samples >= ROAM_WEAK_SAMPLES && roam_signal(r, now_ms) != 0) {
		latch(r, ROAM_WHY_WEAK);
	}
}

void roam_sample(roam_t *r, uint32_t now_ms, bool heard, int16_t rssi_dbm, bool busy)
{
	if (r->state == ROAM_OFF) {
		return;
	}
	heard = heard && rssi_dbm != 0;
	edge(r, now_ms, heard, busy);
	if (r->state == ROAM_WATCH || r->state == ROAM_LOCKED) {
		// Traffic on the current site: the survey holds off, longer when it is frequent
		if (busy) {
			if (!r->busy && r->busy_score < ROAM_SURVEY_BUSY_MAX) {
				r->busy_score++;
			}
			r->busy_ms = now_ms != 0 ? now_ms : 1;
		}
		r->busy = busy;
	}
	if (!heard) {
		return;
	}
	if (r->state == ROAM_SEARCH) {
		if (!since(now_ms, r->tuned_ms, ROAM_SETTLE_MS)) {
			return;	// the last site's outbound may still count as heard
		}
		if (r->best == 0 || rssi_dbm > r->best) {
			r->best = rssi_dbm;
		}
		r->site[r->visit].heard_ms = now_ms;
		r->heard |= (uint16_t)(1u << r->visit);
		return;
	}
	if (r->state == ROAM_SURVEY) {
		if (!before(now_ms, r->valid_ms)) {
			r->vreadings++;
			r->vsum += rssi_dbm;
			r->site[r->visit].heard_ms = now_ms;
		}
		return;
	}
	// Watching (or locked): the current site's average
	if (!r->have_avg || since(now_ms, r->avg_ms, avg_fresh_ms(r))) {
		avg_seed(r, rssi_dbm, now_ms);
	} else {
		r->avg_x16 += ((int32_t)rssi_dbm * 16 - r->avg_x16) / 8;
		r->avg_ms = now_ms;
	}
	roam_site_t *s = &r->site[r->cur];
	s->heard_ms = now_ms;
	s->rssi_dbm = (int16_t)(r->avg_x16 / 16);
	if (s->rssi_dbm < r->cfg.rssi_dbm) {
		if (r->weak_since == 0) {
			r->weak_since = now_ms != 0 ? now_ms : 1;
			r->weak_samples = 0;
		}
		if (r->weak_samples < 0xFFFF) {
			r->weak_samples++;
		}
		weak_check(r, now_ms);
	} else {
		r->weak_since = 0;
		r->weak_samples = 0;
		if (r->pending == ROAM_WHY_WEAK) {
			r->pending = ROAM_WHY_NONE;	// good again before the search could run
		}
		if (r->confirm != 0 && r->why == ROAM_WHY_WEAK) {
			r->confirm = 0;			// ... or before its confirm visit
		}
		if (s->rssi_dbm >= r->cfg.rssi_dbm + r->cfg.hyst_db) {
			r->retry_until = 0;
			r->retry_ms = ROAM_RETRY_MS;
		}
	}
}

bool roam_trigger(roam_t *r, uint8_t why)
{
	if (r->state == ROAM_OFF) {
		return false;
	}
	if (why == ROAM_WHY_MANUAL && r->state == ROAM_LOCKED) {
		r->state = ROAM_WATCH;
	}
	if (r->state == ROAM_LOCKED) {
		return false;
	}
	if (r->state == ROAM_SEARCH) {
		return true;
	}
	latch(r, why);
	return true;
}

bool roam_lock(roam_t *r, bool on, uint32_t now_ms)
{
	if (r->state == ROAM_OFF || roam_searching(r)) {
		return false;
	}
	if (on) {
		r->state = ROAM_LOCKED;
		r->pending = ROAM_WHY_NONE;
		r->confirm = 0;
	} else if (r->state == ROAM_LOCKED) {
		r->state = ROAM_WATCH;
		r->arrived_ms = r->check_ms = now_ms;	// the dwell starts again
		r->weak_since = 0;
	}
	return true;
}

// ---- The search (docs/spec/dmr-roaming.md 4.4) ----

// The next site to visit (lowest index first), or false when all are done
static bool next_visit(roam_t *r, uint32_t now_ms)
{
	for (uint8_t i = 0; i < r->count; i++) {
		if (r->todo & (1u << i)) {
			r->todo &= (uint16_t)~(1u << i);
			r->visit = i;
			r->tuned_ms = now_ms;
			r->best = 0;
			r->probed = r->probe_sent = false;
			r->vreadings = 0;
			r->vsum = 0;
			return true;
		}
	}
	return false;
}

static bool heard_now(const roam_t *r, uint8_t i)
{
	return (r->heard & (1u << i)) != 0 && r->site[i].rssi_dbm != 0;
}

// The choice (docs/spec/dmr-roaming.md 4.6): the strongest qualifying site if it beats the origin by the hysteresis,
// home if it is within the hysteresis of that one
static uint8_t choose(const roam_t *r, int16_t origin_avg)
{
	int t = r->cfg.rssi_dbm, h = r->cfg.hyst_db;
	int o = ROAM_UNKNOWN_DBM;
	if (heard_now(r, r->cur)) {
		o = r->site[r->cur].rssi_dbm;
	} else if (origin_avg != 0 && r->why != ROAM_WHY_TX_FAIL) {
		o = origin_avg;
	}
	int best = -1;
	for (uint8_t i = 0; i < r->count; i++) {
		if (i == r->cur || !heard_now(r, i) || r->site[i].rssi_dbm < t) {
			continue;
		}
		if (best < 0 || r->site[i].rssi_dbm > r->site[best].rssi_dbm) {
			best = i;
		}
	}
	if (best < 0) {
		return r->cur;
	}
	int b = r->site[best].rssi_dbm;
	uint8_t hm = r->home;
	if (hm != r->cur && heard_now(r, hm) && r->site[hm].rssi_dbm >= t && r->site[hm].rssi_dbm >= b - h &&
	    r->site[hm].rssi_dbm >= o + h) {
		return hm;
	}
	return b >= o + h ? (uint8_t)best : r->cur;
}

static void back_off(roam_t *r, uint32_t now_ms)
{
	if (r->why == ROAM_WHY_WEAK || r->why == ROAM_WHY_STALE) {
		r->retry_until = now_ms + r->retry_ms;
		if (r->retry_until == 0) {
			r->retry_until = 1;
		}
		r->retry_ms = r->retry_ms * 2 > ROAM_RETRY_MAX_MS ? ROAM_RETRY_MAX_MS : r->retry_ms * 2;
	}
}

static void moved(roam_t *r, uint8_t to, int16_t dbm, uint32_t now_ms)
{
	r->roams++;
	r->cur = to;
	r->arrived_ms = r->learn_ms = now_ms;
	avg_seed(r, dbm, now_ms);
	r->site[to].rssi_dbm = dbm;
	r->retry_until = 0;
	r->retry_ms = ROAM_RETRY_MS;
}

static void finish(roam_t *r, uint8_t choice, uint32_t now_ms)
{
	r->state = ROAM_WATCH;
	r->todo = 0;
	r->check_ms = now_ms;
	r->force = false;
	edge_reset(r, choice, now_ms, ROAM_SURVEY_SETTLE_MS);
	if (choice != r->cur) {
		moved(r, choice, r->site[choice].rssi_dbm, now_ms);
		return;
	}
	r->stays++;
	if (heard_now(r, r->cur)) {
		avg_seed(r, r->site[r->cur].rssi_dbm, now_ms);	// measured again during the search
	} else {
		r->weak_since = 0;
		r->weak_samples = 0;
	}
	back_off(r, now_ms);
}

static roam_act_t begin(roam_t *r, uint32_t now_ms)
{
	r->why = r->pending;
	r->pending = ROAM_WHY_NONE;
	r->confirm = 0;
	r->force = false;
	r->searches++;
	r->state = ROAM_SEARCH;
	r->heard = 0;
	r->todo = 0;
	bool origin_known = roam_signal(r, now_ms) != 0 && r->why != ROAM_WHY_TX_FAIL;
	for (uint8_t i = 0; i < r->count; i++) {
		if (i != r->cur || !origin_known) {
			r->todo |= (uint16_t)(1u << i);
		}
		if (i != r->cur) {
			r->site[i].rssi_dbm = 0;	// measured afresh
		}
	}
	if (!next_visit(r, now_ms)) {
		finish(r, r->cur, now_ms);
		return ROAM_ACT_DONE;
	}
	return ROAM_ACT_TUNE;
}

void roam_tuned(roam_t *r, uint32_t now_ms)
{
	r->tuned_ms = now_ms;
	edge_reset(r, r->visit, now_ms, r->state == ROAM_SURVEY ? ROAM_SURVEY_SETTLE_MS : ROAM_SETTLE_MS);
}

void roam_probe_sent(roam_t *r, bool sent)
{
	r->probe_sent = sent;
	if (sent) {
		r->probes++;
	}
}

// ---- The survey (docs/spec/dmr-roaming.md 4.8 - 4.10) ----

// No survey this soon after traffic on the current site; the hold-off doubles with each busy period
static bool survey_quiet(const roam_t *r, uint32_t now_ms)
{
	if (r->busy) {
		return false;
	}
	uint8_t score = r->busy_score > ROAM_SURVEY_BUSY_MAX ? ROAM_SURVEY_BUSY_MAX : r->busy_score;
	return r->busy_ms == 0 || since(now_ms, r->busy_ms, (uint32_t)ROAM_SURVEY_HOLD_MS << score);
}

// The other site whose survey data is oldest (never measured first), not in the mask
static int oldest_site(const roam_t *r, uint16_t mask)
{
	int best = -1;
	for (uint8_t i = 0; i < r->count; i++) {
		if (i == r->cur || (mask & (1u << i))) {
			continue;
		}
		if (best < 0) {
			best = i;
			continue;
		}
		uint32_t a = r->site[i].surv_ms, b = r->site[best].surv_ms;
		if (b != 0 && (a == 0 || before(a, b))) {
			best = i;
		}
	}
	return best;
}

static roam_act_t start_visits(roam_t *r, uint8_t kind, uint32_t vend, uint32_t now_ms)
{
	r->state = ROAM_SURVEY;
	r->vkind = kind;
	r->vend = vend;
	r->away_since = now_ms;
	r->vbeacon = false;
	if (kind != ROAM_VISIT_CONFIRM) {
		r->surveys++;
	}
	next_visit(r, now_ms);
	r->visits++;
	r->valid_ms = now_ms + ROAM_SURVEY_SETTLE_MS;
	return ROAM_ACT_TUNE;
}

// The end of a survey round or a confirm visit: back on the current site, the duty cycle sets the next round
static roam_act_t survey_end(roam_t *r, uint32_t now_ms)
{
	uint32_t away = now_ms - r->away_since;
	r->state = ROAM_WATCH;
	r->todo = 0;
	r->survey_after = now_ms + away * (100u - ROAM_SURVEY_DUTY) / ROAM_SURVEY_DUTY;
	if (r->survey_after == 0) {
		r->survey_after = 1;
	}
	if (r->busy_score > 0 && survey_quiet(r, r->away_since)) {
		r->busy_score--;	// a quiet round: the hold-off comes down again
	}
	edge_reset(r, r->cur, now_ms, ROAM_SURVEY_SETTLE_MS);	// the visited site's outbound counts for its hold time
	return ROAM_ACT_DONE;
}

// What the visit to site i measured goes into its survey data
static void record(roam_t *r, uint8_t i, uint32_t now_ms)
{
	roam_site_t *s = &r->site[i];
	if (r->vreadings != 0) {
		int16_t m = (int16_t)(r->vsum / r->vreadings);
		s->surv_dbm = roam_site_fresh(r, i, now_ms) && s->surv_dbm != 0 ? (int16_t)((s->surv_dbm + m) / 2) : m;
		s->surv_ms = now_ms;
		s->miss = 0;
		r->vlive = true;	// a site heard: the beacons are on the air
		if (r->cfg.survey == ROAM_SURVEY_SITE && r->vkind == ROAM_VISIT_SURVEY && site_phase_known(r, i, now_ms)) {
			// Heard on its schedule: the schedule still holds (a clean start refined it already)
			s->phase_ms = sched_last(now_ms, s->phase_ms, interval_ms(r));
		}
	} else if (r->vlive) {
		if (s->miss < 0xFF) {
			s->miss++;
		}
		if (s->miss >= 2) {
			s->surv_dbm = 0;	// missed twice in a live window: not heard here
			s->surv_ms = now_ms;
		}
	}
}

// When site i is next on the air: its beacon (the shared window, or its own schedule), else now (traffic only)
static uint32_t expected(const roam_t *r, uint8_t i, uint32_t now_ms)
{
	uint32_t p = interval_ms(r), anchor;
	if (window_known(r, now_ms)) {
		anchor = r->win_ms;
	} else if (beacons(r) && r->cfg.survey == ROAM_SURVEY_SITE && site_phase_known(r, i, now_ms)) {
		anchor = r->site[i].phase_ms;
	} else {
		return now_ms;
	}
	uint32_t w = sched_last(now_ms, anchor, p);
	if (now_ms - w + ROAM_SURVEY_SETTLE_MS + ROAM_CONFIRM_MS / 2 < len_ms(r)) {
		return now_ms;		// on the air now, with time for the visit
	}
	return w + p - ROAM_SURVEY_SETTLE_MS;
}

// A survey round, when it is time for one (docs/spec/dmr-roaming.md 4.9)
static roam_act_t try_survey(roam_t *r, uint32_t now_ms)
{
	if (!beacons(r) || (!r->force && r->survey_after != 0 && before(now_ms, r->survey_after)) || !survey_quiet(r, now_ms)) {
		return ROAM_ACT_NONE;
	}
	uint32_t p = interval_ms(r), d = len_ms(r);
	if (window_known(r, now_ms)) {
		// The shared window: as many sites as fit, oldest data first; the rest in the next windows
		uint32_t w = sched_last(now_ms, r->win_ms, p);
		uint32_t end = w + d - ROAM_WIN_TAIL_MS;
		if (w == r->win_done || before(now_ms, w + ROAM_WIN_LEAD_MS) || before(end, now_ms + ROAM_SURVEY_VISIT_MS)) {
			return ROAM_ACT_NONE;
		}
		uint32_t n = (end - now_ms) / ROAM_SURVEY_VISIT_MS;
		r->todo = 0;
		for (uint32_t k = 0; k < n; k++) {
			int i = oldest_site(r, r->todo);
			if (i < 0) {
				break;
			}
			r->todo |= (uint16_t)(1u << i);
		}
		if (r->todo == 0) {
			return ROAM_ACT_NONE;
		}
		r->win_done = w;
		uint32_t heard = r->site[r->cur].heard_ms;
		r->vlive = heard != 0 && !before(heard, w - ROAM_WIN_TOL_MS);	// our own site's beacon this window
		return start_visits(r, ROAM_VISIT_SURVEY, end, now_ms);
	}
	if (r->cfg.survey == ROAM_SURVEY_SITE) {
		// Each site on its own schedule: one visit, just before the oldest one's next beacon
		int i = oldest_site(r, 0);
		if (i < 0) {
			return ROAM_ACT_NONE;
		}
		r->todo = (uint16_t)(1u << i);
		r->vlive = false;
		if (site_phase_known(r, (uint8_t)i, now_ms)) {
			// Tuned in early enough for the keyup's start to be seen (it refines the schedule), half the tolerance
			uint32_t lead = ROAM_SURVEY_SETTLE_MS + ROAM_CLEAN_MS + ROAM_WIN_TOL_MS / 2;
			uint32_t e = sched_last(now_ms, r->site[i].phase_ms, p) + p;	// its next beacon
			if (e - now_ms > lead) {
				r->todo = 0;
				return ROAM_ACT_NONE;	// not yet
			}
			r->vlive = true;	// it beacons on this schedule: a miss counts
			return start_visits(r, ROAM_VISIT_SURVEY, e + ROAM_WIN_TOL_MS + ROAM_SURVEY_LISTEN_MS, now_ms);
		}
		return start_visits(r, ROAM_VISIT_DISCOVER, now_ms + ROAM_SURVEY_SETTLE_MS + p + d + ROAM_WIN_TOL_MS, now_ms);
	}
	// The shared window not known yet: our own site's beacons teach it. When our site is too weak to teach it (not
	// heard, or under the threshold) for a while, listen to another site through a whole interval
	int16_t avg = roam_signal(r, now_ms);
	if ((avg == 0 || avg < r->cfg.rssi_dbm) && since(now_ms, r->learn_ms, 2u * p + d + ROAM_WIN_TOL_MS)) {
		int i = oldest_site(r, 0);
		if (i < 0) {
			return ROAM_ACT_NONE;
		}
		r->learn_ms = now_ms;
		r->todo = (uint16_t)(1u << i);
		r->vlive = false;
		return start_visits(r, ROAM_VISIT_DISCOVER, now_ms + ROAM_SURVEY_SETTLE_MS + p + d + ROAM_WIN_TOL_MS, now_ms);
	}
	return ROAM_ACT_NONE;
}

// A survey visit in progress
static roam_act_t survey_step(roam_t *r, uint32_t now_ms, uint8_t blocked)
{
	if (blocked != 0) {
		// Traffic heard here (a networked call is likely on our own site too) or anything else: back at once
		r->cuts++;
		if (r->vkind == ROAM_VISIT_CONFIRM) {
			r->pending = r->why;	// decided again later
			r->confirm = 0;
		}
		return survey_end(r, now_ms);
	}
	uint32_t t = now_ms - r->tuned_ms;
	switch (r->vkind) {
	case ROAM_VISIT_CONFIRM: {
		if (r->vreadings < 2u * ROAM_SURVEY_READINGS && before(now_ms, r->vend)) {
			return ROAM_ACT_NONE;
		}
		uint8_t b = r->visit;
		int16_t m = r->vreadings != 0 ? (int16_t)(r->vsum / r->vreadings) : 0;
		int16_t avg = roam_signal(r, now_ms);
		int o = avg != 0 ? avg : ROAM_UNKNOWN_DBM;
		r->vlive = true;	// it was picked from fresh data in its beacon's time: a miss counts
		record(r, b, now_ms);
		r->confirm = 0;
		if (m != 0 && m >= r->cfg.rssi_dbm && m >= o + r->cfg.hyst_db) {
			survey_end(r, now_ms);
			moved(r, b, m, now_ms);
			edge_reset(r, b, now_ms, ROAM_SURVEY_SETTLE_MS);
			return ROAM_ACT_DONE;	// already there
		}
		r->stays++;
		back_off(r, now_ms);
		return survey_end(r, now_ms);
	}
	case ROAM_VISIT_DISCOVER:
		if (!(r->vbeacon && r->vreadings != 0) && before(now_ms, r->vend)) {
			return ROAM_ACT_NONE;
		}
		record(r, r->visit, now_ms);
		return survey_end(r, now_ms);
	default:
		if (r->vreadings < ROAM_SURVEY_READINGS && t < ROAM_SURVEY_VISIT_MS && before(now_ms, r->vend)) {
			return ROAM_ACT_NONE;
		}
		if (r->cfg.survey == ROAM_SURVEY_SITE && r->vreadings < ROAM_SURVEY_READINGS && before(now_ms, r->vend)) {
			return ROAM_ACT_NONE;	// its own schedule: listen to the end of the tolerance
		}
		record(r, r->visit, now_ms);
		if (r->todo != 0 && !before(r->vend, now_ms + ROAM_SURVEY_VISIT_MS) && next_visit(r, now_ms)) {
			r->visits++;
			r->valid_ms = now_ms + ROAM_SURVEY_SETTLE_MS;
			return ROAM_ACT_TUNE;
		}
		return survey_end(r, now_ms);
	}
}

// A weak or manual trigger with survey data (docs/spec/dmr-roaming.md 4.10). True: handled (a confirm visit
// planned, staying, or waiting for the survey); false: a full search
static bool survey_decide(roam_t *r, uint8_t p, uint32_t now_ms)
{
	int t = r->cfg.rssi_dbm, h = r->cfg.hyst_db;
	int16_t avg = roam_signal(r, now_ms);
	int o = avg != 0 ? avg : ROAM_UNKNOWN_DBM;
	bool all_fresh = true;
	int best = -1;
	for (uint8_t i = 0; i < r->count; i++) {
		if (i == r->cur) {
			continue;
		}
		if (!roam_site_fresh(r, i, now_ms)) {
			all_fresh = false;
			continue;
		}
		int v = r->site[i].surv_dbm;
		if (v != 0 && v >= t && (best < 0 || v > r->site[best].surv_dbm)) {
			best = i;
		}
	}
	if (best >= 0) {
		uint8_t hm = r->home;
		int b = r->site[best].surv_dbm, vh = r->site[hm].surv_dbm;
		if (hm != r->cur && roam_site_fresh(r, hm, now_ms) && vh != 0 && vh >= t && vh >= b - h && vh >= o + h) {
			best = hm;
		}
	}
	if (best >= 0 && r->site[best].surv_dbm >= o + h) {
		r->why = p;
		r->pending = ROAM_WHY_NONE;
		r->force = false;
		r->confirm = (uint8_t)(best + 1);
		r->confirm_at = expected(r, (uint8_t)best, now_ms);
		return true;
	}
	if (all_fresh) {
		if (p == ROAM_WHY_MANUAL) {
			return false;	// the user asked: a full search also catches traffic
		}
		r->why = p;		// nothing better anywhere: stay without a search
		r->pending = ROAM_WHY_NONE;
		r->force = false;
		r->stays++;
		back_off(r, now_ms);
		return true;
	}
	if (window_known(r, now_ms)) {
		// Stale: survey the stale sites in the next windows, past the duty cycle, then decide again
		if (!r->force) {
			r->force = true;
			r->force_ms = now_ms;
		} else if (since(now_ms, r->force_ms, roam_fresh_ms(r))) {
			r->force = false;
			return false;	// the survey couldn't do it: a full search
		}
		return true;
	}
	return false;
}

void roam_abort(roam_t *r, uint32_t now_ms)
{
	if (r->state == ROAM_SURVEY) {
		r->cuts++;
		if (r->vkind == ROAM_VISIT_CONFIRM) {
			r->pending = r->why;
			r->confirm = 0;
		}
		survey_end(r, now_ms);
		return;
	}
	if (r->state != ROAM_SEARCH) {
		return;
	}
	r->aborts++;
	r->state = ROAM_WATCH;
	r->todo = 0;
	if (r->why == ROAM_WHY_WEAK || r->why == ROAM_WHY_STALE || r->why == ROAM_WHY_TX_FAIL) {
		r->pending = r->why;	// again once the user is done
	}
	r->check_ms = now_ms;
	r->weak_since = 0;
	edge_reset(r, r->cur, now_ms, ROAM_SURVEY_SETTLE_MS);
}

roam_act_t roam_poll(roam_t *r, uint32_t now_ms, uint8_t blocked, bool call)
{
	if (r->state == ROAM_SURVEY) {
		return survey_step(r, now_ms, blocked);
	}
	if (r->state == ROAM_SEARCH) {
		uint32_t t = now_ms - r->tuned_ms;
		if (t < ROAM_SETTLE_MS) {
			return ROAM_ACT_NONE;
		}
		if (r->cfg.probe == ROAM_PROBE_WAKE && !r->probed && r->best == 0) {
			r->probed = true;
			return ROAM_ACT_PROBE;
		}
		if (call && r->visit != r->cur && r->best != 0 && r->best >= r->cfg.rssi_dbm) {
			r->site[r->visit].rssi_dbm = r->best;	// a call for us plays here: stay with it
			finish(r, r->visit, now_ms);
			return ROAM_ACT_DONE;
		}
		if (t < (uint32_t)ROAM_SETTLE_MS + (r->probe_sent ? ROAM_PROBE_MS : ROAM_LISTEN_MS)) {
			return ROAM_ACT_NONE;
		}
		r->site[r->visit].rssi_dbm = r->best;
		if (next_visit(r, now_ms)) {
			return ROAM_ACT_TUNE;
		}
		finish(r, choose(r, roam_signal(r, now_ms)), now_ms);
		return ROAM_ACT_DONE;
	}
	if (r->state != ROAM_WATCH) {
		return ROAM_ACT_NONE;
	}
	weak_check(r, now_ms);
	if (r->cfg.check_s != 0 && since(now_ms, r->check_ms, (uint32_t)r->cfg.check_s * 1000u)) {
		latch(r, ROAM_WHY_CHECK);
	}
	if (r->cfg.probe == ROAM_PROBE_WAKE) {
		uint32_t heard = r->site[r->cur].heard_ms;
		uint32_t ref = heard != 0 && (int32_t)(heard - r->arrived_ms) > 0 ? heard : r->arrived_ms;
		if (since(now_ms, ref, ROAM_STALE_MS)) {
			latch(r, ROAM_WHY_STALE);
		}
	}
	if (window_known(r, now_ms) || !beacons(r)) {
		r->learn_ms = now_ms;
	}
	if (blocked != 0) {
		return ROAM_ACT_NONE;
	}
	// A move picked from the survey data: its confirm visit when the site is next on the air
	if (r->confirm != 0) {
		uint8_t b = (uint8_t)(r->confirm - 1u);
		if (b >= r->count || b == r->cur) {
			r->confirm = 0;
		} else if (!before(now_ms, r->confirm_at)) {
			if (since(now_ms, r->confirm_at, len_ms(r) + ROAM_WIN_TOL_MS)) {
				r->confirm_at = expected(r, b, now_ms);	// missed its time (blocked): the next one
				return ROAM_ACT_NONE;
			}
			r->confirms++;
			r->todo = (uint16_t)(1u << b);
			return start_visits(r, ROAM_VISIT_CONFIRM, now_ms + ROAM_SURVEY_SETTLE_MS + ROAM_CONFIRM_MS, now_ms);
		} else {
			return ROAM_ACT_NONE;	// waiting for it: no survey meanwhile
		}
	}
	uint8_t p = r->pending;
	if (p != ROAM_WHY_NONE) {
		bool ready = true;
		if ((p == ROAM_WHY_WEAK || p == ROAM_WHY_CHECK || p == ROAM_WHY_STALE) && !since(now_ms, r->arrived_ms, ROAM_DWELL_MS)) {
			ready = false;
		}
		if ((p == ROAM_WHY_WEAK || p == ROAM_WHY_STALE) && r->retry_until != 0 && before(now_ms, r->retry_until)) {
			ready = false;
		}
		if (ready) {
			if (!(beacons(r) && (p == ROAM_WHY_WEAK || p == ROAM_WHY_MANUAL) && survey_decide(r, p, now_ms))) {
				return begin(r, now_ms);
			}
			if (r->confirm != 0) {
				return ROAM_ACT_NONE;	// the confirm visit starts at the next poll (or at the site's time)
			}
		}
	}
	return try_survey(r, now_ms);
}

#endif
