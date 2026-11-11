// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR PA key-up (core/radio/pa_keyup.c, docs/spec/pa-keyup.md): each level's lead and boost from the cal table's
// drives, their limits, and the checks on the constants the debug link may set
#include <stdio.h>
#include "radio/pa_keyup.h"
#include "radio/apc_ramp.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// A loop with a dead zone at 90 % of the lowest level's drive (760 here: 684), its knee 120 us at the top drive
static const pa_keyup_cfg_t dflt = {
	.dead_pm = 900, .knee_us = 120, .slope_us = 250, .window_us = 1500, .settle_us = 300, .pa_us = 0xFFFF,
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
	uint16_t max_ahead = WALK_REF - WALK_MIN;
	pa_keyup_cfg_t c = {
		.dead_pm = (uint16_t)next_rand(1200), .knee_us = (uint16_t)next_rand(2500), .slope_us = (uint16_t)next_rand(1000),
		.window_us = (uint16_t)(1 + next_rand(max_ahead)),
	};
	c.settle_us = (uint16_t)next_rand(c.window_us);
	c.pa_us = next_rand(3) == 0 ? 0xFFFF : (uint16_t)next_rand(max_ahead + 1u);
	CHECK(pa_keyup_valid(&c, max_ahead));
	uint16_t top = (uint16_t)(500 + next_rand(3500));
	uint16_t d = (uint16_t)(1 + next_rand(top)), max = (uint16_t)(top + next_rand(4096u - top));
	pa_keyup_t k = pa_keyup_drives(&c, d, top, (uint16_t)next_rand(top), max);
	// What every key-up keeps, whatever the constants
	CHECK(k.drive == d && k.pa_us <= c.window_us && k.pa_us <= c.pa_us);
	if (k.boost != 0u) {
		CHECK(k.boost >= d && k.boost <= max && k.drive_us <= k.pa_us && k.pa_us <= k.boost_us && k.boost_us <= c.window_us);
		CHECK(k.boost == d || (k.boost_us == c.window_us && k.drive_us <= c.settle_us));
	}
	return pa_keyup_points(&k, pt);
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

// A key-up's ramp from the dead zone's drive (radio/apc_ramp.h): the steps in proportion, the ends exact
static void test_ramp_from(void)
{
	uint16_t base, span;
	CHECK(apc_ramp_from(1104, 607, 180, &base, &span) == 81 && base == 607 && span == 497);
	CHECK(base + apc_ramp_level(span, 0, 81) == 607 && base + apc_ramp_level(span, 81, 81) == 1104);
	for (uint16_t i = 1; i <= 81; i++) {	// never down, never past the target
		CHECK(apc_ramp_level(span, i, 81) >= apc_ramp_level(span, (uint16_t)(i - 1), 81) && apc_ramp_level(span, i, 81) <= span);
	}
	CHECK(apc_ramp_from(1104, 0, 180, &base, &span) == 180 && base == 0 && span == 1104);	// no dead zone: the full ramp
	CHECK(apc_ramp_from(1104, 1104, 180, &base, &span) == 180 && base == 0 && span == 1104);	// a floor at the target
	CHECK(apc_ramp_from(1104, 2000, 180, &base, &span) == 180 && base == 0);		// or over it: from 0
	CHECK(apc_ramp_from(1104, 1103, 180, &base, &span) == 1 && base == 1103 && span == 1);	// at least one step
	CHECK(apc_ramp_from(0, 0, 180, &base, &span) == 1 && span == 0);
}

int main(void)
{
	// Five levels of a table whose top drive is 1448: each keyed (knee * (top - dead) + slope * (d - dead)) / (d - dead)
	// before the first symbol, the PA with the drive, so a lower level starts earlier and none has a boost
	static const uint16_t d[5] = { 767, 890, 1023, 1235, 1448 };
	static const uint16_t lead[5] = { 1354, 695, 520, 416, 370 };
	for (int i = 0; i < 5; i++) {
		pa_keyup_t k = pa_keyup_drives(&dflt, d[i], 1448, 760, 4095);
		CHECK(k.boost == 0 && k.drive == d[i] && k.pa_us == lead[i]);
		if (i > 0) {
			CHECK(lead[i] < lead[i - 1]);
		}
	}
	// A level too slow for the window: the lowest boost that fits it, from the window to the settle time, then d
	pa_keyup_t k = pa_keyup_drives(&dflt, 700, 1448, 760, 4095);
	CHECK(k.boost == 764 && k.drive == 700 && k.boost_us == 1500 && k.pa_us == 1500 && k.drive_us == 300);
	pa_keyup_cfg_t c = dflt;
	c.knee_us = 2000;
	k = pa_keyup_drives(&c, 767, 1448, 760, 4095);
	CHECK(k.boost == 1737 && k.boost_us == 1500 && k.drive_us == 300);	// 120 % of the top drive: the ceiling
	CHECK(pa_keyup_drives(&c, 767, 1448, 760, 1500).boost == 1500);		// and the board's limit
	// The same constants on a radio whose table is 10 % higher everywhere: the same times, the boost 10 % higher
	k = pa_keyup_drives(&dflt, 844, 1593, 836, 4095);
	CHECK(k.boost == 0 && k.pa_us >= 1340 && k.pa_us <= 1370);
	k = pa_keyup_drives(&dflt, 770, 1593, 836, 4095);
	CHECK(k.boost >= 838 && k.boost <= 842 && k.boost_us == 1500);
	// A PA point of its own: the drive ahead of it charges the loop (a boost at d), the PA at pa_us; a level whose
	// drive comes later than that keys both together
	c = dflt;
	c.pa_us = 400;
	k = pa_keyup_drives(&c, 767, 1448, 760, 4095);
	CHECK(k.boost == 767 && k.boost_us == 1354 && k.pa_us == 400 && k.drive_us == 400);
	k = pa_keyup_drives(&c, 1448, 1448, 760, 4095);
	CHECK(k.boost == 0 && k.pa_us == 370);
	k = pa_keyup_drives(&c, 700, 1448, 760, 4095);
	CHECK(k.boost == 764 && k.boost_us == 1500 && k.pa_us == 400 && k.drive_us == 300);
	c.pa_us = 200;		// the PA after the boost's end: the drive back to d with it at the latest
	k = pa_keyup_drives(&c, 700, 1448, 760, 4095);
	CHECK(k.boost == 764 && k.pa_us == 200 && k.drive_us == 200);
	// A drive in the dead zone has nothing to time: keyed at the window, no boost. No constants: no boost, pa_us 0
	k = pa_keyup_drives(&dflt, 600, 1448, 760, 4095);
	CHECK(k.boost == 0 && k.pa_us == 1500 && k.drive == 600);
	k = pa_keyup_drives(NULL, 955, 2615, 0, 4095);
	CHECK(k.boost == 0 && k.drive == 955 && k.pa_us == 0);
	// A board keyed at one fixed time: every level there, with or without an anchor
	static const pa_keyup_cfg_t fixed = PA_KEYUP_FIXED(1530);
	for (int i = 0; i < 5; i++) {
		k = pa_keyup_drives(&fixed, d[i], 1448, i & 1 ? 760 : 0, 4095);
		CHECK(k.boost == 0 && k.pa_us == 1530);
	}

	// The points in time order; one point without a boost
	pa_keyup_point_t pt[PA_KEYUP_POINTS];
	k = pa_keyup_drives(&c, 700, 1448, 760, 4095);
	CHECK(pa_keyup_points(&k, pt) == 3);
	CHECK(pt[0].kind == PA_KEYUP_PT_BOOST && pt[0].us == 1500 && pt[1].kind == PA_KEYUP_PT_PA && pt[1].us == 200);
	CHECK(pt[2].kind == PA_KEYUP_PT_DRIVE && pt[2].us == 200);
	k = pa_keyup_drives(&dflt, 700, 1448, 760, 4095);
	CHECK(pa_keyup_points(&k, pt) == 3 && pt[0].us == 1500 && pt[1].us == 1500 && pt[2].us == 300);
	k = pa_keyup_drives(&dflt, 890, 1448, 760, 4095);
	CHECK(pa_keyup_points(&k, pt) == 1 && pt[0].kind == PA_KEYUP_PT_PA && pt[0].us == 695);

	// The checks on the constants
	CHECK(pa_keyup_valid(&dflt, 1580));
	CHECK(!pa_keyup_valid(&dflt, 1400));		// the window earlier than the board allows
	CHECK(pa_keyup_valid(&fixed, 1580) && !pa_keyup_valid(&fixed, 1500));
	c = dflt;
	c.settle_us = 1500;				// no room for a boost
	CHECK(!pa_keyup_valid(&c, 1580));
	CHECK(!pa_keyup_valid(NULL, 1580));

	// The board's copy: nothing before init, the board's constants after, a set checked whole, NULL restores
	CHECK(pa_keyup_cfg() == NULL && !pa_keyup_set(&dflt));
	CHECK(pa_keyup_plan(700, 1448, 760).boost == 0);
	pa_keyup_init(&dflt, 1580, 4095);
	CHECK(pa_keyup_cfg() != NULL && pa_keyup_cfg()->knee_us == 120);
	k = pa_keyup_plan(700, 1448, 760);
	CHECK(k.boost == 764 && k.boost_us == 1500 && k.drive_us == 300);
	// A band's constants replace the board's, but not ones the debug link set, until it gives them back
	static const pa_keyup_cfg_t band2 = { .dead_pm = 0, .knee_us = 500, .slope_us = 780, .window_us = 1500,
					      .settle_us = 300, .pa_us = 1500 };
	static const pa_keyup_cfg_t late = PA_KEYUP_FIXED(1600);
	pa_keyup_select(&late);		// past the limit: ignored
	CHECK(pa_keyup_cfg()->knee_us == 120);
	pa_keyup_select(&band2);
	CHECK(pa_keyup_cfg()->knee_us == 500);
	pa_keyup_cfg_t dbg = band2;
	dbg.knee_us = 700;
	CHECK(pa_keyup_set(&dbg));
	pa_keyup_select(&dflt);
	CHECK(pa_keyup_cfg()->knee_us == 700);
	CHECK(pa_keyup_set(NULL) && pa_keyup_cfg()->knee_us == 120);
	pa_keyup_select(&band2);
	CHECK(pa_keyup_cfg()->knee_us == 500);
	pa_keyup_select(&dflt);
	uint16_t lt = 0, lr = 0;
	CHECK(pa_keyup_last(&lt, &lr).drive_us == 300 && lt == 1448 && lr == 760);
	c = dflt;
	c.window_us = 2000;
	CHECK(!pa_keyup_set(&c) && pa_keyup_cfg()->window_us == 1500);
	c.window_us = 1000;
	CHECK(pa_keyup_set(&c) && pa_keyup_plan(890, 1448, 760).pa_us == 695 && pa_keyup_last(NULL, NULL).pa_us == 695);
	CHECK(pa_keyup_set(NULL) && pa_keyup_cfg()->window_us == 1500);

	// A board point: ahead of the points at its time, after the earlier ones; at the end when it is the latest
	pa_keyup_point_t ip[PA_KEYUP_POINTS_MAX] = { { 1500, PA_KEYUP_PT_BOOST }, { 900, PA_KEYUP_PT_PA },
						      { 500, PA_KEYUP_PT_DRIVE } };
	CHECK(pa_keyup_insert(ip, 3, 900, PA_KEYUP_PT_BOARD) == 4);
	CHECK(ip[1].kind == PA_KEYUP_PT_BOARD && ip[1].us == 900 && ip[2].kind == PA_KEYUP_PT_PA && ip[3].us == 500);
	pa_keyup_point_t ip2[PA_KEYUP_POINTS_MAX] = { { 1500, PA_KEYUP_PT_PA } };
	CHECK(pa_keyup_insert(ip2, 1, 1800, PA_KEYUP_PT_BOARD) == 2 && ip2[0].us == 1800 && ip2[1].kind == PA_KEYUP_PT_PA);
	CHECK(pa_keyup_insert(ip2, 2, 100, PA_KEYUP_PT_BOARD) == 3 && ip2[2].us == 100);

	// Steps: the points at one time together, the first no earlier than min after the reference
	pa_keyup_step_t st[PA_KEYUP_POINTS_MAX];
	pa_keyup_point_t sp[PA_KEYUP_POINTS_MAX] = { { 2520, PA_KEYUP_PT_BOOST }, { 2500, PA_KEYUP_PT_PA },
						      { 2489, PA_KEYUP_PT_BOARD }, { 500, PA_KEYUP_PT_DRIVE } };
	CHECK(pa_keyup_steps(sp, 4, 2530, 30, st) == 3);	// the first two at 2500: no earlier than 30 us after
	CHECK(st[0].us == 2500 && st[0].end == 2 && st[1].us == 2489 && st[1].end == 3 && st[2].us == 500 && st[2].end == 4);
	CHECK(pa_keyup_steps(sp, 4, 2530, 0, st) == 4 && st[0].us == 2520 && st[1].us == 2500);
	sp[2].us = 2500;					// one time, one step
	CHECK(pa_keyup_steps(sp, 4, 2530, 0, st) == 3 && st[1].us == 2500 && st[1].end == 3);
	CHECK(pa_keyup_steps(sp, 0, 2530, 30, st) == 0);
	CHECK(pa_keyup_step_wait(2500, 2489, 0, 30) == 0 && pa_keyup_step_wait(2489, 500, 0, 30) == 1989);
	CHECK(pa_keyup_step_wait(2489, 500, 1959, 30) == 0 && pa_keyup_step_wait(2489, 500, 1958, 30) == 1989);
	check_walks();
	test_ramp_from();

	if (failures == 0) {
		printf("paKeyupTest: all passed\n");
	}
	return failures != 0;
}
