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

void roam_init(roam_t *r, const roam_cfg_t *cfg)
{
	memset(r, 0, sizeof(*r));
	r->cfg = *cfg;
	r->retry_ms = ROAM_RETRY_MS;
}

void roam_set_cfg(roam_t *r, const roam_cfg_t *cfg)
{
	r->cfg = *cfg;
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
	memmove(r->site, sites, count * sizeof(sites[0]));
	r->count = count;
	r->todo = r->heard = 0;
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
	if (!same) {
		avg_seed(r, 0, now_ms);
		r->arrived_ms = r->check_ms = now_ms;
		r->pending = ROAM_WHY_NONE;
		r->retry_until = 0;
		r->retry_ms = ROAM_RETRY_MS;
	}
}

int16_t roam_signal(const roam_t *r, uint32_t now_ms)
{
	if (!r->have_avg || since(now_ms, r->avg_ms, ROAM_FRESH_MS)) {
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

void roam_sample(roam_t *r, uint32_t now_ms, bool heard, int16_t rssi_dbm)
{
	if (!heard || rssi_dbm == 0) {
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
	if (r->state == ROAM_OFF) {
		return;
	}
	// Watching (or locked): the current site's average
	if (!r->have_avg || since(now_ms, r->avg_ms, ROAM_FRESH_MS)) {
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
		if (r->state == ROAM_WATCH && since(now_ms, r->weak_since, ROAM_WEAK_MS) &&
		    r->weak_samples >= ROAM_WEAK_SAMPLES) {
			latch(r, ROAM_WHY_WEAK);
		}
	} else {
		r->weak_since = 0;
		r->weak_samples = 0;
		if (r->pending == ROAM_WHY_WEAK) {
			r->pending = ROAM_WHY_NONE;	// good again before the search could run
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
	if (r->state == ROAM_LOCKED || r->state == ROAM_SEARCH) {
		return r->state == ROAM_SEARCH;
	}
	latch(r, why);
	return true;
}

bool roam_lock(roam_t *r, bool on, uint32_t now_ms)
{
	if (r->state == ROAM_OFF || r->state == ROAM_SEARCH) {
		return false;
	}
	if (on) {
		r->state = ROAM_LOCKED;
		r->pending = ROAM_WHY_NONE;
	} else if (r->state == ROAM_LOCKED) {
		r->state = ROAM_WATCH;
		r->arrived_ms = r->check_ms = now_ms;	// the dwell starts again
		r->weak_since = 0;
	}
	return true;
}

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

static void finish(roam_t *r, uint8_t choice, uint32_t now_ms)
{
	r->state = ROAM_WATCH;
	r->todo = 0;
	r->check_ms = now_ms;
	if (choice != r->cur) {
		r->roams++;
		r->cur = choice;
		r->arrived_ms = now_ms;
		avg_seed(r, r->site[choice].rssi_dbm, now_ms);
		r->retry_until = 0;
		r->retry_ms = ROAM_RETRY_MS;
		return;
	}
	r->stays++;
	if (heard_now(r, r->cur)) {
		avg_seed(r, r->site[r->cur].rssi_dbm, now_ms);	// measured again during the search
	} else {
		r->weak_since = 0;
		r->weak_samples = 0;
	}
	if (r->why == ROAM_WHY_WEAK || r->why == ROAM_WHY_STALE) {
		r->retry_until = now_ms + r->retry_ms;
		if (r->retry_until == 0) {
			r->retry_until = 1;
		}
		r->retry_ms = r->retry_ms * 2 > ROAM_RETRY_MAX_MS ? ROAM_RETRY_MAX_MS : r->retry_ms * 2;
	}
}

static roam_act_t begin(roam_t *r, uint32_t now_ms)
{
	r->why = r->pending;
	r->pending = ROAM_WHY_NONE;
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
}

void roam_probe_sent(roam_t *r, bool sent)
{
	r->probe_sent = sent;
	if (sent) {
		r->probes++;
	}
}

void roam_abort(roam_t *r, uint32_t now_ms)
{
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
}

roam_act_t roam_poll(roam_t *r, uint32_t now_ms, uint8_t blocked, bool call)
{
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
	if (r->pending == ROAM_WHY_NONE || blocked != 0) {
		return ROAM_ACT_NONE;
	}
	uint8_t p = r->pending;
	if ((p == ROAM_WHY_WEAK || p == ROAM_WHY_CHECK || p == ROAM_WHY_STALE) && !since(now_ms, r->arrived_ms, ROAM_DWELL_MS)) {
		return ROAM_ACT_NONE;
	}
	if ((p == ROAM_WHY_WEAK || p == ROAM_WHY_STALE) && r->retry_until != 0 && (int32_t)(now_ms - r->retry_until) < 0) {
		return ROAM_ACT_NONE;
	}
	return begin(r, now_ms);
}

#endif
