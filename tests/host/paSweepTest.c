// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The PA drive sweep (core/radio/pa_sweep.c, docs/spec/pa-keyup.md 7): the grid, its limits over the top level's
// drive, the sweep's length cap, and what a run writes and when
#include <stdio.h>
#include "radio/pa_sweep.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// A run's record: every write with the fake clock's time
#define LOG_MAX 512
static struct { char what; int v; unsigned t; } logv[LOG_MAX];
static int nlog;
static unsigned now;
static int refuse_after = -1;	// the drive write refused from this many writes on (-1: never)
static int drives;

static void rec(char w, int v)
{
	if (nlog < LOG_MAX) {
		logv[nlog].what = w;
		logv[nlog].v = v;
		logv[nlog].t = now;
		nlog++;
	}
}

static bool drive(uint16_t d)
{
	if (refuse_after >= 0 && drives >= refuse_after) {
		return false;
	}
	drives++;
	rec('d', d);
	return true;
}

static bool mark(int8_t s)
{
	rec('m', s);
	return true;
}

static void delay_ms(uint32_t ms)
{
	now += ms;
}

static void reset(void)
{
	nlog = 0;
	now = 0;
	drives = 0;
	refuse_after = -1;
}

static void test_points(void)
{
	pa_sweep_req_t r = { .start = 100, .step = 100, .count = 50, .on_ms = 10, .off_ms = 5, .ceiling_pct = 100 };
	uint8_t over = 0;
	uint32_t ms = 0;
	pa_sweep_pt_t pt;
	// Up to the top level's drive and no further at 100 %
	int n = 0;
	while (pa_sweep_point(&r, 1000, 4095, (uint8_t)n, &over, &ms, &pt)) {
		n++;
	}
	CHECK(n == 10 && over == 0 && ms == 150);
	// 115 % at most, whatever is asked: 1150 is the last, and points over the top are short and far apart
	r.ceiling_pct = 200;
	over = 0;
	ms = 0;
	CHECK(pa_sweep_point(&r, 1000, 4095, 10, &over, &ms, &pt) && pt.drive == 1100 && over == 1);
	CHECK(pt.on_ms == 10 && pt.off_ms == PA_SWEEP_OVER_OFF_MIN_MS);
	CHECK(!pa_sweep_point(&r, 1000, 4095, 11, &over, &ms, &pt));
	r.on_ms = 200;
	CHECK(pa_sweep_point(&r, 1000, 4095, 10, &over, &ms, &pt) && pt.on_ms == PA_SWEEP_OVER_ON_MAX_MS);
	CHECK(pa_sweep_point(&r, 1000, 4095, 9, &over, &ms, &pt) && pt.on_ms == 200);	// at the top: as asked
	// The board's hardware limit under the ceiling
	CHECK(!pa_sweep_point(&r, 1000, 950, 9, &over, &ms, &pt));
	// No more than PA_SWEEP_OVER_MAX points over the top in one sweep
	pa_sweep_req_t o = { .start = 1001, .step = 1, .count = 100, .on_ms = 5, .off_ms = 40, .ceiling_pct = 115 };
	over = 0;
	ms = 0;
	n = 0;
	while (pa_sweep_point(&o, 1000, 4095, (uint8_t)n, &over, &ms, &pt)) {
		n++;
	}
	CHECK(n == PA_SWEEP_OVER_MAX);
	// Nothing at or under 0, nothing without a top drive or a length, a step down works
	pa_sweep_req_t d = { .start = 300, .step = -100, .count = 10, .on_ms = 5, .off_ms = 5, .ceiling_pct = 100 };
	over = 0;
	ms = 0;
	n = 0;
	while (pa_sweep_point(&d, 1000, 4095, (uint8_t)n, &over, &ms, &pt)) {
		n++;
	}
	CHECK(n == 3);
	CHECK(!pa_sweep_point(&d, 0, 4095, 0, &over, &ms, &pt));
	d.on_ms = 0;
	CHECK(!pa_sweep_point(&d, 1000, 4095, 0, &over, &ms, &pt));
	// The length cap: 255 + 255 ms points, so the third would pass PA_SWEEP_MS_MAX
	pa_sweep_req_t l = { .start = 10, .step = 10, .count = 50, .on_ms = 255, .off_ms = 255, .ceiling_pct = 100 };
	over = 0;
	ms = 0;
	n = 0;
	while (pa_sweep_point(&l, 1000, 4095, (uint8_t)n, &over, &ms, &pt)) {
		n++;
	}
	CHECK(n == (int)(PA_SWEEP_MS_MAX / 510u) && ms <= PA_SWEEP_MS_MAX);
}

static void test_run(void)
{
	const pa_sweep_io_t io = { .drive = drive, .mark = mark, .delay_ms = delay_ms };
	pa_sweep_req_t r = { .start = 500, .step = 100, .count = 3, .on_ms = 20, .off_ms = 10, .ceiling_pct = 100, .mark_ms = 5 };
	reset();
	CHECK(pa_sweep_run(&r, 1000, 4095, &io) == 3);
	// Per point: 0, the off time, the drive, the mark up (then down) mark_ms later, centered mark_ms before the drop
	CHECK(logv[0].what == 'd' && logv[0].v == 0 && logv[0].t == 0);
	CHECK(logv[1].what == 'd' && logv[1].v == 500 && logv[1].t == 10);
	CHECK(logv[2].what == 'm' && logv[2].v == 1 && logv[2].t == 15);
	CHECK(logv[3].what == 'm' && logv[3].v == 0 && logv[3].t == 25);
	CHECK(logv[4].what == 'd' && logv[4].v == 0 && logv[4].t == 30);
	CHECK(logv[6].what == 'm' && logv[6].v == -1);	// the next point's mark the other way
	CHECK(logv[9].what == 'd' && logv[9].v == 700 && logv[9].t == 70);
	// The end: the drive at 0 and the mark centered
	CHECK(logv[nlog - 2].what == 'd' && logv[nlog - 2].v == 0 && logv[nlog - 2].t == 90);
	CHECK(logv[nlog - 1].what == 'm' && logv[nlog - 1].v == 0);
	// A point too short for two marks runs without them
	r.on_ms = 10;
	reset();
	CHECK(pa_sweep_run(&r, 1000, 4095, &io) == 3);
	int marks = 0;
	for (int i = 0; i < nlog - 1; i++) {
		marks += logv[i].what == 'm';
	}
	CHECK(marks == 0);
	// Without a marker hook
	const pa_sweep_io_t nomark = { .drive = drive, .mark = NULL, .delay_ms = delay_ms };
	r.on_ms = 20;
	reset();
	CHECK(pa_sweep_run(&r, 1000, 4095, &nomark) == 3);
	// The transmission ends partway: the board refuses the writes, and the run stops there
	reset();
	refuse_after = 3;	// 0, 500, 0 accepted; 600 refused
	CHECK(pa_sweep_run(&r, 1000, 4095, &io) == 1);
}

int main(void)
{
	test_points();
	test_run();
	if (failures) {
		printf("paSweepTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("paSweepTest: all passed\n");
	return 0;
}
