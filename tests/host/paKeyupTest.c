// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR PA key-up (core/radio/pa_keyup.c, docs/spec/pa-keyup.md): the boost, its length and the hold from the cal
// table's drives, their limits, and the checks on the constants the debug link may set
#include <stdio.h>
#include "radio/pa_keyup.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const pa_keyup_cfg_t dflt = {
	.dead = 680, .knee = 1450, .slope_us = 600, .boost_pct = 120, .hold_pct = 80,
	.boost_us = 1500, .pa_us = 1500, .drive_us = 500, .late_pct = 100,
};

// ---- The timer walks (pa_keyup_steps, pa_keyup_step_wait) against the arithmetic the boards used before them ----

// A run of a walk: for each point the timer update (us after the reference) it was applied at, each programmed wait
// and the walk's last base (a board may time its PA drop from it)
typedef struct {
	uint32_t at[PA_KEYUP_POINTS_MAX];
	uint32_t waits[PA_KEYUP_POINTS_MAX + 1];
	uint8_t nwaits, napplied;
	uint16_t base;
} walk_t;

static uint32_t rnd = 12345u;
static uint32_t next_rand(uint32_t n)
{
	rnd = rnd * 1103515245u + 12345u;
	return (rnd >> 8) % n;
}

// The handler's model: it enters lat us after an update, each point costs cost us, and TIM7's count runs on from
// the update (ARR 0xFFFF while the points run), so the next update comes the programmed wait after the last one
#define WALK_REF	2530u
#define WALK_MIN	30u

// A free-running walk (the timer counts on from each update) as boards did it before the shared steps: each point
// checked against the timer's base and count
static walk_t old_walk(const pa_keyup_point_t *pt, uint8_t n, uint32_t lat, uint32_t cost)
{
	walk_t w = { 0 };
	uint32_t first = pt[0].us;
	uint32_t t = first + WALK_MIN < WALK_REF ? WALK_REF - first : WALK_MIN;	// the first update after the reference
	w.waits[w.nwaits++] = t;
	uint16_t base = (uint16_t)(first + WALK_MIN < WALK_REF ? first : WALK_REF - WALK_MIN);
	uint8_t i = 0;
	for (;;) {	// one handler entry
		uint32_t cnt = lat;
		for (;;) {
			w.at[w.napplied++] = t;
			cnt += cost;
			if (++i >= n) {
				w.base = base;
				return w;
			}
			uint32_t step = (uint32_t)base - pt[i].us;
			if (cnt + WALK_MIN < step) {
				w.waits[w.nwaits++] = step;
				t += step;
				base = pt[i].us;
				break;
			}
		}
	}
}

static walk_t new_walk(const pa_keyup_point_t *pt, uint8_t n, uint32_t lat, uint32_t cost)
{
	walk_t w = { 0 };
	pa_keyup_step_t st[PA_KEYUP_POINTS_MAX];
	uint8_t ns = pa_keyup_steps(pt, n, WALK_REF, WALK_MIN, st);
	uint16_t base = st[0].us;
	uint32_t t = (uint32_t)(WALK_REF - base);
	w.waits[w.nwaits++] = t;
	uint8_t i = 0;
	for (;;) {
		uint32_t cnt = lat;
		for (;;) {
			for (uint8_t p = i != 0u ? st[i - 1u].end : 0u; p < st[i].end; p++) {
				w.at[w.napplied++] = t;
				cnt += cost;
			}
			if (++i >= ns) {
				w.base = base;
				return w;
			}
			uint32_t step = pa_keyup_step_wait(base, st[i].us, cnt, WALK_MIN);
			if (step != 0u) {
				w.waits[w.nwaits++] = step;
				t += step;
				base = st[i].us;
				break;
			}
		}
	}
}

// A one-shot walk as boards did it before them: the timer restarted per group of points at one time, waits from point
// to point
static uint8_t old_waits_oneshot(const pa_keyup_point_t *pt, uint8_t n, uint32_t *waits)
{
	uint8_t nw = 0;
	waits[nw++] = WALK_REF - pt[0].us;
	for (uint8_t i = 1; i < n; i++) {
		if (pt[i].us < pt[i - 1u].us) {
			waits[nw++] = (uint32_t)pt[i - 1u].us - pt[i].us;
		}
	}
	return nw;
}

static uint8_t new_waits_oneshot(const pa_keyup_point_t *pt, uint8_t n, uint32_t *waits)
{
	pa_keyup_step_t st[PA_KEYUP_POINTS_MAX];
	uint8_t ns = pa_keyup_steps(pt, n, WALK_REF, 0u, st), nw = 0;
	waits[nw++] = WALK_REF - st[0].us;
	for (uint8_t i = 1; i < ns; i++) {
		waits[nw++] = (uint32_t)st[i - 1u].us - st[i].us;
	}
	CHECK(st[ns - 1u].end == n);
	return nw;
}

// Random key-ups as the rule makes them: constants within pa_keyup_valid for the reference, a drive and a top
static uint8_t random_points(pa_keyup_point_t *pt)
{
	pa_keyup_cfg_t c = {
		.dead = (uint16_t)next_rand(900), .knee = (uint16_t)next_rand(2500), .slope_us = (uint16_t)next_rand(1000),
		.boost_pct = (uint8_t)(50 + next_rand(71)), .hold_pct = (uint8_t)next_rand(101),
		.late_pct = (uint8_t)next_rand(101),
	};
	uint16_t max_ahead = WALK_REF - WALK_MIN;
	c.boost_us = next_rand(8) == 0 ? 0 : (uint16_t)(1 + next_rand(max_ahead));
	uint16_t lim = c.boost_us != 0 ? c.boost_us : max_ahead;
	c.pa_us = (uint16_t)next_rand(lim + 1u);
	c.drive_us = (uint16_t)next_rand(lim + 1u);
	if (next_rand(6) == 0) {
		c.pa_us = c.drive_us = c.boost_us;	// ties
	}
	CHECK(pa_keyup_valid(&c, max_ahead));
	uint16_t top = (uint16_t)(500 + next_rand(3500));
	pa_keyup_t k = pa_keyup_drives(&c, (uint16_t)(1 + next_rand(top)), top, (uint16_t)next_rand(300), 4095);
	return pa_keyup_points(&c, &k, pt);
}

static void check_walks(void)
{
	int compared = 0, merged = 0;
	for (int run = 0; run < 20000; run++) {
		pa_keyup_point_t pt[PA_KEYUP_POINTS_MAX];
		uint8_t n = random_points(pt);
		if (next_rand(2) == 0) {	// a board point (a supply, a switch) among them
			n = pa_keyup_insert(pt, n, (uint16_t)(pt[n - 1u].us + next_rand(WALK_REF - WALK_MIN - pt[n - 1u].us + 1u)),
					    PA_KEYUP_PT_BOARD);
		}
		for (uint8_t i = 1; i < n; i++) {
			CHECK(pt[i].us <= pt[i - 1u].us);
		}
		uint32_t lat = next_rand(4) == 0 ? next_rand(200) : next_rand(15), cost = next_rand(25);
		walk_t a = old_walk(pt, n, lat, cost), b = new_walk(pt, n, lat, cost);
		CHECK(a.napplied == n && b.napplied == n && a.nwaits == b.nwaits && a.base == b.base);
		for (uint8_t i = 0; i < n; i++) {
			CHECK(a.at[i] == b.at[i]);
		}
		for (uint8_t i = 0; i < a.nwaits; i++) {
			CHECK(a.waits[i] == b.waits[i]);
		}
		uint32_t wa[PA_KEYUP_POINTS_MAX + 1], wb[PA_KEYUP_POINTS_MAX + 1];
		uint8_t na = old_waits_oneshot(pt, n, wa), nb = new_waits_oneshot(pt, n, wb);
		CHECK(na == nb);
		for (uint8_t i = 0; i < na && i < nb; i++) {
			CHECK(wa[i] == wb[i]);
		}
		merged += a.nwaits < n;
		compared++;
	}
	CHECK(compared == 20000 && merged > 1000);	// the merging was exercised
}

int main(void)
{
	// A UHF table with a top drive of 2615: the boost is 3138 at every step, and lasts
	// (1450000 + 600 * (d - 680)) / (3138 - 680) us, so a higher step's boost is longer. Every boost ends where the top
	// step's does (here at the drive point), so a lower step's starts later by what it is shorter
	static const uint16_t d[5] = { 955, 1249, 1613, 2101, 2615 };
	static const uint16_t len[5] = { 657, 728, 817, 936, 1000 };
	static const uint16_t hold[5] = { 764, 999, 1290, 1680, 2092 };
	for (int i = 0; i < 5; i++) {
		pa_keyup_t k = pa_keyup_drives(&dflt, d[i], 2615, 0, 4095);
		CHECK(k.boost == 3138 && k.hold == hold[i] && k.drive == d[i] && k.hold_us == 500 && k.late_us == 1000 - len[i]);
	}

	// late_pct: how much of that goes to a later start. 0 starts every step at boost_us
	pa_keyup_cfg_t lp = dflt;
	lp.late_pct = 0;
	pa_keyup_t k0 = pa_keyup_drives(&lp, 955, 2615, 0, 4095);
	CHECK(k0.late_us == 0 && k0.hold_us == 1500 - 657);
	lp.late_pct = 50;
	k0 = pa_keyup_drives(&lp, 955, 2615, 0, 4095);
	CHECK(k0.late_us == 171 && k0.hold_us == 1500 - 171 - 657);
	lp.late_pct = 101;
	CHECK(!pa_keyup_valid(&lp, 1580));

	// The boost never over the limit, and it ends by the point the drive goes to the step's: a short lead cuts it
	pa_keyup_t k = pa_keyup_drives(&dflt, 2615, 2615, 0, 2700);
	CHECK(k.boost == 2700 && k.hold_us == 500);
	pa_keyup_cfg_t c = dflt;
	c.boost_us = 900;
	c.pa_us = 900;
	k = pa_keyup_drives(&c, 955, 2615, 0, 4095);
	CHECK(k.boost == 3138 && k.hold_us == 500);
	// A drive under the dead zone still gets the knee's boost, and holds at its own drive
	k = pa_keyup_drives(&dflt, 500, 2615, 0, 4095);
	CHECK(k.boost == 3138 && k.hold_us == 500 && k.late_us == 1000 - 589 && k.hold == 500);
	// The hold never under the dead zone: 80 % of 800 is 640, held at 681
	k = pa_keyup_drives(&dflt, 800, 2615, 0, 4095);
	CHECK(k.boost == 3138 && k.hold == 681 && k.drive == 800);

	// No boost: no table drive, no constants, boost_us 0, a boost inside the dead zone or under the step's drive (a
	// calibration drive over the table's top): the PA keyed at pa_us at the drive, as before
	pa_keyup_cfg_t off = dflt;
	off.boost_us = 0;
	k = pa_keyup_drives(&off, 955, 2615, 0, 4095);
	CHECK(k.boost == 0 && k.hold == 0 && k.drive == 955);
	k = pa_keyup_drives(NULL, 955, 2615, 0, 4095);
	CHECK(k.boost == 0 && k.drive == 955);
	k = pa_keyup_drives(&dflt, 0, 2615, 0, 4095);
	CHECK(k.boost == 0 && k.drive == 0);
	k = pa_keyup_drives(&dflt, 300, 500, 0, 4095);
	CHECK(k.boost == 0 && k.drive == 300);
	k = pa_keyup_drives(&dflt, 3500, 2615, 0, 4095);
	CHECK(k.boost == 0 && k.drive == 3500);

	// A calibration floor (the drive where the output reaches zero): dead counts from it, the hold stays above it,
	// and a drive under it gets no boost
	pa_keyup_cfg_t fl = { .dead = 50, .knee = 100, .slope_us = 0, .boost_pct = 100, .hold_pct = 50,
			      .boost_us = 1500, .pa_us = 1500, .drive_us = 500 };
	k = pa_keyup_drives(&fl, 600, 2000, 440, 2300);
	CHECK(k.boost == 2000 && k.hold == 520 && k.hold_us == 1500 - 66 && k.late_us == 0);
	fl.hold_pct = 0;	// held just above the floor's dead zone
	CHECK(pa_keyup_drives(&fl, 600, 2000, 440, 2300).hold == 491);
	CHECK(pa_keyup_drives(&fl, 400, 2000, 440, 2300).boost == 0);

	// The points in time order, the enum's order on a tie; one point without a boost
	pa_keyup_point_t pt[PA_KEYUP_POINTS];
	k = pa_keyup_drives(&dflt, 955, 2615, 0, 4095);
	CHECK(pa_keyup_points(&dflt, &k, pt) == 4);
	// The boost and the PA with it start 343 us late: the step's boost is that much shorter than the top step's
	CHECK(pt[0].kind == PA_KEYUP_PT_BOOST && pt[0].us == 1157 && pt[1].kind == PA_KEYUP_PT_PA && pt[1].us == 1157);
	CHECK(pt[2].kind == PA_KEYUP_PT_HOLD && pt[2].us == 500 && pt[3].kind == PA_KEYUP_PT_DRIVE && pt[3].us == 500);
	c = dflt;
	c.pa_us = 1200;		// a PA point of its own stays, until the boost starts after it
	CHECK(pa_keyup_points(&c, &k, pt) == 4 && pt[0].kind == PA_KEYUP_PT_BOOST && pt[1].kind == PA_KEYUP_PT_PA && pt[1].us == 1157);
	pa_keyup_t top = pa_keyup_drives(&dflt, 2615, 2615, 0, 4095);
	CHECK(top.late_us == 0 && pa_keyup_points(&c, &top, pt) == 4 && pt[0].us == 1500 && pt[1].us == 1200);
	c = dflt;
	c.pa_us = 300;
	CHECK(pa_keyup_points(&c, &k, pt) == 4 && pt[3].kind == PA_KEYUP_PT_PA && pt[3].us == 300);
	CHECK(pt[0].kind == PA_KEYUP_PT_BOOST && pt[1].kind == PA_KEYUP_PT_HOLD && pt[2].kind == PA_KEYUP_PT_DRIVE);
	k = pa_keyup_drives(&off, 955, 2615, 0, 4095);
	CHECK(pa_keyup_points(&off, &k, pt) == 1 && pt[0].kind == PA_KEYUP_PT_PA && pt[0].us == off.pa_us);

	// The checks on the constants
	CHECK(pa_keyup_valid(&dflt, 1580));
	CHECK(!pa_keyup_valid(&dflt, 1400));		// the boost earlier than the board allows
	off.pa_us = 300;
	CHECK(pa_keyup_valid(&off, 1400));		// no boost: only the PA's point counts
	c = dflt;
	c.boost_pct = PA_KEYUP_BOOST_MAX_PCT + 1u;
	CHECK(!pa_keyup_valid(&c, 1580));
	c = dflt;
	c.boost_pct = 49;
	CHECK(!pa_keyup_valid(&c, 1580));
	c = dflt;
	c.hold_pct = 101;
	CHECK(!pa_keyup_valid(&c, 1580));
	c = dflt;
	c.drive_us = 1600;				// the drive's point before the boost
	CHECK(!pa_keyup_valid(&c, 1580));
	c = dflt;
	c.pa_us = 1550;					// the PA before the boost
	CHECK(!pa_keyup_valid(&c, 1580));
	CHECK(!pa_keyup_valid(NULL, 1580));

	// The board's copy: nothing before init, the board's constants after, a set checked whole, NULL restores
	CHECK(pa_keyup_cfg() == NULL && !pa_keyup_set(&dflt));
	pa_keyup_cfg_t used = { .knee = 7 };
	CHECK(pa_keyup_plan(955, 2615, 0, &used).boost == 0 && used.knee == 7);
	pa_keyup_init(&dflt, 1580, 4095);
	CHECK(pa_keyup_cfg() != NULL && pa_keyup_cfg()->knee == 1450);
	k = pa_keyup_plan(955, 2615, 0, &used);
	CHECK(k.boost == 3138 && k.hold == 764 && k.hold_us == 500 && k.late_us == 343 && used.knee == 1450 && used.boost_us == 1500);
	// A band's constants replace the board's, but not ones the debug link set, until it gives them back
	static const pa_keyup_cfg_t band2 = { .dead = 0, .knee = 500, .slope_us = 780, .boost_pct = 120, .hold_pct = 100,
					      .boost_us = 1500, .pa_us = 1500, .drive_us = 300 };
	static const pa_keyup_cfg_t late = { .boost_pct = 100, .hold_pct = 100, .boost_us = 1600, .pa_us = 1600 };
	pa_keyup_select(&late);		// past the limit: ignored
	CHECK(pa_keyup_cfg()->knee == 1450);
	pa_keyup_select(&band2);
	CHECK(pa_keyup_cfg()->knee == 500);
	pa_keyup_cfg_t dbg = band2;
	dbg.knee = 700;
	CHECK(pa_keyup_set(&dbg));
	pa_keyup_select(&dflt);
	CHECK(pa_keyup_cfg()->knee == 700);
	CHECK(pa_keyup_set(NULL) && pa_keyup_cfg()->knee == 1450);
	pa_keyup_select(&band2);
	CHECK(pa_keyup_cfg()->knee == 500);
	pa_keyup_select(&dflt);
	CHECK(pa_keyup_last().hold_us == 500);
	c = dflt;
	c.knee = 1229;
	CHECK(pa_keyup_set(&c) && pa_keyup_cfg()->knee == 1229);
	CHECK(pa_keyup_plan(680, 2615, 0, NULL).hold_us == 528);	// boosts of 500 and (the top step's) 972 us
	CHECK(pa_keyup_plan(955, 2615, 0, &used).boost == 3138 && used.knee == 1229);
	c.boost_us = 2000;
	CHECK(!pa_keyup_set(&c) && pa_keyup_cfg()->boost_us == 1500);
	CHECK(pa_keyup_set(NULL) && pa_keyup_cfg()->knee == 1450 && pa_keyup_cfg()->boost_pct == 120);

	// A board point: ahead of the points at its time, after the earlier ones; at the end when it is the latest
	pa_keyup_point_t ip[PA_KEYUP_POINTS_MAX] = { { 1500, PA_KEYUP_PT_BOOST }, { 1500, PA_KEYUP_PT_PA },
						      { 900, PA_KEYUP_PT_HOLD }, { 500, PA_KEYUP_PT_DRIVE } };
	CHECK(pa_keyup_insert(ip, 4, 900, PA_KEYUP_PT_BOARD) == 5);
	CHECK(ip[2].kind == PA_KEYUP_PT_BOARD && ip[2].us == 900 && ip[3].kind == PA_KEYUP_PT_HOLD && ip[4].us == 500);
	pa_keyup_point_t ip2[PA_KEYUP_POINTS_MAX] = { { 1500, PA_KEYUP_PT_PA } };
	CHECK(pa_keyup_insert(ip2, 1, 1800, PA_KEYUP_PT_BOARD) == 2 && ip2[0].us == 1800 && ip2[1].kind == PA_KEYUP_PT_PA);
	CHECK(pa_keyup_insert(ip2, 2, 100, PA_KEYUP_PT_BOARD) == 3 && ip2[2].us == 100);

	// Steps: the points at one time together, the first no earlier than min after the reference
	pa_keyup_step_t st[PA_KEYUP_POINTS_MAX];
	pa_keyup_point_t sp[PA_KEYUP_POINTS_MAX] = { { 2520, PA_KEYUP_PT_BOOST }, { 2500, PA_KEYUP_PT_PA },
						      { 2489, PA_KEYUP_PT_HOLD }, { 500, PA_KEYUP_PT_DRIVE } };
	CHECK(pa_keyup_steps(sp, 4, 2530, 30, st) == 3);	// the first two at 2500: no earlier than 30 us after
	CHECK(st[0].us == 2500 && st[0].end == 2 && st[1].us == 2489 && st[1].end == 3 && st[2].us == 500 && st[2].end == 4);
	CHECK(pa_keyup_steps(sp, 4, 2530, 0, st) == 4 && st[0].us == 2520 && st[1].us == 2500);
	sp[2].us = 2500;					// one time, one step
	CHECK(pa_keyup_steps(sp, 4, 2530, 0, st) == 3 && st[1].us == 2500 && st[1].end == 3);
	CHECK(pa_keyup_steps(sp, 0, 2530, 30, st) == 0);
	CHECK(pa_keyup_step_wait(2500, 2489, 0, 30) == 0 && pa_keyup_step_wait(2489, 500, 0, 30) == 1989);
	CHECK(pa_keyup_step_wait(2489, 500, 1959, 30) == 0 && pa_keyup_step_wait(2489, 500, 1958, 30) == 1989);
	check_walks();

	if (failures == 0) {
		printf("paKeyupTest: all passed\n");
	}
	return failures != 0;
}
