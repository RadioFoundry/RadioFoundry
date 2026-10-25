// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR repeater roaming (docs/spec/dmr-roaming.md): the engine (core/radio/roam.c) on its own, then through the radio
// core (radio_sm.c) against a fake board whose repeaters sit on frequencies: each one, while up, sends idle bursts
// with base station sync and its color code through the simulated modem, at a signal level the test sets; a sleeping
// repeater wakes on a BS_Dwn_Act CSBK sent on its input frequency
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "radio/radio_sm.h"
#include "radio/roam.h"
#include "dmr/call/dmr_call.h"
#include "dmr/call/dmr_lc.h"
#include "dmr/ambe/ambeVoice.h"
#include "sim/dmr_sim_modem.h"
#include "settings/settings.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ======================================================================================================================
// The engine on its own
// ======================================================================================================================

// The classic engine (no survey) for the first tests; the survey's own tests set beacons
static int iabs_t(int v) { return v < 0 ? -v : v; }
static uint32_t phase_of(uint32_t t) { return t % 60000u; }

static const roam_cfg_t cfg_default = { ROAM_RSSI_DEFAULT, ROAM_HYST_DEFAULT, ROAM_PROBE_PASSIVE, 0, 0, 0, ROAM_SURVEY_OFF };

static void sites_n(roam_t *r, uint8_t n, int cur, uint32_t t)
{
	roam_site_t s[ROAM_SITES_MAX];
	memset(s, 0, sizeof(s));
	for (uint8_t i = 0; i < n; i++) {
		s[i].pos = i;
		s[i].number = (uint16_t)(i + 1);
	}
	roam_set_sites(r, s, n, cur, -1, t);
}

// Feed the current site's reading every 25 ms for ms
static uint32_t feed(roam_t *r, uint32_t t, uint32_t ms, int16_t dbm)
{
	for (uint32_t e = 0; e < ms; e += 25) {
		t += 25;
		roam_sample(r, t, true, dbm, false);
	}
	return t;
}

// Run a search to its end: rssi[i] what site i gives (0 = silent); returns the end time
static uint32_t search(roam_t *r, uint32_t t, const int16_t *rssi, roam_act_t first)
{
	roam_act_t a = first;
	for (int guard = 0; guard < 2000 && a != ROAM_ACT_DONE; guard++) {
		if (a == ROAM_ACT_TUNE) {
			roam_tuned(r, t);
		} else if (a == ROAM_ACT_PROBE) {
			roam_probe_sent(r, false);
		}
		t += 25;
		uint8_t at = r->state == ROAM_SEARCH ? r->visit : r->cur;
		roam_sample(r, t, rssi[at] != 0, rssi[at], false);
		a = roam_poll(r, t, 0, false);
	}
	return t;
}

static void test_engine_basics(void)
{
	roam_t r;
	roam_init(&r, &cfg_default);
	CHECK(r.state == ROAM_OFF);
	sites_n(&r, 1, 0, 1000);
	CHECK(r.state == ROAM_OFF);		// one site: nothing to roam to
	sites_n(&r, 3, -1, 1000);
	CHECK(r.state == ROAM_OFF);		// not on a site
	sites_n(&r, 3, 0, 1000);
	CHECK(r.state == ROAM_WATCH && r.cur == 0 && r.home == 0);
	CHECK(!roam_trigger(&(roam_t){ 0 }, ROAM_WHY_MANUAL));	// off: no trigger

	// The average follows the readings; weak only after ROAM_WEAK_MS with enough readings
	uint32_t t = feed(&r, 1000, 2000, -90);
	CHECK(roam_signal(&r, t) == -90);
	t = feed(&r, t, 2000, -115);
	CHECK(roam_signal(&r, t) <= -113 && r.weak_since != 0 && r.pending == ROAM_WHY_NONE);
	t = feed(&r, t, ROAM_WEAK_MS, -115);
	CHECK(r.pending == ROAM_WHY_WEAK);
	// ... but no search before the dwell
	CHECK(roam_poll(&r, t, 0, false) == ROAM_ACT_NONE);
	t = feed(&r, t, ROAM_DWELL_MS, -115);
	// ... nor while blocked
	CHECK(roam_poll(&r, t, 1, false) == ROAM_ACT_NONE && r.state == ROAM_WATCH);
	// Good again before it ran: the trigger is dropped
	t = feed(&r, t, 1000, -80);
	CHECK(r.pending == ROAM_WHY_NONE && r.weak_since == 0);
	CHECK(roam_poll(&r, t, 0, false) == ROAM_ACT_NONE);
	// The average goes stale without readings
	CHECK(roam_signal(&r, t + ROAM_FRESH_MS) == 0);

	// A rebuild naming the same current channel keeps the average and a lock; another channel starts afresh
	CHECK(roam_lock(&r, true, t) && r.state == ROAM_LOCKED);
	sites_n(&r, 3, 0, t);
	CHECK(r.state == ROAM_LOCKED && roam_signal(&r, t) != 0);
	sites_n(&r, 3, 1, t);
	CHECK(r.state == ROAM_WATCH && roam_signal(&r, t) == 0 && r.cur == 1 && r.home == 1);
}

static void test_engine_choice(void)
{
	roam_t r;
	uint32_t t;

	// Weak: the strongest qualifying site wins; the origin (known) is not visited again
	roam_init(&r, &cfg_default);
	sites_n(&r, 4, 0, 1000);
	t = feed(&r, 1000, ROAM_DWELL_MS + ROAM_WEAK_MS + 1000, -112);
	CHECK(r.pending == ROAM_WHY_WEAK);
	roam_act_t a = roam_poll(&r, t, 0, false);
	CHECK(a == ROAM_ACT_TUNE && r.state == ROAM_SEARCH && r.visit == 1 && r.searches == 1 && r.why == ROAM_WHY_WEAK);
	t = search(&r, t, (int16_t[]){ -112, -95, -80, 0 }, a);
	CHECK(r.state == ROAM_WATCH && r.cur == 2 && r.roams == 1 && r.home == 0);
	CHECK(roam_signal(&r, t) == -80);	// seeded with its measurement
	// The search took about three visits of settle + listen (the origin was not visited)
	CHECK(r.site[3].rssi_dbm == 0 && r.site[1].rssi_dbm == -95);

	// Hysteresis: a candidate over the threshold but within H of the origin: stay, and back off
	roam_init(&r, &cfg_default);
	sites_n(&r, 3, 0, 1000);
	t = feed(&r, 1000, ROAM_DWELL_MS + ROAM_WEAK_MS + 1000, -108);
	a = roam_poll(&r, t, 0, false);
	CHECK(a == ROAM_ACT_TUNE);
	t = search(&r, t, (int16_t[]){ -108, -104, 0 }, a);
	CHECK(r.cur == 0 && r.stays == 1 && r.roams == 0 && r.retry_until != 0);
	// Still weak: latched again, but the back-off holds the next search
	t = feed(&r, t, ROAM_WEAK_MS + 500, -108);
	CHECK(r.pending == ROAM_WHY_WEAK && roam_poll(&r, t, 0, false) == ROAM_ACT_NONE);
	t = feed(&r, t, ROAM_RETRY_MS, -108);
	CHECK(roam_poll(&r, t, 0, false) == ROAM_ACT_TUNE);
	t = search(&r, t, (int16_t[]){ -108, -104, 0 }, ROAM_ACT_TUNE);
	CHECK(r.stays == 2 && r.retry_ms == ROAM_RETRY_MS * 4);	// doubled twice
	// Under the threshold: never a candidate, however much stronger than the origin
	roam_init(&r, &cfg_default);
	sites_n(&r, 2, 0, 1000);
	t = feed(&r, 1000, ROAM_DWELL_MS + ROAM_WEAK_MS + 1000, -124);
	t = search(&r, t, (int16_t[]){ -124, -106 }, roam_poll(&r, t, 0, false));
	CHECK(r.cur == 0 && r.stays == 1);

	// Home preferred when within H of the best and H better than the origin
	roam_init(&r, &cfg_default);
	sites_n(&r, 3, 0, 1000);			// home = site 0
	t = feed(&r, 1000, 1000, -90);
	CHECK(roam_trigger(&r, ROAM_WHY_MANUAL));
	t = search(&r, t, (int16_t[]){ -90, -70, 0 }, roam_poll(&r, t, 0, false));
	CHECK(r.cur == 1 && r.home == 0);		// moved away from home
	t = feed(&r, t, ROAM_DWELL_MS + ROAM_WEAK_MS + 1000, -110);
	CHECK(r.pending == ROAM_WHY_WEAK);
	t = search(&r, t, (int16_t[]){ -76, -110, -74 }, roam_poll(&r, t, 0, false));
	CHECK(r.cur == 0 && r.roams == 2);		// home (-76) within 6 dB of site 2 (-74)
	// ... and not when the best is more than H stronger
	t = feed(&r, t, 1000, -90);
	roam_trigger(&r, ROAM_WHY_MANUAL);
	t = search(&r, t, (int16_t[]){ -90, -70, 0 }, roam_poll(&r, t, 0, false));
	t = feed(&r, t, ROAM_DWELL_MS + ROAM_WEAK_MS + 1000, -110);
	t = search(&r, t, (int16_t[]){ -90, -110, -74 }, roam_poll(&r, t, 0, false));
	CHECK(r.cur == 2);

	// Failed transmission: the origin is visited too (its average does not count) and picked when best
	roam_init(&r, &cfg_default);
	sites_n(&r, 2, 0, 1000);
	t = feed(&r, 1000, 1000, -70);
	CHECK(roam_trigger(&r, ROAM_WHY_TX_FAIL));
	a = roam_poll(&r, t, 0, false);
	CHECK(a == ROAM_ACT_TUNE && r.visit == 0 && r.why == ROAM_WHY_TX_FAIL);	// TX_FAIL: no dwell needed
	t = search(&r, t, (int16_t[]){ 0, -100 }, a);
	CHECK(r.cur == 1);				// the origin silent: the other site, whatever the old average

	// A call for us heard on a visited site ends the search there
	roam_init(&r, &cfg_default);
	sites_n(&r, 3, 0, 1000);
	t = feed(&r, 1000, 1000, -100);
	roam_trigger(&r, ROAM_WHY_MANUAL);
	a = roam_poll(&r, t, 0, false);
	roam_tuned(&r, t);
	t += ROAM_SETTLE_MS + 25;
	roam_sample(&r, t, true, -92, false);
	CHECK(roam_poll(&r, t, 0, true) == ROAM_ACT_DONE && r.cur == 1 && r.roams == 1);
}

static void test_engine_lock_abort(void)
{
	roam_t r;
	uint32_t t;
	roam_init(&r, &cfg_default);
	sites_n(&r, 3, 0, 1000);
	CHECK(roam_lock(&r, true, 1000));
	t = feed(&r, 1000, ROAM_DWELL_MS + ROAM_WEAK_MS + 1000, -120);
	CHECK(r.pending == ROAM_WHY_NONE && roam_poll(&r, t, 0, false) == ROAM_ACT_NONE);	// locked: no weak trigger
	CHECK(!roam_trigger(&r, ROAM_WHY_TX_FAIL) && r.state == ROAM_LOCKED);	// nor a failed TX
	CHECK(roam_trigger(&r, ROAM_WHY_MANUAL) && r.state == ROAM_WATCH);	// roam now ends the lock
	roam_act_t a = roam_poll(&r, t, 0, false);
	CHECK(a == ROAM_ACT_TUNE && r.why == ROAM_WHY_MANUAL);
	CHECK(!roam_lock(&r, true, t));			// not while searching
	roam_abort(&r, t);
	CHECK(r.state == ROAM_WATCH && r.aborts == 1 && r.pending == ROAM_WHY_NONE && r.cur == 0);	// manual is not kept

	// An aborted weak search is latched again
	t = feed(&r, t, ROAM_DWELL_MS + ROAM_WEAK_MS + 1000, -120);
	CHECK(r.pending == ROAM_WHY_WEAK);
	CHECK(roam_poll(&r, t, 0, false) == ROAM_ACT_TUNE);
	roam_abort(&r, t);
	CHECK(r.pending == ROAM_WHY_WEAK && r.aborts == 2);

	// Unlocking restarts the dwell
	CHECK(roam_lock(&r, true, t) && roam_lock(&r, false, t + 5));
	CHECK(r.state == ROAM_WATCH && r.arrived_ms == t + 5);

	// Beacon check: a search every check_s even on a good site; with wake probing, a site unheard for
	// ROAM_STALE_MS is searched
	roam_cfg_t c = cfg_default;
	c.check_s = 60;
	roam_init(&r, &c);
	sites_n(&r, 2, 0, 1000);
	t = feed(&r, 1000, 59000, -80);
	CHECK(roam_poll(&r, t, 0, false) == ROAM_ACT_NONE);
	t = feed(&r, t, 1100, -80);
	CHECK(roam_poll(&r, t, 0, false) == ROAM_ACT_TUNE && r.why == ROAM_WHY_CHECK);
	c.check_s = 0;
	c.probe = ROAM_PROBE_WAKE;
	roam_init(&r, &c);
	sites_n(&r, 2, 0, 1000);
	t = 1000 + ROAM_STALE_MS - 25;
	CHECK(roam_poll(&r, t, 0, false) == ROAM_ACT_NONE);
	t += 50;
	CHECK(roam_poll(&r, t, 0, false) == ROAM_ACT_TUNE && r.why == ROAM_WHY_STALE);
	// The visit asks for a probe once the settle time is over, and listens longer when it went out
	roam_tuned(&r, t);
	CHECK(roam_poll(&r, t + 100, 0, false) == ROAM_ACT_NONE);
	CHECK(roam_poll(&r, t + ROAM_SETTLE_MS, 0, false) == ROAM_ACT_PROBE);
	roam_probe_sent(&r, true);
	CHECK(r.probes == 1);
	CHECK(roam_poll(&r, t + ROAM_SETTLE_MS + ROAM_LISTEN_MS + 25, 0, false) == ROAM_ACT_NONE);
	CHECK(roam_poll(&r, t + ROAM_SETTLE_MS + ROAM_PROBE_MS + 25, 0, false) == ROAM_ACT_TUNE);
}

// ======================================================================================================================
// The survey on its own: sites that key up only for their beacons
// ======================================================================================================================

typedef struct {
	int16_t dbm;		// 0 = not on the air at all
	uint32_t phase;		// first beacon start
	uint32_t period;	// 0 = always up
	uint32_t len;
	int jitter;		// each beacon starts up to this many ms early or late
	bool busy;		// carrying traffic now (always up while set)
} bsite_t;

static bsite_t bs[ROAM_SITES_MAX];
static struct {
	uint32_t away, away_max, run, tunes;
} st_away;

static bool bs_up(int i, uint32_t t)
{
	const bsite_t *b = &bs[i];
	if (b->dbm == 0) {
		return false;
	}
	if (b->period == 0 || b->busy) {
		return true;
	}
	int32_t d = (int32_t)(t - b->phase) + (int32_t)b->period;	// one period of margin for early beacons
	if (d < 0) {
		return false;
	}
	uint32_t k = (uint32_t)d / b->period, off = (uint32_t)d % b->period;
	int j = b->jitter != 0 ? (int)((k * 7919u + (uint32_t)i * 104729u) % (uint32_t)(2 * b->jitter + 1)) - b->jitter : 0;
	int32_t o = (int32_t)off - j;
	if (o < 0) {
		o += (int32_t)b->period;
	}
	return o < (int32_t)(b->len + ROAM_HOLD_MS);	// "heard" lasts the hold time past the last burst
}

static void bs_reset(void)
{
	memset(bs, 0, sizeof(bs));
	memset(&st_away, 0, sizeof(st_away));
}

// Run the engine ms long against bs[]; blocked as given
static uint32_t drive(roam_t *r, uint32_t t, uint32_t ms, uint8_t blocked)
{
	for (uint32_t e = 0; e < ms; e += 25) {
		t += 25;
		uint8_t at = roam_searching(r) ? r->visit : r->cur;
		bool up = bs_up(at, t);
		roam_sample(r, t, up, up ? bs[at].dbm : -125, up && bs[at].busy);
		roam_act_t a = roam_poll(r, t, blocked, false);
		if (a == ROAM_ACT_TUNE) {
			roam_tuned(r, t);
			st_away.tunes++;
		} else if (a == ROAM_ACT_PROBE) {
			roam_probe_sent(r, false);
		}
		if (roam_searching(r)) {
			st_away.away += 25;
			st_away.run += 25;
			if (st_away.run > st_away.away_max) {
				st_away.away_max = st_away.run;
			}
		} else {
			st_away.run = 0;
		}
	}
	return t;
}

static const roam_cfg_t cfg_sync = { ROAM_RSSI_DEFAULT, ROAM_HYST_DEFAULT, ROAM_PROBE_PASSIVE, 0, 60, 4, ROAM_SURVEY_SYNC };

// n sites beaconing together every 60 s for 4 s from phase, at the levels given (0: none)
static void bs_window(uint8_t n, uint32_t phase, const int16_t *dbm, int jitter)
{
	bs_reset();
	for (uint8_t i = 0; i < n; i++) {
		bs[i].dbm = dbm[i];
		bs[i].phase = phase;
		bs[i].period = 60000;
		bs[i].len = 4000;
		bs[i].jitter = jitter;
	}
}

static void test_survey_window(void)
{
	roam_t r;
	roam_init(&r, &cfg_sync);
	bs_window(4, 20000, (int16_t[]){ -90, -85, -100, 0 }, 0);
	sites_n(&r, 4, 0, 1000);
	// The schedule is learned from our own site's beacons: one is enough to try, the next ones confirm it
	uint32_t t = drive(&r, 1000, 18000, 0);
	CHECK(r.win_conf == 0 && !roam_window(&r, t, NULL) && r.surveys == 0 && st_away.away == 0);
	t = drive(&r, t, 12000, 0);
	uint32_t next = 0;
	CHECK(r.win_conf == 1 && r.beacons == 1 && roam_window(&r, t, &next) && next == 80000);
	CHECK(r.surveys == 0);
	// The next window: one round visits all three other sites (six fit in 4 s), back before the window ends
	t = drive(&r, t, 60000, 0);
	CHECK(r.surveys == 1 && r.win_conf >= 2);
	CHECK(iabs_t((int)phase_of(r.win_ms) - 20000) <= 100);
	CHECK(st_away.away_max <= 4000 - ROAM_WIN_LEAD_MS && st_away.away_max >= 2 * ROAM_SURVEY_SETTLE_MS);
	CHECK(r.state == ROAM_WATCH && r.cur == 0);
	CHECK(r.site[1].surv_dbm == -85 && r.site[2].surv_dbm == -100 && r.site[3].surv_ms == 0);	// 3: one miss so far
	CHECK(roam_site_fresh(&r, 1, t) && roam_site_fresh(&r, 2, t) && !roam_site_fresh(&r, 3, t));
	t = drive(&r, t, 3 * 60000, 0);
	CHECK(r.site[3].surv_ms != 0 && r.site[3].surv_dbm == 0 && r.site[3].miss >= 2);	// missed twice: not heard
	CHECK(r.searches == 0 && r.roams == 0 && r.cur == 0);
	// Time away: at most a window's worth per interval (the duty cycle bound holds)
	CHECK(st_away.away * 100u <= (t - 1000u) * ROAM_SURVEY_DUTY);
}

static void test_survey_jitter_and_rotation(void)
{
	roam_t r;
	// 12 sites, beacons up to 400 ms early or late: the schedule holds, every site gets measured in turn
	int16_t lv[12];
	for (int i = 0; i < 12; i++) {
		lv[i] = (int16_t)(-80 - i);
	}
	roam_init(&r, &cfg_sync);
	bs_window(12, 5000, lv, 400);
	sites_n(&r, 12, 0, 1000);
	uint32_t t = drive(&r, 1000, 10 * 60000, 0);
	CHECK(roam_window(&r, t, NULL) && r.win_conf >= 2);
	int measured = 0;
	for (int i = 1; i < 12; i++) {
		measured += r.site[i].surv_ms != 0 && iabs_t(r.site[i].surv_dbm - lv[i]) <= 1;
	}
	CHECK(measured == 11);
	for (int i = 1; i < 12; i++) {
		CHECK(roam_site_fresh(&r, (uint8_t)i, t));
	}
	CHECK(st_away.away_max <= 4000);
	CHECK(r.surveys >= 3);		// 11 sites over windows of about 9
}

static void test_survey_weak_confirm(void)
{
	roam_t r;
	roam_init(&r, &cfg_sync);
	bs_window(4, 20000, (int16_t[]){ -95, -85, -75, -100 }, 200);
	sites_n(&r, 4, 0, 1000);
	uint32_t t = drive(&r, 1000, 5 * 60000, 0);
	CHECK(r.site[2].surv_dbm == -75 && roam_site_fresh(&r, 2, t));
	// Our site fades: the weak trigger picks the best fresh site, confirms it in the next window and moves
	bs[0].dbm = -112;
	t = drive(&r, t, 3 * 60000, 0);
	CHECK(r.roams == 1 && r.cur == 2 && r.searches == 0 && r.confirms == 1);
	CHECK(r.why == ROAM_WHY_WEAK && r.home == 0);
	// Hysteresis with survey data: a site only 3 dB better is not worth a move
	roam_init(&r, &cfg_sync);
	bs_window(3, 20000, (int16_t[]){ -95, -104, -120 }, 0);
	sites_n(&r, 3, 0, 1000);
	t = drive(&r, 1000, 5 * 60000, 0);
	bs[0].dbm = -107;
	t = drive(&r, t, 3 * 60000, 0);
	CHECK(r.roams == 0 && r.searches == 0 && r.stays >= 1 && r.retry_until != 0);	// all fresh: stayed, no search
	// The confirm visit fails (the site went quiet since): stay, and that site's data says so
	roam_init(&r, &cfg_sync);
	bs_window(3, 20000, (int16_t[]){ -95, -80, -120 }, 0);
	sites_n(&r, 3, 0, 1000);
	t = drive(&r, 1000, 5 * 60000, 0);
	bs[1].dbm = 0;
	bs[0].dbm = -112;
	t = drive(&r, t, 90000, 0);
	CHECK(r.confirms >= 1 && r.roams == 0 && r.cur == 0);
}

static void test_survey_stale_fallback(void)
{
	roam_t r;
	// Beacons set but the schedule unknown (our site is always up, no beacon learned): a full search
	roam_init(&r, &cfg_sync);
	bs_reset();
	bs[0].dbm = -90;
	bs[1].dbm = -80;
	sites_n(&r, 2, 0, 1000);
	uint32_t t = drive(&r, 1000, ROAM_DWELL_MS, 0);
	CHECK(r.surveys == 0);
	bs[0].dbm = -115;
	t = drive(&r, t, ROAM_WEAK_MS + 8000, 0);
	CHECK(r.searches == 1 && r.cur == 1);
	// The schedule known, but the data too old: the stale sites are surveyed first (past the duty cycle), then the
	// move is decided on the fresh data without a full search
	roam_init(&r, &cfg_sync);
	bs_window(3, 20000, (int16_t[]){ -95, -80, -90 }, 0);
	sites_n(&r, 3, 0, 1000);
	t = drive(&r, 1000, 3 * 60000, 0);
	CHECK(roam_window(&r, t, NULL));
	r.site[1].surv_ms = r.site[2].surv_ms = 0;	// forgotten
	bs[0].dbm = -112;
	t = drive(&r, t, 3 * 60000, 0);
	CHECK(r.searches == 0 && r.roams == 1 && r.cur == 1);
	// Survey off: a weak site does the classic search
	roam_cfg_t c = cfg_sync;
	c.survey = ROAM_SURVEY_OFF;
	roam_init(&r, &c);
	bs_window(2, 20000, (int16_t[]){ -95, -80 }, 0);
	bs[1].period = 0;	// traffic: always up
	sites_n(&r, 2, 0, 1000);
	t = drive(&r, 1000, 3 * 60000, 0);
	CHECK(r.surveys == 0 && r.win_conf == 0);
	bs[0].dbm = -112;
	t = drive(&r, t, 2 * 60000, 0);
	CHECK(r.searches >= 1 && r.cur == 1);
}

static void test_survey_blocks_and_traffic(void)
{
	roam_t r;
	roam_init(&r, &cfg_sync);
	bs_window(3, 20000, (int16_t[]){ -90, -85, -88 }, 0);
	sites_n(&r, 3, 0, 1000);
	uint32_t t = drive(&r, 1000, 2 * 60000, 0);
	CHECK(roam_window(&r, t, NULL));
	uint32_t surveys = r.surveys;
	// Blocked (a call, the reply window, ...): no round
	t = drive(&r, t, 2 * 60000, RROAM_BLOCK_CALL);
	CHECK(r.surveys == surveys);
	// Traffic on our site: the survey holds off ROAM_SURVEY_HOLD_MS after it, doubled per busy period
	t = drive(&r, t, 60000, 0);
	CHECK(r.surveys == surveys + 1);
	surveys = r.surveys;
	uint32_t w = 0;
	roam_window(&r, t, &w);
	t = drive(&r, t, w - t - 3000, 0);	// 3 s before the window
	bs[0].busy = true;
	t = drive(&r, t, 2500, 0);
	bs[0].busy = false;
	t = drive(&r, t, 4000, 0);		// the window: still in the hold-off
	CHECK(r.surveys == surveys && r.busy_score >= 1);
	// A call heard on the visited site ends the visit at once (it is likely on ours too)
	t = drive(&r, t, 60000 - 4500, 0);
	roam_window(&r, t, &w);
	t = drive(&r, t, w - t + ROAM_WIN_LEAD_MS + 50, 0);
	CHECK(r.state == ROAM_SURVEY);
	uint32_t cuts = r.cuts;
	CHECK(roam_poll(&r, t, RROAM_BLOCK_CALL, true) == ROAM_ACT_DONE && r.state == ROAM_WATCH && r.cuts == cuts + 1);
	// The user's PTT during a visit: roam_abort, back at once
	t = drive(&r, t, 60000, 0);
	roam_window(&r, t, &w);
	t = drive(&r, t, w - t + ROAM_WIN_LEAD_MS + 50, 0);
	CHECK(r.state == ROAM_SURVEY && r.visit != r.cur);
	roam_abort(&r, t);
	CHECK(r.state == ROAM_WATCH && r.cuts == cuts + 2 && r.aborts == 0);
	// Locked: no survey
	roam_lock(&r, true, t);
	surveys = r.surveys;
	t = drive(&r, t, 3 * 60000, 0);
	CHECK(r.surveys == surveys);
}

static void test_survey_per_site(void)
{
	roam_t r;
	roam_cfg_t c = cfg_sync;
	c.survey = ROAM_SURVEY_SITE;
	c.beacon_s = 30;
	c.beacon_len_s = 3;
	roam_init(&r, &c);
	bs_reset();
	// Each site on its own schedule
	const uint32_t ph[3] = { 4000, 13000, 22000 };
	const int16_t lv[3] = { -90, -82, -97 };
	for (int i = 0; i < 3; i++) {
		bs[i].dbm = lv[i];
		bs[i].phase = ph[i];
		bs[i].period = 30000;
		bs[i].len = 3000;
		bs[i].jitter = 150;
	}
	sites_n(&r, 3, 0, 1000);
	uint32_t t = drive(&r, 1000, 20 * 30000, 0);
	// Each site's phase learned (a discovery visit listens through an interval once), then short aligned visits
	for (int i = 1; i < 3; i++) {
		CHECK(r.site[i].phase_ms != 0 && iabs_t((int)((r.site[i].phase_ms - ph[i] + 30000u * 40u) % 30000u)) <= 400);
		CHECK(r.site[i].surv_ms != 0 && iabs_t(r.site[i].surv_dbm - lv[i]) <= 1);
	}
	CHECK(st_away.away * 100u <= (t - 1000u) * ROAM_SURVEY_DUTY + 30000u * 100u);
	uint32_t tunes = st_away.tunes, away = st_away.away;
	t = drive(&r, t, 10 * 30000, 0);
	CHECK(st_away.tunes > tunes);
	// Aligned visits are short: the extra time away averages well under one beacon per visit
	CHECK((st_away.away - away) / (st_away.tunes - tunes) < 3000);
}

// Our own site silent (out of range): after a while one discovery visit listens to another site through an
// interval, learns the window from its beacon, and the rounds measure it from then on
static void test_survey_discovery(void)
{
	roam_t r;
	roam_init(&r, &cfg_sync);
	bs_window(3, 20000, (int16_t[]){ 0, -88, -95 }, 0);
	sites_n(&r, 3, 0, 1000);
	uint32_t t = drive(&r, 1000, 2 * 60000, 0);
	CHECK(r.surveys == 0 && st_away.away == 0);		// waits for its own site's beacons first
	t = drive(&r, t, 90000, 0);
	CHECK(r.surveys >= 1 && r.win_conf >= 1 && r.beacons >= 1);	// the discovery visit
	CHECK(st_away.away_max <= 60000 + 4000 + ROAM_WIN_TOL_MS + ROAM_SURVEY_SETTLE_MS + 25);
	t = drive(&r, t, 12 * 60000, 0);
	CHECK(roam_window(&r, t, NULL) && r.win_conf >= 2);
	CHECK(roam_site_fresh(&r, 1, t) && r.site[1].surv_dbm == -88);
	CHECK(roam_site_fresh(&r, 2, t) && r.site[2].surv_dbm == -95);
	CHECK(st_away.away * 100u <= (t - 1000u) * ROAM_SURVEY_DUTY + 65000u * 100u);
}

static void test_survey_defaults_off_without_beacons(void)
{
	roam_t r;
	roam_cfg_t c = cfg_sync;
	c.beacon_s = 0;		// no beacons: nothing to survey
	roam_init(&r, &c);
	bs_window(2, 20000, (int16_t[]){ -90, -80 }, 0);
	sites_n(&r, 2, 0, 1000);
	uint32_t t = drive(&r, 1000, 5 * 60000, 0);
	CHECK(r.surveys == 0 && st_away.away == 0 && r.beacons == 0);
	(void)t;
}

// ======================================================================================================================
// Through the radio core
// ======================================================================================================================

// ---- fake board: repeaters on frequencies ----
#define MAX_RPT 6
typedef struct {
	uint32_t out_hz;	// its output (the radio's RX)
	uint32_t in_hz;		// its input (the radio's TX)
	uint8_t cc;
	int16_t rssi_dbm;
	bool beacon;		// always up (else up only after a wake)
	bool wakeable;
	uint32_t bcn_period;	// idle beacons: up bcn_len ms every bcn_period ms from bcn_phase (0 = none)
	uint32_t bcn_len, bcn_phase;
	uint32_t up_until;	// awake until
	int wakes;
} rpt_t;

static struct {
	uint32_t rx_hz;
	uint32_t tx_hz;
	bool keyed;
	int tx_calls;
	rpt_t rpt[MAX_RPT];
	unsigned air_seen;
	int ts_toggle;
} hw;

static uint32_t now;

static rpt_t *rpt_on_rx(void)
{
	for (int i = 0; i < MAX_RPT; i++) {
		if (hw.rpt[i].out_hz != 0 && hw.rpt[i].out_hz == hw.rx_hz) {
			return &hw.rpt[i];
		}
	}
	return NULL;
}

static bool rpt_up(const rpt_t *p)
{
	if (p == NULL) {
		return false;
	}
	if (p->bcn_period != 0 && (int32_t)(now - p->bcn_phase) >= 0 && (now - p->bcn_phase) % p->bcn_period < p->bcn_len) {
		return true;
	}
	return p->beacon || (int32_t)(p->up_until - now) > 0;
}

// Repeater i beacons every 60 s for 4 s from phase (the network's shared window)
static void beacon(int i, int16_t dbm, uint32_t phase)
{
	hw.rpt[i].rssi_dbm = dbm;
	hw.rpt[i].beacon = false;
	hw.rpt[i].bcn_period = 60000;
	hw.rpt[i].bcn_len = 4000;
	hw.rpt[i].bcn_phase = phase;
}

static void r_init(void) {}
static void r_set_mode(radio_mode_t m) { sim.refuse_start = m != RADIO_MODE_DMR; }
static bool r_rx(uint32_t hz, radio_bw_t bw, const subtone_t *t)
{
	(void)bw;
	(void)t;
	hw.rx_hz = hz;
	hw.keyed = false;
	return true;
}
static bool r_tx(uint32_t hz, radio_bw_t bw, uint8_t p, const subtone_t *t)
{
	(void)bw;
	(void)p;
	(void)t;
	hw.tx_calls++;
	hw.tx_hz = hz;
	hw.keyed = true;
	return true;
}
static void r_tx_off(void) { hw.keyed = false; }
static void r_status(radio_status_t *st)
{
	const rpt_t *p = rpt_on_rx();
	st->noise = 120;
	st->rssi_dbm = -125;
	st->carrier = false;
	st->tone_ok = true;
	if (rpt_up(p)) {
		st->noise = 10;
		st->rssi_dbm = p->rssi_dbm;
		st->carrier = true;
	}
}
static void r_audio(bool on) { (void)on; }
static const hal_radio_t fake_radio = {
	.init = r_init, .set_mode = r_set_mode, .rx = r_rx, .tx = r_tx, .tx_off = r_tx_off, .status = r_status,
	.audio = r_audio,
};

static void p_led(uint8_t which, bool on) { (void)which; (void)on; }
static uint16_t p_battery_mv(void) { return 8000; }
static uint8_t p_battery_pct(void) { return 80; }
static const hal_power_t fake_power = { .led = p_led, .battery_mv = p_battery_mv, .battery_pct = p_battery_pct };

static struct {
	bool streaming;
	audio_block_cb_t cb;
} aud;
static bool a_start(audio_block_cb_t cb, void *ctx) { (void)ctx; aud.cb = cb; aud.streaming = true; return true; }
static void a_stop(void) { aud.streaming = false; aud.cb = NULL; }
static void a_pcm(bool on) { (void)on; }
static const hal_audio_t fake_audio = { a_start, a_stop, a_pcm };

static bool v_present(void) { return true; }
static void v_reset(void) {}
static void v_encode(const int16_t *pcm, uint8_t *bits) { (void)pcm; memset(bits, 0, VOCODER_BITS); }
static void v_decode(const uint8_t *bits, int16_t *pcm) { (void)bits; memset(pcm, 0, VOCODER_FRAME_SAMPLES * 2); }
static const hal_vocoder_t fake_vocoder = { v_present, v_reset, v_encode, v_decode, NULL, NULL, NULL, NULL };

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
};
static const char *const power_names[] = { "L", "M", "H" };

const board_t board = {
	.name = "fake",
	.bands = bands,
	.band_count = 2,
	.power_count = 3,
	.power_names = power_names,
	.power_default = 1,
	.radio = &fake_radio,
	.power = &fake_power,
	.dmr = &sim_modem,
	.audio = &fake_audio,
	.vocoder = &fake_vocoder,
	.battery_low_mv = 6400,
	.battery_critical_mv = 6000,
};

// ---- fake codeplug: one zone ----
#define NCH 8
static channel_t chans[NCH + 1];
static bool used[NCH + 1];
static zone_t zone;

codeplug_source_t codeplug_source(void) { return CODEPLUG_OPENFW; }
uint32_t codeplug_serial(void) { return 1; }
void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}
uint16_t codeplug_channel_count(void) { return NCH; }
bool codeplug_channel(uint16_t n, channel_t *out)
{
	if (n < 1 || n > NCH || !used[n]) {
		return false;
	}
	*out = chans[n];
	return true;
}
uint16_t codeplug_zone_count(void) { return 1; }
bool codeplug_zones_stored(void) { return true; }
bool codeplug_zone(uint16_t i, zone_t *out)
{
	if (i != 0) {
		return false;
	}
	*out = zone;
	return true;
}
static const contact_t contacts[] = { { "Local", 9, CONTACT_GROUP, 0 }, { "World", 91, CONTACT_GROUP, 0 } };
uint32_t codeplug_dmr_id(void) { return 3101276; }
bool codeplug_contact(uint16_t index, contact_t *out)
{
	if (index < 1 || index > 2) {
		return false;
	}
	*out = contacts[index - 1];
	return true;
}
bool codeplug_listen_list(uint16_t index, listen_list_t *out) { (void)index; (void)out; return false; }
bool codeplug_contact_find(uint32_t id, bool group, contact_t *out)
{
	for (int i = 0; i < 2; i++) {
		if (group && id == contacts[i].id) {
			*out = contacts[i];
			return true;
		}
	}
	return false;
}
bool codeplug_dmr_settings(codeplug_dmr_settings_t *out)
{
	*out = (codeplug_dmr_settings_t){ "N0CALL", 0, 0 };
	return true;
}
bool dmr_iddb_lookup(uint32_t id, char *out, size_t len) { (void)id; (void)out; (void)len; return false; }

static void add(uint16_t n, const char *name, uint32_t rx, uint32_t tx, uint8_t mode, uint16_t contact)
{
	memset(&chans[n], 0, sizeof(chans[n]));
	strcpy(chans[n].name, name);
	chans[n].rx_hz = rx;
	chans[n].tx_hz = tx;
	chans[n].mode = mode;
	chans[n].power = CODEPLUG_POWER_DEFAULT;
	chans[n].color_code = 1;
	chans[n].timeslot = 1;
	chans[n].contact = contact;
	used[n] = true;
}

// Zone positions = channel numbers - 1:
//   1 Site A 438.500 / 430.900, 2 Site B 439.000 / 431.400, 3 Site C 439.500 / 431.900, 4 Site D 440.000 / 432.400
//   (all CC 1, TS 1, TG 9: the site list); 5 Other TG 440.500 / 432.900 (TG 91), 6 Simplex 438.000 (DMR, rx = tx),
//   7 FM 146.520 (not sites)
#define A 0
#define B 1
#define C 2
#define D 3
#define E 4
static void setup_codeplug(void)
{
	memset(used, 0, sizeof(used));
	add(1, "Site A", 438500000, 430900000, RADIO_MODE_DMR, 1);
	add(2, "Site B", 439000000, 431400000, RADIO_MODE_DMR, 1);
	add(3, "Site C", 439500000, 431900000, RADIO_MODE_DMR, 1);
	add(4, "Site D", 440000000, 432400000, RADIO_MODE_DMR, 1);
	add(5, "Other TG", 440500000, 432900000, RADIO_MODE_DMR, 2);
	add(6, "Simplex", 438000000, 438000000, RADIO_MODE_DMR, 1);
	add(7, "FM", 146520000, 146520000, RADIO_MODE_FM, 0);
	memset(&zone, 0, sizeof(zone));
	strcpy(zone.name, "Roam");
	zone.count = 7;
	for (int i = 0; i < 7; i++) {
		zone.channels[i] = (uint16_t)(i + 1);
	}
}

// Repeaters behind channels 1-5 (index = zone position); site D's repeater sends color code 3 (the channel says 1)
static void setup_repeaters(void)
{
	memset(hw.rpt, 0, sizeof(hw.rpt));
	for (int i = 0; i < 5; i++) {
		hw.rpt[i].out_hz = chans[i + 1].rx_hz;
		hw.rpt[i].in_hz = chans[i + 1].tx_hz;
		hw.rpt[i].cc = 1;
		hw.rpt[i].rssi_dbm = -125;
		hw.rpt[i].beacon = false;
	}
	hw.rpt[D].cc = 3;
}

static void site(int i, int16_t dbm, bool beacon)
{
	hw.rpt[i].rssi_dbm = dbm;
	hw.rpt[i].beacon = beacon;
}

static radio_sm_t sm;

static void req2(uint8_t type, int32_t arg, uint32_t arg2)
{
	radio_req_t r = { type, arg, arg2 };
	radio_sm_request(&sm, &r, now);
}
static void req(uint8_t type, int32_t arg) { req2(type, arg, 0); }

// The repeater the radio listens to: an idle burst on the other timeslot each slot while it is up (BS sync, its
// color code); a BS_Dwn_Act on a sleeping repeater's input wakes it for 10 s
static void air_step(void)
{
	while (hw.air_seen < sim.air_count) {
		const sim_air_t *a = &sim.air[hw.air_seen++];
		if (a->kind != DMR_BURST_CSBK || (a->payload[0] & 0x3F) != 0x38 || !hw.keyed) {
			continue;
		}
		for (int i = 0; i < MAX_RPT; i++) {
			if (hw.rpt[i].in_hz != 0 && hw.rpt[i].in_hz == hw.tx_hz && hw.rpt[i].wakeable) {
				hw.rpt[i].up_until = now + 10000;
				hw.rpt[i].wakes++;
			}
		}
	}
	const rpt_t *p = rpt_on_rx();
	if (!rpt_up(p) || hw.keyed) {
		return;
	}
	dmr_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_IDLE;
	b.sync = DMR_SYNC_BS;
	b.cc = p->cc;
	b.ts = 2;
	b.crc_ok = true;
	b.in_step = true;
	b.rssi_dbm = p->rssi_dbm;
	b.len = 12;
	sim_rx(&b);
}

static void run(uint32_t ms)
{
	static int16_t spk[AUDIO_BLOCK_SAMPLES], mic[AUDIO_BLOCK_SAMPLES];
	for (uint32_t t = 0; t < ms; t += 5) {
		now += 5;
		if (now % 30 == 0) {
			sim_slot();
			air_step();
		}
		if (now % RADIO_TICK_MS == 0) {
			radio_sm_tick(&sm, now);
		}
		if (now % RADIO_POLL_MS == 0) {
			radio_sm_poll(&sm, now);
		}
		if (now % 20 == 0 && aud.streaming && aud.cb != NULL) {
			aud.cb(mic, spk, NULL);
		}
	}
}

#define RV (&sm.view.roam)
static uint16_t on_ch(void) { return sm.view.vfo ? 0 : sm.view.channel_number; }

static void start(bool zone_on)
{
	ambeVoiceStop();
	memset(&aud, 0, sizeof(aud));
	sim_reset();
	memset(&hw, 0, sizeof(hw));
	setup_codeplug();
	setup_repeaters();
	now = 1000;
	settings_init();
	radio_sm_init(&sm, now);
	run(100);
	req(RREQ_ZONE_POS, A);
	run(100);
	if (zone_on) {
		req(RREQ_ROAM, RROAM_ZONE_ON);
		run(50);
	}
}

static void set_probe(radio_settings_t *s) { s->roam_probe = 1; }
static void edit(void (*f)(radio_settings_t *))
{
	radio_settings_t s = sm.set;
	f(&s);
	radio_sm_set_settings(&sm, &s, now);
}

// Run until the radio is watching again after a search (or ms ran out)
static void until_watch(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 25) {
		run(25);
		if (RV->state != ROAM_SEARCH && sm.roam.state != ROAM_SEARCH) {
			return;
		}
	}
}

static void until_search(uint32_t ms)
{
	for (uint32_t t = 0; t < ms && sm.roam.state != ROAM_SEARCH; t += 25) {
		run(25);
	}
}

static void test_sites_and_zone_flag(void)
{
	start(false);
	CHECK(on_ch() == 1 && sm.view.ch.mode == RADIO_MODE_DMR);
	CHECK(!RV->zone_on && RV->state == ROAM_OFF && RV->sites == 0);	// off until the zone is a roaming zone
	settings_t s;
	settings_read(&s);
	CHECK((s.roam_zones[0] & 1) == 0);

	req(RREQ_ROAM, RROAM_ZONE_TOGGLE);
	run(50);
	CHECK(RV->zone_on && RV->state == ROAM_WATCH);
	// Sites: the four repeater channels with TG 9 (not the TG 91 one, the simplex one or the FM one)
	CHECK(RV->sites == 4 && RV->site == 0 && RV->home == 0);
	CHECK(RV->site_number[0] == 1 && RV->site_number[1] == 2 && RV->site_number[2] == 3 && RV->site_number[3] == 4);
	settings_read(&s);
	CHECK((s.roam_zones[0] & 1) == 1);	// kept across power cycles

	// A channel that is not a site: roaming off there, the zone flag stays
	req(RREQ_ZONE_POS, 4);	// Other TG
	run(100);
	CHECK(RV->zone_on && RV->state == ROAM_OFF);
	req(RREQ_ZONE_POS, 5);	// Simplex
	run(100);
	CHECK(RV->state == ROAM_OFF);
	// Back on a site the user picks: that one is home
	req(RREQ_ZONE_POS, B);
	run(100);
	CHECK(RV->state == ROAM_WATCH && RV->site == 1 && RV->home == 1);
	// The VFO: off
	req(RREQ_VFO, 1);
	run(100);
	CHECK(RV->state == ROAM_OFF && !RV->zone_on);
	req(RREQ_VFO, 0);
	run(100);
	CHECK(RV->state == ROAM_WATCH);

	// The open-firmware CPS Roaming flag narrows the zone to the flagged channels
	chans[1].roam = chans[3].roam = true;
	req(RREQ_ROAM, RROAM_ZONE_OFF);
	run(50);
	CHECK(RV->state == ROAM_OFF && !RV->zone_on);
	req(RREQ_ZONE_POS, A);
	req(RREQ_ROAM, RROAM_ZONE_ON);
	run(50);
	CHECK(RV->sites == 2 && RV->site_number[0] == 1 && RV->site_number[1] == 3);

	// A new power up reads the zone flag back
	radio_sm_init(&sm, now);
	run(200);
	CHECK(RV->zone_on);
}

// Site A fades; B and C are heard (C stronger), D louder still but with another color code
static void test_weak_roams_to_strongest(void)
{
	start(true);
	site(A, -85, true);
	site(B, -95, true);
	site(C, -80, true);
	site(D, -60, true);
	run(ROAM_DWELL_MS);
	CHECK(RV->state == ROAM_WATCH && RV->rssi_dbm == -85 && !RV->weak && RV->searches == 0);
	site(A, -112, true);
	run(2000);
	CHECK(RV->weak && RV->searches == 0);	// weak, but not for ROAM_WEAK_MS yet
	until_search(ROAM_WEAK_MS + 2000);
	CHECK(sm.roam.state == ROAM_SEARCH && RV->why == ROAM_WHY_WEAK);
	run(ROAM_SETTLE_MS + 100);
	CHECK(on_ch() == 2);			// visiting B
	until_watch(15000);
	CHECK(RV->state == ROAM_WATCH && on_ch() == 3 && RV->site == 2 && RV->roams == 1);
	CHECK(RV->home == 0);			// home is still the user's site
	CHECK(RV->site_rssi[3] == 0);		// D's repeater never counted (color code 3)
	CHECK(hw.rx_hz == 439500000 && sm.view.dmr.state == RDMR_IDLE);
	settings_t s;
	settings_read(&s);
	CHECK(s.channel == 3);			// powers up on the new site
	CHECK(hw.tx_calls == 0);		// passive: nothing sent

	// C fades, home is strong again: back home (B as strong, but home is within the hysteresis of it)
	site(A, -76, true);
	site(B, -74, true);
	run(ROAM_DWELL_MS);
	site(C, -113, true);
	until_search(ROAM_WEAK_MS + 3000);
	CHECK(sm.roam.state == ROAM_SEARCH);
	until_watch(15000);
	CHECK(on_ch() == 1 && RV->site == 0 && RV->roams == 2);
}

static void test_hysteresis_stays(void)
{
	start(true);
	site(A, -108, true);	// weak (under -105)
	site(B, -104, true);	// over the threshold, but only 4 dB better
	site(C, -120, true);	// under the threshold
	run(ROAM_DWELL_MS + ROAM_WEAK_MS + 1000);
	CHECK(RV->searches == 1);
	until_watch(15000);
	CHECK(on_ch() == 1 && RV->stays == 1 && RV->roams == 0);
	// Back off: no new search for ROAM_RETRY_MS
	run(ROAM_RETRY_MS - 2000);
	CHECK(RV->searches == 1);
}

// Nothing starts a search during a call, the reply window, a transmission or a temporary override
static void test_block_list(void)
{
	start(true);
	site(A, -85, true);
	site(B, -70, true);
	run(ROAM_DWELL_MS);
	site(A, -115, true);

	// A call: blocked, and for ROAM_QUIET_MS after
	uint8_t lc[9];
	dmr_lc_t l = { .flco = DMR_FLCO_GROUP_VOICE, .dst = 9, .src = 3101234 };
	dmr_lc_pack(&l, lc);
	for (int k = 0; k < 140; k++) {	// 8.4 s of voice header + voice
		dmr_burst_t b;
		memset(&b, 0, sizeof(b));
		b.sync = DMR_SYNC_BS;
		b.cc = 1;
		b.ts = 1;
		b.crc_ok = true;
		b.in_step = true;
		b.rssi_dbm = -115;
		if (k < 3) {
			b.kind = DMR_BURST_VOICE_LC_HEADER;
			b.len = 12;
			dmr_lc_encode_full(lc, DMR_RS_MASK_VOICE_HEADER, b.payload);
		} else {
			b.kind = DMR_BURST_VOICE;
			b.voice_index = (uint8_t)((k - 3) % 6);
			b.len = 27;
			if (b.voice_index == 5) {
				memcpy(b.lc, lc, 9);
				b.lc_valid = true;
			}
		}
		sim_rx(&b);
		run(60);
		if (k == 100) {
			CHECK(sm.view.dmr.state == RDMR_RX && RV->pending == ROAM_WHY_WEAK);
			CHECK(RV->blocked == RROAM_BLOCK_CALL && RV->searches == 0 && on_ch() == 1);
		}
	}
	{
		dmr_burst_t b;
		memset(&b, 0, sizeof(b));
		b.kind = DMR_BURST_TERMINATOR_LC;
		b.sync = DMR_SYNC_BS;
		b.cc = 1;
		b.ts = 1;
		b.crc_ok = true;
		b.in_step = true;
		b.len = 12;
		dmr_lc_encode_full(lc, DMR_RS_MASK_TERMINATOR, b.payload);
		sim_rx(&b);
	}
	run(500);
	CHECK(sm.view.dmr.state == RDMR_IDLE && RV->searches == 0);
	CHECK(RV->blocked == RROAM_BLOCK_QUIET || RV->blocked == RROAM_BLOCK_REPLY);
	// A temporary talk target: an override
	req(RREQ_DMR_TARGET, 3101234 | (1 << 24));
	run(ROAM_QUIET_MS + 3000);
	CHECK(RV->blocked == RROAM_BLOCK_OVERRIDE && RV->searches == 0 && on_ch() == 1);
	req(RREQ_DMR_TARGET_CLEAR, 0);
	// Hear all latched: an override
	req(RREQ_DMR_HEAR_ALL, 1);
	run(500);
	CHECK(RV->blocked == RROAM_BLOCK_OVERRIDE && RV->searches == 0);
	req(RREQ_DMR_HEAR_ALL, 0);
	// The monitor key (DMR: its press latches digital monitor, a second press ends it, #19)
	req(RREQ_MONITOR, 1);
	req(RREQ_MONITOR, 0);
	run(500);
	CHECK(RV->searches == 0 && sm.view.dmr.hear_all_latch);
	req(RREQ_MONITOR, 1);
	req(RREQ_MONITOR, 0);
	CHECK(!sm.view.dmr.hear_all_latch);
	// Transmitting (B up, A up: the PTT goes through)
	req(RREQ_PTT, 1);
	run(2000);
	CHECK(RV->blocked == RROAM_BLOCK_TX && RV->searches == 0 && on_ch() == 1);
	req(RREQ_PTT, 0);
	run(1000);
	CHECK(RV->searches == 0);
	// Free: the latched weak trigger runs
	until_search(ROAM_QUIET_MS + 3000);
	CHECK(RV->searches == 1);
	until_watch(15000);
	CHECK(on_ch() == 2);
}

// The user does something during a search: back to where it started at once
static void test_ptt_aborts_search(void)
{
	start(true);
	site(A, -90, true);
	site(B, -70, true);
	run(4000);
	req(RREQ_ROAM, RROAM_NOW);	// no dwell needed
	run(ROAM_SETTLE_MS + 200);
	CHECK(sm.roam.state == ROAM_SEARCH && on_ch() == 2 && RV->why == ROAM_WHY_MANUAL);
	req(RREQ_PTT, 1);
	CHECK(on_ch() == 1 && sm.roam.state == ROAM_WATCH && sm.roam.aborts == 1);
	run(500);
	CHECK(hw.keyed && hw.tx_hz == 430900000);	// transmitting on the origin's input
	req(RREQ_PTT, 0);
	run(ROAM_QUIET_MS + 2000);
	CHECK(RV->searches == 1 && on_ch() == 1);	// roam now is not kept

	// A channel change during a search: the user's channel wins
	req(RREQ_ROAM, RROAM_NOW);
	run(ROAM_SETTLE_MS + 200);
	CHECK(sm.roam.state == ROAM_SEARCH);
	req(RREQ_ZONE_POS, C);
	run(200);
	CHECK(on_ch() == 3 && sm.roam.state == ROAM_WATCH && RV->home == 2);
}

// The repeater doesn't answer our transmission: a search, the origin included
static void test_tx_fail_trigger(void)
{
	start(true);
	site(A, -90, false);	// sleeping, and it never wakes
	site(B, -80, true);
	run(4000);
	req(RREQ_PTT, 1);
	run(4000);
	req(RREQ_PTT, 0);
	run(500);
	CHECK(sm.roam.pending == ROAM_WHY_TX_FAIL || RV->searches == 1);
	until_search(ROAM_QUIET_MS + 3000);
	CHECK(RV->searches == 1 && RV->why == ROAM_WHY_TX_FAIL);
	until_watch(15000);
	CHECK(on_ch() == 2 && RV->roams == 1);
}

// Wake probing: no repeater sends unless woken; the probe finds B
static void test_wake_probe(void)
{
	start(true);
	edit(set_probe);
	site(A, -110, false);
	site(B, -80, false);
	hw.rpt[B].wakeable = true;
	site(C, -90, false);	// never wakes
	run(1000);
	req(RREQ_ROAM, RROAM_NOW);
	until_search(ROAM_QUIET_MS + 500);	// roam now waits out the quiet time after the last activity
	CHECK(RV->searches == 1);
	until_watch(30000);
	CHECK(RV->searches == 1 && RV->probes >= 1);
	CHECK(hw.rpt[B].wakes >= 1 && hw.rpt[C].wakes == 0);
	CHECK(on_ch() == 2 && RV->roams == 1);
	CHECK(!hw.keyed);
}

static void test_lock_and_roam_now(void)
{
	start(true);
	site(A, -90, true);
	site(B, -70, true);
	run(2000);
	req(RREQ_ROAM, RROAM_LOCK);
	run(50);
	CHECK(RV->state == ROAM_LOCKED);
	site(A, -120, true);
	run(ROAM_DWELL_MS + ROAM_WEAK_MS + 2000);
	CHECK(RV->searches == 0 && on_ch() == 1);	// locked: stays
	req(RREQ_ROAM, RROAM_LOCK_TOGGLE);
	run(50);
	CHECK(RV->state == ROAM_WATCH);
	req(RREQ_ROAM, RROAM_LOCK);
	run(50);
	req(RREQ_ROAM, RROAM_NOW);	// roam now ends the lock and searches
	until_search(500);
	CHECK(RV->searches == 1);
	until_watch(15000);
	CHECK(on_ch() == 2 && RV->state == ROAM_WATCH);
	// A lock ends with the user's channel change
	req(RREQ_ROAM, RROAM_LOCK);
	run(50);
	req(RREQ_ZONE_POS, A);
	run(100);
	CHECK(RV->state == ROAM_WATCH);
}

// Scanning: no search; hotspot-free builds here
static void test_scan_blocks(void)
{
#if CONFIG_SCAN
	start(true);
	site(A, -85, true);
	run(ROAM_DWELL_MS);
	site(A, -115, true);
	req2(RREQ_SCAN, RSCAN_ZONE, 0);
	run(ROAM_WEAK_MS + 4000);
	CHECK(RV->searches == 0);
	req2(RREQ_SCAN, RSCAN_STOP, 0);
	run(200);
#endif
}

// The defaults with roaming on: the beacon survey of a synchronized network (60 s / 4 s), listening only
static void test_rc_defaults_and_store(void)
{
	start(true);
	CHECK(sm.set.roam_beacon_s == 60 && sm.set.roam_beacon_len_s == 4 && sm.set.roam_survey == 1);
	CHECK(sm.set.roam_probe == 0 && !sm.set.rpt_beacon);
	settings_t s;
	settings_defaults(&s);
	CHECK(s.roam_beacon_s == 60 && s.roam_beacon_len_s == 4 && s.roam_survey == 1 && s.rpt_beacon == 0);
	// A record from before these fields (roam_stored with the old fields; the new ones keep settings_defaults'):
	// the new defaults apply
	settings_t *w = settings_begin();
	w->roam_stored = 1;
	w->roam_rssi_dbm = -100;
	settings_end(now);
	radio_sm_init(&sm, now);
	run(100);
	CHECK(sm.set.roam_rssi_dbm == -100 && sm.set.roam_beacon_s == 60 && sm.set.roam_survey == 1);
	// Stored and clamped
	radio_settings_t rs = sm.set;
	rs.roam_beacon_s = 5;
	rs.roam_beacon_len_s = 30;
	rs.roam_survey = 2;
	rs.rpt_beacon = true;
	radio_sm_set_settings(&sm, &rs, now);
	CHECK(sm.set.roam_beacon_s == 10 && sm.set.roam_beacon_len_s == 10 && sm.set.roam_survey == 2 && sm.set.rpt_beacon);
	settings_read(&s);
	CHECK(s.roam_beacon_s == 10 && s.roam_beacon_len_s == 10 && s.roam_survey == 2 && s.rpt_beacon == 1);
	// The SFR user receive mode shares rpt_beacon's byte (bits 1-2); an out of range value is the repeat only
	CHECK(sm.set.sfr_rx == 0);
	rs = sm.set;
	rs.sfr_rx = 2;
	radio_sm_set_settings(&sm, &rs, now);
	settings_read(&s);
	CHECK(sm.set.sfr_rx == (CONFIG_SFR_VOTING ? 2 : 1) && sm.set.rpt_beacon);
	CHECK(s.rpt_beacon == (CONFIG_SFR_VOTING ? 0x05 : 0x03));
	radio_sm_init(&sm, now);
	run(100);
	CHECK(sm.set.sfr_rx == (CONFIG_SFR_VOTING ? 2 : 1) && sm.set.rpt_beacon);
	rs = sm.set;
	rs.sfr_rx = 7;
	rs.rpt_beacon = false;
	radio_sm_set_settings(&sm, &rs, now);
	settings_read(&s);
	CHECK(sm.set.sfr_rx == 0 && s.rpt_beacon == 0);
	// The SFR repeater's CW ID interval: bits 3-5, off by default, 0..4; off without CONFIG_SFR_CWID
	CHECK(sm.set.sfr_cwid == 0);
	rs = sm.set;
	rs.sfr_cwid = 3;
	radio_sm_set_settings(&sm, &rs, now);
	settings_read(&s);
	CHECK(sm.set.sfr_cwid == (CONFIG_SFR_CWID ? 3 : 0) && s.rpt_beacon == (CONFIG_SFR_CWID ? 0x18 : 0));
	radio_sm_init(&sm, now);
	run(100);
	CHECK(sm.set.sfr_cwid == (CONFIG_SFR_CWID ? 3 : 0) && sm.set.sfr_rx == 0 && !sm.set.rpt_beacon);
	rs = sm.set;
	rs.sfr_cwid = 5;
	radio_sm_set_settings(&sm, &rs, now);
	settings_read(&s);
	CHECK(sm.set.sfr_cwid == 0 && s.rpt_beacon == 0);
}

// Run until the radio is on a survey visit (or ms ran out)
static void until_survey(uint32_t ms)
{
	for (uint32_t t = 0; t < ms && sm.roam.state != ROAM_SURVEY; t += 5) {
		run(5);
	}
}

// Idle repeaters that only beacon: the survey learns the window from site A and measures the others in it
static void test_rc_survey(void)
{
	start(true);
	beacon(A, -85, 20000);
	beacon(B, -80, 20000);
	beacon(C, -95, 20000);
	beacon(D, -70, 20000);	// color code 3: never counted
	run(60000 - 1000);		// the beacon at 20 s: the window tried, the next one (80 s) not yet here
	CHECK(RV->win_conf == 1 && RV->win_next_ms != 0 && RV->beacons == 1);
	CHECK(RV->surveys == 0 && on_ch() == 1);
	// The next window: a round; the screens keep showing site A while the radio is away
	until_survey(60000);
	CHECK(sm.roam.state == ROAM_SURVEY && RV->state == RROAM_STATE_SURVEY && RV->visit != 0xFF);
	radio_view_t v = sm.view;
	radio_sm_shown_view(&sm, &v);
	CHECK(v.channel_number == 1 && strcmp(v.ch.name, "Site A") == 0);
	run(5000);
	CHECK(sm.roam.state == ROAM_WATCH && on_ch() == 1 && RV->surveys == 1);
	CHECK(RV->site_surv[1] == -80 && RV->site_surv[2] == -95 && RV->site_surv[3] == 0);
	CHECK((RV->site_fresh16 & 0x6) == 0x6);
	run(3 * 60000);
	CHECK(RV->site_age_s[3] != 0xFFFF && RV->site_surv[3] == 0);	// D: missed twice, "not heard"
	CHECK(hw.tx_calls == 0 && RV->searches == 0 && RV->roams == 0);	// listening only
	radio_view_t v2 = sm.view;
	radio_sm_shown_view(&sm, &v2);
	CHECK(v2.channel_number == 1);	// watching: the view is the radio's own

	// Our site fades: the move uses the survey data (a confirm visit in the next window), no blind search
	beacon(A, -112, 20000);
	for (int k = 0; k < 4 * 60 && RV->roams == 0; k++) {
		run(1000);
	}
	CHECK(RV->roams == 1 && on_ch() == 2 && RV->searches == 0 && RV->confirms >= 1);
	settings_t s;
	settings_read(&s);
	CHECK(s.channel == 2);
}

// PTT during a survey visit: back on the site at once and the transmission starts there; the extra delay is the
// retune (and the repeater's outbound seen again), not a visit's length
static uint32_t ptt_latency(void)
{
	uint32_t t0 = now;
	req(RREQ_PTT, 1);
	while (now - t0 < 3000 && !(hw.keyed && hw.tx_hz == 430900000 && dmr_call_view()->phase == DMR_CALL_PHASE_TX_VOICE)) {
		run(5);
	}
	uint32_t lat = now - t0;
	req(RREQ_PTT, 0);
	run(ROAM_QUIET_MS + 2000);
	return lat;
}

static void test_rc_survey_ptt_and_traffic(void)
{
	start(true);
	beacon(A, -85, 20000);
	beacon(B, -80, 20000);
	beacon(C, -95, 20000);
	run(2 * 60000 + 30000);
	CHECK(RV->win_conf >= 2);
	// Baseline: PTT on site A during its beacon, no survey
	radio_settings_t rs = sm.set;
	rs.roam_survey = 0;
	radio_sm_set_settings(&sm, &rs, now);
	while ((now - 20000u) % 60000u != ROAM_WIN_LEAD_MS + 200u) {
		run(5);
	}
	uint32_t base = ptt_latency();
	rs.roam_survey = 1;
	radio_sm_set_settings(&sm, &rs, now);
	run(3 * 60000);			// the schedule again (a mode change forgets it)
	CHECK(RV->win_conf >= 2);
	until_survey(70000);
	run(200);
	CHECK(sm.roam.state == ROAM_SURVEY && on_ch() != 1);
	uint32_t cuts = RV->cuts;
	uint32_t lat = ptt_latency();
	CHECK(RV->cuts == cuts + 1 && RV->aborts == 0);
	CHECK(lat <= base + 100);
	printf("# roamTest: PTT latency on a survey visit %u ms (on the site, no survey: %u ms)\n", (unsigned)lat,
	       (unsigned)base);

	// A call heard on the visited site: back on our own site at once (the call is likely carried there too)
	until_survey(70000);
	run(200);
	CHECK(sm.roam.state == ROAM_SURVEY);
	cuts = RV->cuts;
	uint8_t lc[9];
	dmr_lc_t l = { .flco = DMR_FLCO_GROUP_VOICE, .dst = 9, .src = 3101234 };
	dmr_lc_pack(&l, lc);
	for (int k = 0; k < 3 && sm.roam.state == ROAM_SURVEY; k++) {
		dmr_burst_t b;
		memset(&b, 0, sizeof(b));
		b.sync = DMR_SYNC_BS;
		b.cc = 1;
		b.ts = 1;
		b.crc_ok = true;
		b.in_step = true;
		b.rssi_dbm = -80;
		b.kind = DMR_BURST_VOICE_LC_HEADER;
		b.len = 12;
		dmr_lc_encode_full(lc, DMR_RS_MASK_VOICE_HEADER, b.payload);
		sim_rx(&b);
		run(60);
	}
	CHECK(sm.roam.state == ROAM_WATCH && RV->cuts == cuts + 1 && on_ch() == 1);
}

int main(void)
{
	test_engine_basics();
	test_engine_choice();
	test_engine_lock_abort();
	test_survey_window();
	test_survey_jitter_and_rotation();
	test_survey_weak_confirm();
	test_survey_stale_fallback();
	test_survey_blocks_and_traffic();
	test_survey_per_site();
	test_survey_discovery();
	test_survey_defaults_off_without_beacons();
	test_sites_and_zone_flag();
	test_weak_roams_to_strongest();
	test_hysteresis_stays();
	test_block_list();
	test_ptt_aborts_search();
	test_tx_fail_trigger();
	test_wake_probe();
	test_lock_and_roam_now();
	test_scan_blocks();
	test_rc_defaults_and_store();
	test_rc_survey();
	test_rc_survey_ptt_and_traffic();
	printf("# roamTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
