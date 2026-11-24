// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fox mode (core/radio/radio_fox.c): the record (defaults, round trip, what reads as absent, the interval's bounds),
// the jingle (the record's steps, the default tune, the length cap), the length of a cycle, and the schedule (the parts in
// order with their gaps, the jingle's repeats, skipped parts, the wait, a busy retry, a cut cycle, stop, the timer wrapping)
#include <stdio.h>
#include <string.h>
#include "radio/radio_fox.h"
#include "radio/morse.h"
#include "radio/sigtone.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_rec(void)
{
	fox_rec_t r, d;
	uint8_t b[FOX_REC_LEN];

	fox_rec_default(&r);
	CHECK(r.interval_s == FOX_INTERVAL_DEFAULT_S && r.flags == (FOX_F_MSG | FOX_F_JINGLE) && r.repeats == 1);
	CHECK(fox_msg_len(&r) == 3 && memcmp(r.msg, "FOX", 3) == 0 && r.ms10[0] == 0);

	// round trip, a full message (no terminator) and every step set
	memset(&r, 0, sizeof r);
	r.interval_s = 0x0102;	// 258 s: both bytes in use
	r.flags = FOX_F_JINGLE;
	r.repeats = FOX_REPEATS_MAX;
	memcpy(r.msg, "FOX 1 DE TEST/QR", FOX_MSG_MAX);
	for (unsigned i = 0; i < FOX_STEPS; i++) {
		r.hz[i] = (uint16_t)(300 + 10 * i);
		r.ms10[i] = (uint8_t)(1 + i);
	}
	r.hz[5] = 0;	// a pause
	fox_rec_encode(&r, b);
	CHECK(b[0] == 0x02 && b[1] == 0x01 && b[2] == FOX_F_JINGLE && b[3] == FOX_REPEATS_MAX && b[4] == 'F');
	CHECK(fox_rec_decode(b, FOX_REC_LEN, &d));
	CHECK(memcmp(&r, &d, sizeof r) == 0 && fox_msg_len(&d) == FOX_MSG_MAX);

	// a shorter record reads with the rest zero: no step (the default tune), the message to its end
	CHECK(fox_rec_decode(b, 4 + FOX_MSG_MAX, &d));
	CHECK(d.ms10[0] == 0 && d.hz[0] == 0 && fox_msg_len(&d) == FOX_MSG_MAX);
	CHECK(fox_rec_decode(b, 2, &d) && d.interval_s == 0x0102 && d.flags == 0 && fox_msg_len(&d) == 0);
	CHECK(d.repeats == 1);	// none stored: once
	CHECK(!fox_rec_decode(b, 1, &d));	// half an interval: 2 s, under the shortest
	CHECK(!fox_rec_decode(b, 0, &d));
	// a longer one (a later version) is read from the front
	uint8_t big[FOX_REC_LEN + 8];
	memcpy(big, b, FOX_REC_LEN);
	memset(big + FOX_REC_LEN, 0xAA, 8);
	CHECK(fox_rec_decode(big, sizeof big, &d) && memcmp(&r, &d, sizeof r) == 0);

	// what the fox can't take reads as absent: nothing is clamped
	uint8_t x[FOX_REC_LEN];
	memcpy(x, b, sizeof x);
	x[0] = FOX_INTERVAL_MIN_S - 1u;	// under the shortest wait
	x[1] = 0;
	CHECK(!fox_rec_decode(x, FOX_REC_LEN, &d));
	x[0] = FOX_INTERVAL_MIN_S;
	CHECK(fox_rec_decode(x, FOX_REC_LEN, &d) && d.interval_s == FOX_INTERVAL_MIN_S);
	x[0] = (uint8_t)((FOX_INTERVAL_MAX_S + 1u) & 0xFF);	// over the longest
	x[1] = (uint8_t)((FOX_INTERVAL_MAX_S + 1u) >> 8);
	CHECK(!fox_rec_decode(x, FOX_REC_LEN, &d));
	x[0] = (uint8_t)(FOX_INTERVAL_MAX_S & 0xFF);
	x[1] = (uint8_t)(FOX_INTERVAL_MAX_S >> 8);
	CHECK(fox_rec_decode(x, FOX_REC_LEN, &d) && d.interval_s == FOX_INTERVAL_MAX_S);
	memcpy(x, b, sizeof x);
	x[3] = FOX_REPEATS_MAX + 1u;	// more repeats than it plays
	CHECK(!fox_rec_decode(x, FOX_REC_LEN, &d));
	x[3] = 0;	// 0 reads as once
	CHECK(fox_rec_decode(x, FOX_REC_LEN, &d) && d.repeats == 1);
	memcpy(x, b, sizeof x);
	x[2] = 0x04;	// a flag it doesn't know
	CHECK(!fox_rec_decode(x, FOX_REC_LEN, &d));
	memcpy(x, b, sizeof x);
	x[4 + 3] = '~';	// no Morse for it
	CHECK(!fox_rec_decode(x, FOX_REC_LEN, &d));
	memcpy(x, b, sizeof x);
	x[4 + 3] = 0;	// text after the padding
	CHECK(!fox_rec_decode(x, FOX_REC_LEN, &d));
	memcpy(x, b, sizeof x);
	x[4 + FOX_MSG_MAX] = 0x47;	// step 1 at 327 Hz: off the 10 Hz grid
	x[4 + FOX_MSG_MAX + 1] = 0x01;
	CHECK(!fox_rec_decode(x, FOX_REC_LEN, &d));
	memcpy(x, b, sizeof x);
	x[4 + FOX_MSG_MAX] = (uint8_t)((SIG_HZ_MAX + 10) & 0xFF);	// over the limit
	x[4 + FOX_MSG_MAX + 1] = (uint8_t)((SIG_HZ_MAX + 10) >> 8);
	CHECK(!fox_rec_decode(x, FOX_REC_LEN, &d));
	// lower case letters have Morse: they are taken as they are (the keyer sends either case)
	memcpy(x, b, sizeof x);
	x[4] = 'f';
	CHECK(fox_rec_decode(x, FOX_REC_LEN, &d) && d.msg[0] == 'f');

	CHECK(fox_msg_char_ok(' ') && fox_msg_char_ok('A') && fox_msg_char_ok('7') && fox_msg_char_ok('/'));
	CHECK(!fox_msg_char_ok('~') && !fox_msg_char_ok('\0'));
}

static uint32_t seg_ms(const beep_seg_t *s, uint8_t n)
{
	uint32_t t = 0;
	for (uint8_t i = 0; i < n; i++) {
		t += s[i].ms;
	}
	return t;
}

static void test_jingle(void)
{
	fox_rec_t r;
	beep_seg_t s[FOX_STEPS];

	// no step: the default tune, on the grid and within the limits
	fox_rec_default(&r);
	uint8_t n = fox_jingle(&r, s);
	CHECK(n >= 2 && n <= FOX_STEPS);
	for (uint8_t i = 0; i < n; i++) {
		CHECK(s[i].hz % SIG_HZ_UNIT == 0 && s[i].hz <= SIG_HZ_MAX && s[i].ms != 0);
	}
	CHECK(seg_ms(s, n) <= FOX_MAX_MS);

	// the record's steps up to the first of length 0
	r.hz[0] = 1000;
	r.ms10[0] = 50;
	r.hz[1] = 0;
	r.ms10[1] = 10;
	r.hz[2] = 2000;
	r.ms10[2] = 0;
	r.hz[3] = 3000;
	r.ms10[3] = 20;
	n = fox_jingle(&r, s);
	CHECK(n == 2 && s[0].hz == 1000 && s[0].ms == 500 && s[1].hz == 0 && s[1].ms == 100);

	// longer than one signaling tone may be (SIG_MAX_MS), cut at FOX_MAX_MS
	for (unsigned i = 0; i < FOX_STEPS; i++) {
		r.hz[i] = 800;
		r.ms10[i] = 255;	// 2.55 s each: 81.6 s in all
	}
	n = fox_jingle(&r, s);
	CHECK(seg_ms(s, n) == FOX_MAX_MS && n == 4 && s[3].ms == FOX_MAX_MS - 3u * 2550u);
	CHECK(seg_ms(s, n) > SIG_MAX_MS);

	// sig_chain_n alone: no standard sound for an empty chain
	uint16_t hz[3] = { 500, 600, 700 };
	uint8_t ms[3] = { 0, 10, 10 };
	CHECK(sig_chain_n(hz, ms, 3, FOX_MAX_MS, s) == 0);
	ms[0] = 10;
	CHECK(sig_chain_n(hz, ms, 3, 250, s) == 3 && s[2].ms == 50 && seg_ms(s, 3) == 250);
	CHECK(sig_chain_n(hz, ms, 2, FOX_MAX_MS, s) == 2);
}

static void test_cycle(void)
{
	fox_rec_t r;
	beep_seg_t s[FOX_STEPS];
	const char *call = "N0CALL";
	uint32_t dot = morse_dot_ms(20);

	fox_rec_default(&r);
	uint8_t n = fox_jingle(&r, s);
	uint32_t want = FOX_LEAD_MS + morse_units("FOX", 3) * dot + FOX_GAP_MS + seg_ms(s, n) + FOX_GAP_MS +
			morse_units(call, 6) * dot + FOX_TAIL_MS;
	CHECK(fox_cycle_ms(&r, call, 6, 20) == want);

	// the ID alone: no gap before it
	r.flags = 0;
	CHECK(fox_cycle_ms(&r, call, 6, 20) == FOX_LEAD_MS + morse_units(call, 6) * dot + FOX_TAIL_MS);
	// the message on but empty: nothing for it, and no gap
	r.flags = FOX_F_MSG;
	memset(r.msg, 0, sizeof r.msg);
	CHECK(fox_cycle_ms(&r, call, 6, 20) == FOX_LEAD_MS + morse_units(call, 6) * dot + FOX_TAIL_MS);
	// the jingle alone: one gap, before the ID
	r.flags = FOX_F_JINGLE;
	CHECK(fox_cycle_ms(&r, call, 6, 20) == FOX_LEAD_MS + seg_ms(s, n) + FOX_GAP_MS + morse_units(call, 6) * dot +
	      FOX_TAIL_MS);
	// the jingle three times: two more plays, a gap before each
	r.repeats = 3;
	CHECK(fox_cycle_ms(&r, call, 6, 20) == FOX_LEAD_MS + 3u * seg_ms(s, n) + 3u * FOX_GAP_MS +
	      morse_units(call, 6) * dot + FOX_TAIL_MS);
	r.repeats = 1;
	// slower Morse takes longer
	CHECK(fox_cycle_ms(&r, call, 6, 5) > fox_cycle_ms(&r, call, 6, 20));
}

// Run the schedule from now until it asks for something; parts end at once when done says so
static fox_act_t step(radio_fox_t *f, bool done, uint32_t *now)
{
	for (int i = 0; i < 100000; i++) {
		fox_act_t a = radio_fox_poll(f, done, *now);
		if (a != FOX_ACT_NONE) {
			return a;
		}
		*now += 10;
	}
	return FOX_ACT_NONE;
}

static void test_schedule(uint32_t t0)
{
	radio_fox_t f;
	uint32_t now = t0;

	radio_fox_start(&f, FOX_F_MSG | FOX_F_JINGLE, 1, 60000u, now);
	CHECK(f.phase == FOX_WAIT && !radio_fox_on_air(&f) && radio_fox_wait_left(&f, now) == 0);
	CHECK(radio_fox_poll(&f, false, now) == FOX_ACT_KEY);	// the first cycle at once
	CHECK(radio_fox_poll(&f, false, now) == FOX_ACT_KEY);	// until the radio core says how the key-up went
	radio_fox_keyed(&f, now);
	CHECK(f.phase == FOX_LEAD && radio_fox_on_air(&f));
	// the lead-in, then the message
	CHECK(radio_fox_poll(&f, true, now + FOX_LEAD_MS - 1) == FOX_ACT_NONE);	// timed: done means nothing here
	now += FOX_LEAD_MS;
	CHECK(radio_fox_poll(&f, false, now) == FOX_ACT_MSG && f.phase == FOX_MSG);
	// a part lasts until the radio core says it is done, however long
	CHECK(radio_fox_poll(&f, false, now + 3600000u) == FOX_ACT_NONE);
	now += 2000;
	CHECK(radio_fox_poll(&f, true, now) == FOX_ACT_QUIET && f.phase == FOX_GAP_JINGLE);
	CHECK(radio_fox_poll(&f, false, now + FOX_GAP_MS - 1) == FOX_ACT_NONE);
	now += FOX_GAP_MS;
	CHECK(radio_fox_poll(&f, false, now) == FOX_ACT_JINGLE);
	now += 1500;
	CHECK(radio_fox_poll(&f, true, now) == FOX_ACT_QUIET && f.phase == FOX_GAP_ID);
	now += FOX_GAP_MS;
	CHECK(radio_fox_poll(&f, false, now) == FOX_ACT_ID);
	now += 3000;
	CHECK(radio_fox_poll(&f, true, now) == FOX_ACT_QUIET && f.phase == FOX_TAIL);
	CHECK(radio_fox_poll(&f, false, now + FOX_TAIL_MS - 1) == FOX_ACT_NONE);
	now += FOX_TAIL_MS;
	CHECK(f.cycles == 0);
	CHECK(radio_fox_poll(&f, false, now) == FOX_ACT_UNKEY);
	CHECK(f.phase == FOX_WAIT && f.cycles == 1 && f.cut == 0 && !radio_fox_on_air(&f));
	// the wait is an interval from the end of the cycle
	CHECK(radio_fox_wait_left(&f, now) == 60000u && radio_fox_wait_left(&f, now + 59000u) == 1000u);
	CHECK(radio_fox_poll(&f, false, now + 59999u) == FOX_ACT_NONE);
	now += 60000u;
	CHECK(radio_fox_poll(&f, false, now) == FOX_ACT_KEY);

	// busy at key-up: due again a few seconds later, nothing counted
	radio_fox_retry(&f, now);
	CHECK(f.phase == FOX_WAIT && f.cut == 0 && radio_fox_wait_left(&f, now) == FOX_BUSY_RETRY_MS);
	now += FOX_BUSY_RETRY_MS;
	CHECK(radio_fox_poll(&f, false, now) == FOX_ACT_KEY);
	// keyed, then cut in the middle: counted, the next one an interval later
	radio_fox_keyed(&f, now);
	now += FOX_LEAD_MS;
	CHECK(radio_fox_poll(&f, false, now) == FOX_ACT_MSG);
	radio_fox_cut(&f, now);
	CHECK(f.phase == FOX_WAIT && f.cut == 1 && f.cycles == 1 && radio_fox_wait_left(&f, now) == 60000u);
	// keyed is ignored outside a due key-up
	radio_fox_keyed(&f, now);
	CHECK(f.phase == FOX_WAIT);

	// stopped: nothing more, the counts kept
	radio_fox_stop(&f);
	CHECK(radio_fox_poll(&f, true, now + 120000u) == FOX_ACT_NONE && f.cycles == 1 && f.cut == 1);
	radio_fox_retry(&f, now);
	radio_fox_cut(&f, now);
	CHECK(f.phase == FOX_OFF && f.cut == 1);
}

// The parts a run sends: the ID alone, the jingle alone (one gap, before the ID), the message alone
static void test_parts(void)
{
	radio_fox_t f;
	uint32_t now = 500;

	radio_fox_start(&f, 0, 1, 30000u, now);
	CHECK(step(&f, true, &now) == FOX_ACT_KEY);
	radio_fox_keyed(&f, now);
	CHECK(step(&f, true, &now) == FOX_ACT_ID);
	CHECK(step(&f, true, &now) == FOX_ACT_QUIET && f.phase == FOX_TAIL);
	CHECK(step(&f, true, &now) == FOX_ACT_UNKEY);

	radio_fox_start(&f, FOX_F_JINGLE | 0x80u, 1, 30000u, now);
	CHECK(f.parts == FOX_F_JINGLE);	// an unknown part is dropped
	CHECK(step(&f, true, &now) == FOX_ACT_KEY);
	radio_fox_keyed(&f, now);
	CHECK(step(&f, true, &now) == FOX_ACT_JINGLE);
	CHECK(step(&f, true, &now) == FOX_ACT_QUIET && f.phase == FOX_GAP_ID);
	CHECK(step(&f, true, &now) == FOX_ACT_ID);
	CHECK(step(&f, true, &now) == FOX_ACT_QUIET && f.phase == FOX_TAIL);
	CHECK(step(&f, true, &now) == FOX_ACT_UNKEY && f.cycles == 1);

	radio_fox_start(&f, FOX_F_MSG, 1, 30000u, now);
	CHECK(f.cycles == 0);	// a new run counts from 0
	CHECK(step(&f, true, &now) == FOX_ACT_KEY);
	radio_fox_keyed(&f, now);
	CHECK(step(&f, true, &now) == FOX_ACT_MSG);
	CHECK(step(&f, true, &now) == FOX_ACT_QUIET && f.phase == FOX_GAP_ID);
	CHECK(step(&f, true, &now) == FOX_ACT_ID);
	CHECK(step(&f, true, &now) == FOX_ACT_QUIET);
	CHECK(step(&f, true, &now) == FOX_ACT_UNKEY);

	// not started: nothing
	memset(&f, 0, sizeof f);
	CHECK(f.phase == FOX_OFF && radio_fox_poll(&f, true, 1000) == FOX_ACT_NONE && radio_fox_wait_left(&f, 0) == 0);
}

// The jingle's repeats: each play after a gap, then the ID; counted again in every cycle; a count out of range taken
// as the nearest
static void test_repeats(void)
{
	radio_fox_t f;
	uint32_t now = 100;

	radio_fox_start(&f, FOX_F_MSG | FOX_F_JINGLE, 3, FOX_INTERVAL_MIN_S * 1000u, now);
	for (int cycle = 0; cycle < 2; cycle++) {
		CHECK(step(&f, true, &now) == FOX_ACT_KEY);
		radio_fox_keyed(&f, now);
		CHECK(step(&f, true, &now) == FOX_ACT_MSG);
		for (int k = 0; k < 3; k++) {
			CHECK(step(&f, true, &now) == FOX_ACT_QUIET && f.phase == FOX_GAP_JINGLE);
			uint32_t gap = now;
			CHECK(step(&f, true, &now) == FOX_ACT_JINGLE && now - gap == FOX_GAP_MS && f.played == k + 1);
		}
		CHECK(step(&f, true, &now) == FOX_ACT_QUIET && f.phase == FOX_GAP_ID);
		CHECK(step(&f, true, &now) == FOX_ACT_ID);
		CHECK(step(&f, true, &now) == FOX_ACT_QUIET && f.phase == FOX_TAIL);
		CHECK(step(&f, true, &now) == FOX_ACT_UNKEY && f.cycles == (uint32_t)cycle + 1u);
		CHECK(radio_fox_wait_left(&f, now) == FOX_INTERVAL_MIN_S * 1000u);
	}
	// a cut cycle starts the next one's count over
	CHECK(step(&f, true, &now) == FOX_ACT_KEY);
	radio_fox_keyed(&f, now);
	CHECK(step(&f, true, &now) == FOX_ACT_MSG);
	CHECK(step(&f, true, &now) == FOX_ACT_QUIET);
	CHECK(step(&f, true, &now) == FOX_ACT_JINGLE && f.played == 1);
	radio_fox_cut(&f, now);
	CHECK(step(&f, true, &now) == FOX_ACT_KEY);
	radio_fox_keyed(&f, now);
	CHECK(f.played == 0);

	// the jingle alone, twice: no gap before the first play
	radio_fox_start(&f, FOX_F_JINGLE, 2, 30000u, now);
	CHECK(step(&f, true, &now) == FOX_ACT_KEY);
	radio_fox_keyed(&f, now);
	CHECK(step(&f, true, &now) == FOX_ACT_JINGLE);
	CHECK(step(&f, true, &now) == FOX_ACT_QUIET && f.phase == FOX_GAP_JINGLE);
	CHECK(step(&f, true, &now) == FOX_ACT_JINGLE);
	CHECK(step(&f, true, &now) == FOX_ACT_QUIET && f.phase == FOX_GAP_ID);

	radio_fox_start(&f, FOX_F_JINGLE, 0, 30000u, now);
	CHECK(f.repeats == 1);
	radio_fox_start(&f, FOX_F_JINGLE, 200, 30000u, now);
	CHECK(f.repeats == FOX_REPEATS_MAX);
}

int main(void)
{
	test_rec();
	test_repeats();
	test_jingle();
	test_cycle();
	test_schedule(1000);
	test_schedule(0xFFFFF000u);	// the ms timer wraps in the middle of a cycle and of the wait
	test_parts();
	printf("%d checks, %d failed\n", checks, failures);
	printf(failures ? "# FAILED foxTest: %d\n" : "# PASSED foxTest\n", failures);
	return failures != 0;
}
