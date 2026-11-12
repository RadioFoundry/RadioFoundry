// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The ring-to-block rate converter (drivers/audio/resample): silence until primed, DC and a tone through at the right
// frequency, no slips at the nominal rate, rare slips at a drifting one, recovery from a stalled reader
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include "audio/resample/resample.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define CAP	1024u
#define BLOCK	160u
#define BURST	32u	// the producer writes this many at a time
#define TARGET	256u
#define IN_HZ	9146u
#define OUT_HZ	8000u

static int16_t ring[CAP];
static uint32_t head;
static double t_in;	// input time of the next sample, s

typedef double (*wave_t)(double t);

static double dc(double t)
{
	(void)t;
	return 1000.0;
}

static double tone(double t)
{
	return 10000.0 * sin(2.0 * M_PI * 1000.0 * t);
}

// The producer for one output block's time: whole bursts at in_hz (fractional samples carried)
static double owed;
static void produce(wave_t w, double in_hz)
{
	owed += in_hz * (double)BLOCK / (double)OUT_HZ;
	while (owed >= BURST) {
		for (uint32_t k = 0; k < BURST; k++) {
			ring[head % CAP] = (int16_t)lrint(w(t_in));
			head++;
			t_in += 1.0 / in_hz;
		}
		owed -= BURST;
	}
}

static void reset(void)
{
	head = 0;
	t_in = 0;
	owed = 0;
	for (uint32_t i = 0; i < CAP; i++) {
		ring[i] = 0;
	}
}

static void test_silence_then_dc(void)
{
	reset();
	resample_t r;
	resample_init(&r, IN_HZ, OUT_HZ, head);
	int16_t out[BLOCK];
	produce(dc, IN_HZ);	// 183 samples rounded down to bursts: under the target
	resample_block(&r, ring, CAP, head, out, BLOCK, TARGET);
	CHECK(!r.primed && out[0] == 0 && out[BLOCK - 1] == 0);
	for (int b = 0; b < 20; b++) {
		produce(dc, IN_HZ);
		resample_block(&r, ring, CAP, head, out, BLOCK, TARGET);
	}
	CHECK(r.primed);
	int ok = 1;
	for (uint32_t i = 0; i < BLOCK; i++) {
		ok &= out[i] == 1000;
	}
	CHECK(ok);
	CHECK(r.slips == 0 && r.holds == 0);
}

// Zero crossings of the output over many blocks give the tone's frequency; the peak stays near the input's
static void test_tone(double in_hz, uint32_t blocks, uint32_t max_slips)
{
	reset();
	resample_t r;
	resample_init(&r, IN_HZ, OUT_HZ, head);
	int16_t out[BLOCK];
	for (int b = 0; b < 4; b++) {	// prime
		produce(tone, in_hz);
		resample_block(&r, ring, CAP, head, out, BLOCK, TARGET);
	}
	uint32_t slips0 = r.slips;
	uint32_t crossings = 0;
	int16_t last = out[BLOCK - 1], peak = 0;
	for (uint32_t b = 0; b < blocks; b++) {
		produce(tone, in_hz);
		resample_block(&r, ring, CAP, head, out, BLOCK, TARGET);
		for (uint32_t i = 0; i < BLOCK; i++) {
			if ((last < 0) != (out[i] < 0)) {
				crossings++;
			}
			last = out[i];
			peak = out[i] > peak ? out[i] : peak;
		}
	}
	double secs = (double)blocks * BLOCK / OUT_HZ;
	double hz = (double)crossings / 2.0 / secs;
	printf("  in %.1f Hz: %.1f Hz out over %.0f s, peak %d, slips %u, holds %u\n", in_hz, hz, secs, peak,
	       r.slips - slips0, r.holds);
	// Input samples arrive faster or slower than the converter assumes, so the tone comes out shifted the other way
	CHECK(fabs(hz - 1000.0 * IN_HZ / in_hz) < 3.0);
	CHECK(peak > 9700 && peak <= 10000);
	CHECK(r.slips - slips0 <= max_slips);
}

// A reader that stops for a while (the ring wraps) comes back with one slip and a clean tone
static void test_stall(void)
{
	reset();
	resample_t r;
	resample_init(&r, IN_HZ, OUT_HZ, head);
	int16_t out[BLOCK];
	for (int b = 0; b < 10; b++) {
		produce(tone, IN_HZ);
		resample_block(&r, ring, CAP, head, out, BLOCK, TARGET);
	}
	CHECK(r.slips == 0);
	for (int b = 0; b < 12; b++) {
		produce(tone, IN_HZ);	// nobody reads: more than CAP written
	}
	produce(tone, IN_HZ);
	resample_block(&r, ring, CAP, head, out, BLOCK, TARGET);
	CHECK(r.slips == 1);
	CHECK(r.holds == 0);
	// The tail is target behind head again: a sample read now is the input from target samples ago
	CHECK(head - r.tail <= TARGET);
}

// The producer stops: once the ring runs dry the blocks are silence, not the ring's last ms replayed
static void test_dry(void)
{
	reset();
	resample_t r;
	resample_init(&r, IN_HZ, OUT_HZ, head);
	int16_t out[BLOCK];
	for (int b = 0; b < 10; b++) {
		produce(dc, IN_HZ);
		resample_block(&r, ring, CAP, head, out, BLOCK, TARGET);
	}
	uint32_t slips0 = r.slips;
	for (int b = 0; b < 3; b++) {
		resample_block(&r, ring, CAP, head, out, BLOCK, TARGET);
	}
	CHECK(r.slips == slips0);
	CHECK(out[0] == 0 && out[BLOCK - 1] == 0);
	// It starts again: the tail jumps to target behind the new head and DC comes through
	for (int b = 0; b < 3; b++) {
		produce(dc, IN_HZ);
		resample_block(&r, ring, CAP, head, out, BLOCK, TARGET);
	}
	CHECK(out[BLOCK - 1] == 1000);
}

int main(void)
{
	test_silence_then_dc();
	test_tone(IN_HZ, 500, 0);		// 10 s at the nominal rate: no slip
	test_tone(IN_HZ * 1.0001, 3000, 2);	// 100 ppm fast for 60 s: 55 samples over, within the window
	test_tone(IN_HZ * 0.9999, 3000, 2);	// 100 ppm slow
	test_tone(IN_HZ * 1.01, 500, 5);	// 1 % fast: a slip every 2.8 s (256 samples over)
	test_stall();
	test_dry();
	if (failures) {
		printf("resampleTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("resampleTest: all passed\n");
	return 0;
}
