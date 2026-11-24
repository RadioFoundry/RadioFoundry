// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The mic gain and peak limiter before a vocoder's encoder (drivers/audio/miclimit): a quiet voice lifted by the set
// gain with nothing changed in its shape, a loud one held under the ceiling, the recovery's pace, and the dB table
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "audio/miclimit/miclimit.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define N	160	// one 20 ms block at 8 kHz, as the audio callbacks deliver

static uint32_t phase;

// A 400 Hz tone block at an rms level in dBFS (full scale 32768)
static void tone(int16_t *b, double dbfs)
{
	double a = 32768.0 * pow(10.0, dbfs / 20.0) * sqrt(2.0);
	for (int i = 0; i < N; i++, phase++) {
		double v = a * sin(2.0 * M_PI * 400.0 * phase / 8000.0);
		b[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
}

static double rms_dbfs(const int16_t *b)
{
	double s = 0;
	for (int i = 0; i < N; i++) {
		s += (double)b[i] * b[i];
	}
	return 10.0 * log10(s / N / (32768.0 * 32768.0) + 1e-20);
}

static int peak(const int16_t *b)
{
	int p = 0;
	for (int i = 0; i < N; i++) {
		int a = b[i] < 0 ? -b[i] : b[i];
		p = a > p ? a : p;
	}
	return p;
}

static void test_table(void)
{
	CHECK(miclimit_db_q8(0) == 256, "0 dB is unity");
	for (int db = MICLIMIT_DB_MIN; db <= MICLIMIT_DB_MAX; db++) {
		double want = 256.0 * pow(10.0, db / 20.0);
		CHECK(fabs(miclimit_db_q8(db) - want) <= 0.5, "%d dB: %u, want %.1f", db, miclimit_db_q8(db), want);
		CHECK(miclimit_q8_db(miclimit_db_q8(db)) == db, "%d dB back: %d", db, miclimit_q8_db(miclimit_db_q8(db)));
	}
	CHECK(miclimit_db_q8(-40) == miclimit_db_q8(MICLIMIT_DB_MIN) && miclimit_db_q8(99) == miclimit_db_q8(MICLIMIT_DB_MAX),
	      "clamped");
}

// One radio's measured case (19 Nov): speech at -31 dBFS and +12 dB gets to -19 dBFS, the limiter never touching it
static void test_quiet_lifted(void)
{
	miclimit_t l;
	uint16_t g = miclimit_db_q8(12);
	miclimit_reset(&l, g);
	phase = 0;
	int16_t in[N], out[N];
	for (int b = 0; b < 50; b++) {
		tone(in, -31.0);
		miclimit_block(&l, g, in, out, N);
		double d = rms_dbfs(out) - rms_dbfs(in);
		CHECK(fabs(d - 12.0) < 0.1, "block %d: lifted %.2f dB", b, d);
	}
	CHECK(fabs(rms_dbfs(out) - -19.0) < 0.2, "level %.2f dBFS", rms_dbfs(out));
	CHECK(l.limited == 0 && l.low_q8 == g && l.gain_q8 == g, "limiter idle: %u blocks, low %u", (unsigned)l.limited,
	      l.low_q8);
	CHECK(l.peak_in > 0 && l.peak_out > l.peak_in * 3, "peaks in %u out %u", l.peak_in, l.peak_out);
}

// A shout at full scale with +12 dB set: no sample past the ceiling, ever, and the cut shows in low_q8
static void test_loud_held(void)
{
	miclimit_t l;
	uint16_t g = miclimit_db_q8(12);
	miclimit_reset(&l, g);
	phase = 0;
	int16_t in[N], out[N];
	for (int b = 0; b < 30; b++) {
		tone(in, b < 10 ? -12.0 : -3.0);	// -12 dBFS + 12 would clip; -3 dBFS far more
		miclimit_block(&l, g, in, out, N);
		CHECK(peak(out) <= MICLIMIT_CEILING, "block %d: peak %d", b, peak(out));
	}
	CHECK(l.peak_out <= MICLIMIT_CEILING && l.peak_out > MICLIMIT_CEILING - 400, "peak out %u", l.peak_out);
	CHECK(l.limited == 30 && miclimit_q8_db(l.low_q8) <= 1, "limited %u, lowest %d dB", (unsigned)l.limited,
	      miclimit_q8_db(l.low_q8));
	// Clipping, which the plain multiply did, would have flattened the tops: the limiter's output is still a sine
	double crest = 20.0 * log10(peak(out) / 32768.0) - rms_dbfs(out);
	CHECK(fabs(crest - 3.01) < 0.1, "crest factor %.2f dB, a sine's 3.01", crest);
}

// After a loud block the gain climbs back no faster than MICLIMIT_RELEASE_Q8 a block, without a step inside a block,
// and reaches the set gain again
static void test_recovery(void)
{
	miclimit_t l;
	uint16_t g = miclimit_db_q8(12);
	miclimit_reset(&l, g);
	phase = 0;
	int16_t in[N], out[N];
	tone(in, -3.0);
	uint16_t low = miclimit_block(&l, g, in, out, N);
	CHECK(low < miclimit_db_q8(2), "cut to %u", low);
	uint16_t prev = low;
	int blocks = 0;
	while (l.gain_q8 < g && blocks < 100) {
		tone(in, -40.0);
		uint16_t now = miclimit_block(&l, g, in, out, N);
		CHECK(now >= prev && now <= (prev * MICLIMIT_RELEASE_Q8) / 256u + 1u, "block %d: %u after %u", blocks, now, prev);
		prev = now;
		blocks++;
	}
	CHECK(l.gain_q8 == g && blocks >= 15 && blocks <= 30, "back to the set gain in %d blocks", blocks);

	// Inside a rising block the samples' gain ramps: a constant input comes out as a rising staircase whose steps are
	// one Q8 gain step (1000 / 256, about 0.03 dB), not one jump at the block's start
	miclimit_reset(&l, g);
	l.gain_q8 = 512;
	int16_t dc[N];
	for (int i = 0; i < N; i++) {
		dc[i] = 1000;
	}
	miclimit_block(&l, g, dc, out, N);
	int jumps = 0;
	for (int i = 1; i < N; i++) {
		jumps += out[i] < out[i - 1] || out[i] - out[i - 1] > 4;
	}
	CHECK(jumps == 0 && out[0] >= 1999 && out[N - 1] == (int16_t)(1000 * l.gain_q8 / 256), "ramp %d..%d, %d jumps",
	      out[0], out[N - 1], jumps);
}

// Unity and under the ceiling: the samples pass unchanged (in place, as the callers may call it)
static void test_unity(void)
{
	miclimit_t l;
	miclimit_reset(&l, MICLIMIT_UNITY);
	phase = 0;
	int16_t in[N], copy[N];
	tone(in, -10.0);
	memcpy(copy, in, sizeof in);
	miclimit_block(&l, MICLIMIT_UNITY, in, in, N);
	CHECK(memcmp(in, copy, sizeof in) == 0 && l.limited == 0, "unity changed the block");
}

// The setting turned down mid-transmission takes effect in the next block, not over the release time
static void test_set_lowered(void)
{
	miclimit_t l;
	miclimit_reset(&l, miclimit_db_q8(12));
	phase = 0;
	int16_t in[N], out[N];
	tone(in, -40.0);
	miclimit_block(&l, miclimit_db_q8(12), in, out, N);
	tone(in, -40.0);
	uint16_t g = miclimit_block(&l, miclimit_db_q8(2), in, out, N);
	CHECK(g == miclimit_db_q8(2) && fabs(rms_dbfs(out) - rms_dbfs(in) - 2.0) < 0.15, "gain %u", g);
}

int main(void)
{
	test_table();
	test_quiet_lifted();
	test_loud_held();
	test_recovery();
	test_unity();
	test_set_lowered();
	if (failures) {
		printf("miclimitTest: %d failures\n", failures);
		return 1;
	}
	printf("miclimitTest: all passed\n");
	return 0;
}
