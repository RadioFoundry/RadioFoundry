// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The quadrature decoder (drivers/input/quadrature): clean turns, contact bounce, wobble around a detent, reversals,
// missed samples, fast turns, the consumer's take, and a random walk with bounce against the true count
#include <stdio.h>
#include <stdlib.h>
#include "input/quadrature/quadrature.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// The Gray sequence forward (+1 per step): 00 01 11 10
static const uint8_t gray[4] = { 0, 1, 3, 2 };

static quad_dec_t q;
static int pos;			// index into gray[] of the lines' state now
static int events;		// nonzero results of quad_sample
static int backward;		// results against the direction being turned

static void sample(uint8_t ab, int dir)
{
	int8_t d = quad_sample(&q, ab);
	if (d != 0) {
		events++;
		if (dir != 0 && d != dir)
			backward++;
	}
}

// One step of the lines in direction dir, the changing line bouncing `bounce` times first (each bounce is two extra
// samples: the new level, then the old one again)
static void step(int dir, int bounce)
{
	int next = (pos + (dir > 0 ? 1 : 3)) & 3;
	for (int i = 0; i < bounce; i++) {
		sample(gray[next], dir);
		sample(gray[pos], dir);
	}
	pos = next;
	sample(gray[pos], dir);
}

static void turn(int detents, int steps_per_detent, int bounce)
{
	int dir = detents > 0 ? 1 : -1;
	for (int d = 0; d < (detents > 0 ? detents : -detents); d++)
		for (int s = 0; s < steps_per_detent; s++)
			step(dir, bounce);
}

static void reset(uint8_t steps, uint8_t rest, int start)
{
	pos = start;
	events = backward = 0;
	quad_init(&q, steps, rest, gray[pos]);
}

// The decoder this replaced: one count per B edge, -1 if A == B after the edge, else +1, from levels read in the
// interrupt (which may come late and see a later level)
static int legacy_count;
static void legacy_b_edge(uint8_t ab)
{
	int a = (ab >> 1) & 1, b = ab & 1;
	legacy_count += a == b ? -1 : 1;
}

int main(void)
{
	// Clean turns, two steps per detent (rest where A == B), both ways
	reset(2, QUAD_REST_EQUAL, 0);
	turn(10, 2, 0);
	CHECK(q.detents == 10 && events == 10 && backward == 0);
	CHECK(q.transitions == 20 && q.illegal == 0 && q.reversals == 0);
	turn(-7, 2, 0);
	CHECK(q.detents == 3 && events == 17 && backward == 0);
	CHECK(q.reversals == 1);

	// Heavy bounce on every edge: still exactly one count per detent, never one backward
	reset(2, QUAD_REST_EQUAL, 0);
	turn(25, 2, 3);
	CHECK(q.detents == 25 && events == 25 && backward == 0);
	turn(-25, 2, 5);
	CHECK(q.detents == 0 && events == 50 && backward == 0);
	CHECK(q.illegal == 0);

	// Wobble at a detent: the first line leaves the rest state and comes back, many times. Nothing counts
	reset(2, QUAD_REST_EQUAL, 0);
	for (int i = 0; i < 20; i++) {
		step(1, 0);
		step(-1, 0);
		step(-1, 0);
		step(1, 0);
	}
	CHECK(q.detents == 0 && events == 0);
	CHECK(q.reversals > 0);

	// Half a detent, back to where it started, then a whole one: one count, forward
	reset(2, QUAD_REST_EQUAL, 0);
	step(1, 2);
	step(-1, 2);
	turn(1, 2, 2);
	CHECK(q.detents == 1 && events == 1 && backward == 0);

	// Reversal halfway through a detent (the knob turned back before the click)
	reset(2, QUAD_REST_EQUAL, 0);
	turn(3, 2, 1);
	step(1, 1);		// half way to the 4th
	step(-1, 1);		// back to the 3rd
	turn(-2, 2, 1);
	CHECK(q.detents == 1 && events == 5 && backward == 0);

	// A missed sample (both lines changed between two samples): counted as illegal, ignored. The detent it fell in is
	// lost, the ones after it count, nothing ever goes backward
	reset(2, QUAD_REST_EQUAL, 0);
	turn(2, 2, 0);				// pos 0 (00) again, detents 2
	sample(gray[2], 1);			// 00 -> 11 in one sample
	pos = 2;
	CHECK(q.illegal == 1 && q.detents == 2 && backward == 0);
	turn(3, 2, 1);
	CHECK(q.detents == 5 && backward == 0);
	// Missed in the middle of a detent going the other way
	step(-1, 0);
	sample(gray[(pos + 2) & 3], -1);
	pos = (pos + 2) & 3;
	turn(-2, 2, 0);
	CHECK(q.illegal == 2 && backward == 0);
	CHECK(q.detents == 3);		// 3 back of the 4 crossed: the one in the gap is lost, none gained

	// Fast turn: the board samples only on edges, so every sample is a new state; 200 detents in a row
	reset(2, QUAD_REST_EQUAL, 0);
	turn(200, 2, 0);
	CHECK(q.detents == 200 && events == 200);

	// Four steps per detent, resting at 11
	reset(4, QUAD_REST_11, 2);	// gray[2] = 11
	turn(6, 4, 2);
	CHECK(q.detents == 6 && events == 6 && backward == 0);
	turn(-6, 4, 2);
	CHECK(q.detents == 0 && events == 12 && backward == 0);
	// three of the four steps and back: nothing
	for (int s = 0; s < 3; s++)
		step(1, 0);
	for (int s = 0; s < 3; s++)
		step(-1, 0);
	CHECK(q.detents == 0 && events == 12);

	// One step per detent: every state counts (bounce can't be told from a turn here; the total still comes out right)
	reset(1, QUAD_REST_ALL, 0);
	turn(9, 1, 0);
	CHECK(q.detents == 9 && events == 9 && backward == 0);
	turn(9, 1, 2);
	CHECK(q.detents == 18);

	// The consumer: takes what accumulated, at most 127 at a time, the rest on the next take
	reset(2, QUAD_REST_EQUAL, 0);
	int32_t taken = 0;
	turn(300, 2, 0);
	CHECK(quad_take(&q, &taken) == 127);
	CHECK(quad_take(&q, &taken) == 127);
	CHECK(quad_take(&q, &taken) == 46);
	CHECK(quad_take(&q, &taken) == 0);
	turn(-5, 2, 0);
	CHECK(quad_take(&q, &taken) == -5);
	CHECK(taken == 295);

	// Random walk with random bounce: the decoder's total is the number of detents actually turned
	srand(1701);
	for (int run = 0; run < 50; run++) {
		reset(2, QUAD_REST_EQUAL, 0);
		int truth = 0, half = 0;
		for (int i = 0; i < 400; i++) {
			int dir = (rand() & 3) == 0 ? -1 : 1;
			if (run & 1)
				dir = -dir;
			step(dir, rand() % 4);
			half += dir;
			if (gray[pos] == 0 || gray[pos] == 3) {	// at a rest state
				if (half == 2 || half == -2)
					truth += half / 2;
				half = 0;
			}
		}
		CHECK(q.detents == truth);
		CHECK(q.illegal == 0);
	}

	// Why the B-edge decoder miscounted (the reasons in docs/spec/hardware-dm1701.md 10.3):
	// 1. A wobble at a detent moves B (on a two-step detent B changes first going one way): every wobble gives a count
	//    and then a count back, so the screen jumps one row and back
	legacy_count = 0;
	int legacy_events = 0;
	for (int i = 0; i < 5; i++) {
		legacy_b_edge(gray[1]);		// 00 -> 01: B rose
		legacy_events += legacy_count != 0;
		legacy_b_edge(gray[0]);		// 01 -> 00: B fell
	}
	CHECK(legacy_count == 0 && legacy_events == 5);
	// 2. Two bounce edges of B close together give one interrupt (one pending flag); it reads the level after both,
	//    so a rise-fall-rise bounce reads "risen" once per interrupt it got. Interrupts at the first and third edge
	//    of three count two steps for one
	legacy_count = 0;
	legacy_b_edge(gray[1]);		// first edge: B high
	legacy_b_edge(gray[1]);		// edges 2 and 3 merged: B high again
	CHECK(legacy_count == 2);
	// The table decoder given the same samples: the repeated state is no change
	reset(2, QUAD_REST_EQUAL, 0);
	sample(gray[1], 1);
	sample(gray[1], 1);
	sample(gray[2], 1);
	CHECK(q.detents == 1 && events == 1);

	if (failures) {
		printf("quadratureTest: %d of %d checks failed\n", failures, checks);
		return 1;
	}
	printf("quadratureTest: all %d checks passed\n", checks);
	return 0;
}
