// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <stdio.h>
#include <string.h>
#include "radio/spectrum.h"

static unsigned failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

static void test_key_assignments(void)
{
	spectrum_t s;
	CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, 12500, 129, 0));
	CHECK(s.hash_key == SPECTRUM_KEY_BANDWIDTH && s.star_key == SPECTRUM_KEY_CENTER_TUNER);
	CHECK(spectrum_sample(&s, -55, 10) && s.audio_open);
	for (unsigned action = 0; action < SPECTRUM_KEY_COUNT; action++) {
		CHECK(spectrum_adjust(&s, SPECTRUM_HASH_KEY, (int32_t)action, 20));
		CHECK(spectrum_adjust(&s, SPECTRUM_STAR_KEY, (int32_t)action, 20));
		CHECK(s.hash_key == action && s.star_key == action);
		CHECK(s.audio_open && s.listening && s.due_ms == 20 && spectrum_receiver_hz(&s) == 446000000);
		CHECK(spectrum_level_dbm(spectrum_peak_level(&s, 64)) == -55);
	}
	spectrum_t before = s;
	CHECK(!spectrum_adjust(&s, SPECTRUM_HASH_KEY, SPECTRUM_KEY_COUNT, 20));
	CHECK(!spectrum_adjust(&s, SPECTRUM_STAR_KEY, -1, 20));
	CHECK(memcmp(&s, &before, sizeof s) == 0);
	CHECK(spectrum_adjust(&s, SPECTRUM_HASH_KEY, SPECTRUM_KEY_FILTER, 20));
	CHECK(spectrum_adjust(&s, SPECTRUM_STAR_KEY, SPECTRUM_KEY_AUTO_PEAK, 20));
	CHECK(spectrum_adjust(&s, SPECTRUM_STEP, 6250, 30));
	CHECK(spectrum_adjust(&s, SPECTRUM_BANDWIDTH, 97, 40));
	CHECK(spectrum_adjust(&s, SPECTRUM_FILTER, 1, 50));
	CHECK(spectrum_recenter(&s, 145062500, 136000000, 174000000, 60));
	CHECK(s.hash_key == SPECTRUM_KEY_FILTER && s.star_key == SPECTRUM_KEY_AUTO_PEAK);
}

static void test_edge_pan(void)
{
	static const unsigned counts[] = { 65, 97, 129 };
	static const uint32_t steps[] = { 6250, 12500 };
	for (unsigned n = 0; n < 3; n++) {
		for (unsigned k = 0; k < 2; k++) {
			for (int dir = -1; dir <= 1; dir += 2) {
				spectrum_t s;
				CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, steps[k], counts[n], 0));
				for (unsigned i = 0; i < s.bins / 2u; i++) CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, dir, 0));
				uint8_t edge = dir < 0 ? 0 : s.bins - 1;
				CHECK(s.tuner == edge && s.center_hz == 446000000);
				uint32_t hz = spectrum_receiver_hz(&s);
				CHECK(spectrum_sample(&s, -55, 10) && s.audio_open);
				CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, dir, 20));
				uint32_t center = (uint32_t)(446000000 + dir * (int32_t)s.step_hz);
				CHECK(s.center_hz == center && s.tuner == edge && !s.center_tuner);
				CHECK(spectrum_receiver_hz(&s) == (uint32_t)((int64_t)hz + dir * (int32_t)s.step_hz));
				CHECK(s.listening && !s.audio_open && s.index == 0);
				CHECK(s.level[edge] == SPECTRUM_EMPTY && spectrum_peak_level(&s, edge) == SPECTRUM_EMPTY);
				CHECK(spectrum_level_dbm(s.level[edge - dir]) == -55);
				CHECK(spectrum_level_dbm(spectrum_peak_level(&s, edge - dir)) == -55);
				CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, -dir, 30));
				CHECK(s.center_hz == center && s.tuner == edge - dir && spectrum_receiver_hz(&s) == hz);
				spectrum_t before = s;
				CHECK(!spectrum_adjust(&s, SPECTRUM_TUNER, 0, 40) && memcmp(&s, &before, sizeof s) == 0);
				// A window at the receiver's band limit cannot scroll beyond it.
				CHECK(spectrum_start(&s, dir < 0 ? 400000000 : 480000000, 400000000, 480000000,
						     steps[k], counts[n], 0));
				for (unsigned i = 0; i < s.bins / 2u; i++) CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, dir, 0));
				before = s;
				CHECK(!spectrum_adjust(&s, SPECTRUM_TUNER, dir, 10) && memcmp(&s, &before, sizeof s) == 0);
			}
		}
	}
}

static void test_center_tuner(void)
{
	spectrum_t s;
	CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, 12500, 129, 0));
	CHECK(!s.center_tuner);
	CHECK(spectrum_sample(&s, -55, 10) && s.audio_open);
	CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, 1, 20));
	CHECK(spectrum_sample(&s, -60, 30) && s.audio_open);
	CHECK(spectrum_adjust(&s, SPECTRUM_CENTER_TUNER, 1, 40));
	CHECK(s.center_tuner && s.tuner == 64 && s.center_hz == 446012500 && s.audio_open);
	CHECK(spectrum_level_dbm(s.level[63]) == -55 && spectrum_level_dbm(s.level[64]) == -60);
	CHECK(spectrum_level_dbm(spectrum_peak_level(&s, 63)) == -55);
	CHECK(s.level[128] == SPECTRUM_EMPTY && spectrum_peak_level(&s, 128) == SPECTRUM_EMPTY);
	CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, -1, 50));
	CHECK(s.tuner == 64 && s.center_hz == 446000000 && !s.audio_open && s.listening);
	CHECK(spectrum_level_dbm(s.level[64]) == -55 && s.level[0] == SPECTRUM_EMPTY);
	CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, 3, 60));
	CHECK(s.center_hz == 446037500 && spectrum_receiver_hz(&s) == s.center_hz);
	CHECK(spectrum_adjust(&s, SPECTRUM_STEP, 6250, 70));
	CHECK(s.center_tuner && s.tuner == 64 && s.center_hz == 446037500);
	CHECK(spectrum_adjust(&s, SPECTRUM_BANDWIDTH, 65, 80));
	CHECK(s.center_tuner && s.tuner == 32);
	CHECK(spectrum_adjust(&s, SPECTRUM_FILTER, 1, 90));
	CHECK(s.center_tuner && s.tuner == 32 && s.narrow);
	CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, INT32_MAX, 100));
	CHECK(s.center_hz == 479800000 && spectrum_frequency(&s, 64) == 480000000 && s.tuner == 32);
	spectrum_t before = s;
	CHECK(!spectrum_adjust(&s, SPECTRUM_TUNER, 1, 110) && memcmp(&s, &before, sizeof s) == 0);
	CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, INT32_MIN, 120));
	CHECK(s.center_hz == 400200000 && spectrum_frequency(&s, 0) == 400000000 && s.tuner == 32);
	before = s;
	CHECK(!spectrum_adjust(&s, SPECTRUM_TUNER, -1, 130) && memcmp(&s, &before, sizeof s) == 0);
	CHECK(!spectrum_adjust(&s, SPECTRUM_CENTER_TUNER, 2, 130) && memcmp(&s, &before, sizeof s) == 0);
	CHECK(spectrum_adjust(&s, SPECTRUM_CENTER_TUNER, 0, 140) && !s.center_tuner);
	CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, -1, 150));
	CHECK(s.center_hz == 400200000 && s.tuner == 31 && spectrum_receiver_hz(&s) == 400193750);
	CHECK(spectrum_adjust(&s, SPECTRUM_CENTER_TUNER, 1, 160));
	CHECK(s.tuner == 32 && spectrum_receiver_hz(&s) == 400200000);

	// Automatic capture borrows the tuner in a fixed window; manual movement always starts at its center.
	for (unsigned mode = SPECTRUM_AUTO_SWEEP; mode <= SPECTRUM_AUTO_IMMEDIATE; mode++) {
		for (unsigned action = 0; action < 4; action++) {
			CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, 12500, 65, 0));
			CHECK(spectrum_adjust(&s, SPECTRUM_CENTER_TUNER, 1, 0));
			CHECK(spectrum_adjust(&s, SPECTRUM_AUTO_PEAK, (int32_t)mode, 0));
			CHECK(spectrum_sample(&s, -120, 10) && !s.listening);
			uint32_t now = 20;
			while (!s.listening && now < 1000) {
				CHECK(spectrum_sample(&s, s.index == 3 ? -55 : -120, now));
				now += 10;
			}
			unsigned resume = mode == SPECTRUM_AUTO_IMMEDIATE ? 4 : 0;
			CHECK(s.center_hz == 446000000 && s.tuner == 3 && s.index == resume && s.listening);
			CHECK(spectrum_receiver_hz(&s) == 445637500);
			CHECK(spectrum_level_dbm(spectrum_peak_level(&s, 3)) == -55);
			CHECK(s.audio_open == (mode == SPECTRUM_AUTO_IMMEDIATE));
			CHECK(spectrum_sample(&s, -55, now) && s.audio_open);
			if (action == 1 || action == 2) {
				int move = action == 1 ? -1 : 1;
				CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, move, now));
				CHECK(s.center_hz == (uint32_t)(446000000 + move * 12500) && s.tuner == 32);
				CHECK(spectrum_receiver_hz(&s) == s.center_hz && s.listening && !s.audio_open);
				CHECK(s.index == 0 && spectrum_level_dbm(spectrum_peak_level(&s, 3 - move)) == -55);
			} else {
				// Turning Auto peak off while listening must still return to the center after carrier loss.
				if (action == 3)
					CHECK(spectrum_adjust(&s, SPECTRUM_AUTO_PEAK, SPECTRUM_AUTO_OFF, now) && s.audio_open);
				CHECK(spectrum_sample(&s, -120, now + 10) && !s.audio_open && s.tuner == 3);
				CHECK(spectrum_sample(&s, -120, now + SPECTRUM_RELEASE_MS - 10) && s.tuner == 3);
				CHECK(spectrum_sample(&s, -120, now + SPECTRUM_RELEASE_MS) && s.tuner == 32 && s.listening);
				CHECK(s.center_hz == 446000000 && spectrum_receiver_hz(&s) == 446000000);
				CHECK(s.index == resume && !s.audio_open);
				CHECK(spectrum_sample(&s, -120, now + SPECTRUM_RELEASE_MS + 10) && !s.listening);
				CHECK(s.index == resume);
			}
		}
	}
	// A peak at the band edge is heard too. A blocked pan still cancels that peak and checks the center.
	CHECK(spectrum_start(&s, 400000000, 400000000, 480000000, 12500, 65, 0));
	CHECK(spectrum_adjust(&s, SPECTRUM_CENTER_TUNER, 1, 0));
	CHECK(spectrum_sample(&s, -120, 10));
	for (unsigned i = 0; i < 65; i++)
		CHECK(spectrum_sample(&s, i == 0 ? -20 : i == 40 ? -60 : -120, 20 + 10 * i));
	CHECK(s.center_hz == 400400000 && s.tuner == 0 && s.listening);
	CHECK(spectrum_sample(&s, -20, 670) && s.audio_open);
	CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, -1, 680));
	CHECK(s.center_hz == 400400000 && s.tuner == 32 && !s.audio_open && s.listening);
	CHECK(spectrum_receiver_hz(&s) == s.center_hz && spectrum_level_dbm(spectrum_peak_level(&s, 0)) == -20);
	before = s;
	CHECK(!spectrum_adjust(&s, SPECTRUM_TUNER, -1, 690) && memcmp(&s, &before, sizeof s) == 0);
	CHECK(spectrum_frequency(&s, 0) >= s.lo_hz && spectrum_frequency(&s, 64) <= s.hi_hz);
}

int main(void)
{
	test_key_assignments();
	test_edge_pan();
	test_center_tuner();
	_Static_assert(sizeof(spectrum_t) <= 824, "recent peaks must stay within their RAM budget");
	CHECK(spectrum_level_dbm(SPECTRUM_EMPTY) == INT16_MIN);
	CHECK(spectrum_level_dbm(0) == -160 && spectrum_level_dbm(160) == 0);
	spectrum_t s = { 0 };
	CHECK(spectrum_peak_level(&s, 0) == SPECTRUM_EMPTY);
	CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, 12500, SPECTRUM_BINS, 0));
	CHECK(spectrum_frequency(&s, 0) == 445200000);
	CHECK(spectrum_frequency(&s, SPECTRUM_BINS - 1) == 446800000);
	CHECK(spectrum_strongest(&s) == 0);
	// Unread bytes must never outrank real readings, including the receiver's endpoints.
	CHECK(spectrum_sample(&s, -160, 10));
	CHECK(spectrum_strongest(&s) == SPECTRUM_BINS / 2);
	CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, 12500, SPECTRUM_BINS, 0));
	CHECK(s.tuner == SPECTRUM_BINS / 2 && spectrum_receiver_hz(&s) == 446000000);
	CHECK(!spectrum_sample(&s, -120, 9));
	CHECK(spectrum_sample(&s, -120, 10) && !s.listening && s.index == 0);
	// A below-threshold peak is plotted while the quiet sweep continues.
	CHECK(spectrum_sample(&s, -90, 20) && !s.audio_open && s.index == 1);
	uint32_t now;
	for (now = 30; now <= 2000 && !s.sweeps; now += 10)
		CHECK(spectrum_sample(&s, -120, now));
	CHECK(s.sweeps == 1 && now <= 1600 && spectrum_strongest(&s) == 0);
	CHECK(spectrum_level_dbm(s.level[0]) == -90 && spectrum_level_dbm(spectrum_peak_level(&s, 0)) == -90);
	for (; now < 3200; now += 10)
		spectrum_sample(&s, -120, now);
	CHECK(spectrum_level_dbm(s.level[0]) == -120 && spectrum_level_dbm(spectrum_peak_level(&s, 0)) == -90);

	// Automatic tuning waits for the strongest bin of a full sweep and confirms the carrier before audio.
	CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, 12500, SPECTRUM_BINS, 0));
	CHECK(s.auto_peak == SPECTRUM_AUTO_SWEEP && s.dwell_ms == 10 && s.scale_db == 0);
	CHECK(!s.narrow);
	CHECK(s.squelch_dbm == -80 && spectrum_threshold_dbm(&s) == -80);
	CHECK(spectrum_sample(&s, -120, 10) && !s.listening);
	for (unsigned i = 0; i < s.bins; i++) {
		CHECK(s.index == i && !s.listening);
		CHECK(spectrum_sample(&s, i == 0 ? -65 : i == 1 ? -50 : -120, 20 + 10 * i));
	}
	CHECK(s.sweeps == 1 && s.tuner == 1 && s.index == 0 && s.listening && !s.audio_open);
	CHECK(spectrum_receiver_hz(&s) == 445212500);
	CHECK(spectrum_sample(&s, -55, 1310) && s.audio_open);
	CHECK(spectrum_sample(&s, -82, 1320) && s.audio_open);
	CHECK(spectrum_sample(&s, -84, 1330) && !s.audio_open && s.listening);
	CHECK(spectrum_sample(&s, -120, 1620) && !s.listening && s.index == 0);
	CHECK(spectrum_receiver_hz(&s) == 445200000);

	// Immediate mode holds the first settled bin at threshold and resumes at the following bin after release.
	CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, 12500, 65, 0));
	CHECK(spectrum_adjust(&s, SPECTRUM_AUTO_PEAK, SPECTRUM_AUTO_IMMEDIATE, 0));
	CHECK(spectrum_sample(&s, -120, 10) && !s.listening);
	CHECK(spectrum_sample(&s, -81, 20) && !s.audio_open && s.index == 1);
	CHECK(spectrum_sample(&s, -80, 30) && s.audio_open && s.listening && s.tuner == 1);
	CHECK(s.index == 2 && s.sweeps == 0 && spectrum_receiver_hz(&s) == 445612500);
	CHECK(spectrum_sample(&s, -82, 40) && s.audio_open);
	CHECK(spectrum_sample(&s, -84, 50) && !s.audio_open && s.listening);
	CHECK(spectrum_sample(&s, -120, 330) && s.listening);
	CHECK(spectrum_sample(&s, -120, 340) && !s.listening && s.index == 2);
	CHECK(spectrum_sample(&s, -65, 350) && s.audio_open && s.tuner == 2 && s.index == 3);
	// Capturing the final bin still counts the sweep and resumes at bin zero.
	s.listening = s.audio_open = false;
	s.index = s.bins - 1;
	CHECK(spectrum_sample(&s, -70, 360) && s.audio_open && s.tuner == 64 && s.sweeps == 1 && s.index == 0);

	// The threshold is a direct signed dBm value, including zero: no unsigned wrap or special open-at-zero.
	for (int threshold = -160; threshold <= 0; threshold++) {
		CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, 12500, SPECTRUM_BINS, 0));
		CHECK(spectrum_adjust(&s, SPECTRUM_SQUELCH, threshold, 0));
		CHECK(spectrum_threshold_dbm(&s) == threshold);
		CHECK(spectrum_sample(&s, -100, 10));
		CHECK(s.audio_open == (threshold <= -100));
	}
	CHECK(!spectrum_adjust(&s, SPECTRUM_SQUELCH, -161, 10));
	CHECK(!spectrum_adjust(&s, SPECTRUM_SQUELCH, 1, 10));
	CHECK(spectrum_adjust(&s, SPECTRUM_SQUELCH, 0, 20));
	CHECK(spectrum_sample(&s, -1, 30) && !s.audio_open);
	CHECK(spectrum_adjust(&s, SPECTRUM_SQUELCH, 0, 30));
	CHECK(spectrum_sample(&s, 0, 40) && s.audio_open);
	CHECK(spectrum_adjust(&s, SPECTRUM_SQUELCH, -160, 50));
	CHECK(spectrum_sample(&s, -160, 60) && s.audio_open);

	CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, 12500, SPECTRUM_BINS, 0));
	CHECK(spectrum_sample(&s, -65, 10) && s.listening && s.audio_open);
	for (now = 20; now <= 1000; now += 10)
		CHECK(spectrum_sample(&s, -65, now) && s.audio_open && s.index == 0);
	CHECK(!s.sweeps && spectrum_receiver_hz(&s) == 446000000);
	CHECK(spectrum_sample(&s, -81, 1010) && s.audio_open); // 3 dB hysteresis
	CHECK(spectrum_sample(&s, -84, 1020) && !s.audio_open && s.listening);
	CHECK(spectrum_sample(&s, -120, 1300) && s.listening);
	CHECK(spectrum_sample(&s, -120, 1310) && !s.listening);

	CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, -1, 1320));
	CHECK(s.tuner == 63 && s.center_hz == 446000000 && spectrum_receiver_hz(&s) == 445987500);
	CHECK(spectrum_adjust(&s, SPECTRUM_STEP, 6250, 1320));
	CHECK(s.step_hz == 6250 && spectrum_frequency(&s, s.tuner) == 445987500);
	CHECK(s.level[0] == SPECTRUM_EMPTY);
	CHECK(spectrum_adjust(&s, SPECTRUM_SQUELCH, -100, 1320));
	CHECK(spectrum_sample(&s, -95, 1330) && s.audio_open);
	CHECK(spectrum_adjust(&s, SPECTRUM_CENTER, 447000000, 1340));
	CHECK(s.center_hz == 447000000 && s.tuner == SPECTRUM_BINS / 2 && s.squelch_dbm == -100);
	CHECK(spectrum_adjust(&s, SPECTRUM_SQUELCH, -160, 1340));
	CHECK(spectrum_sample(&s, -160, 1350) && s.audio_open && s.listening);
	while (s.tuner > 0) spectrum_adjust(&s, SPECTRUM_TUNER, -1, 1360);
	CHECK(s.tuner == 0 && spectrum_adjust(&s, SPECTRUM_TUNER, -1, 1360));
	while (s.tuner < SPECTRUM_BINS - 1) spectrum_adjust(&s, SPECTRUM_TUNER, 1, 1360);
	CHECK(s.tuner == SPECTRUM_BINS - 1 && spectrum_adjust(&s, SPECTRUM_TUNER, 1, 1360));
	spectrum_t before = s;
	CHECK(!spectrum_adjust(&s, SPECTRUM_SQUELCH, 101, 1360));
	CHECK(!spectrum_adjust(&s, SPECTRUM_STEP, 6251, 1360));
	CHECK(!spectrum_adjust(&s, SPECTRUM_STEP, -1, 1360));
	CHECK(!spectrum_adjust(&s, SPECTRUM_CENTER, 200000000, 1360));
	CHECK(memcmp(&before, &s, sizeof s) == 0);

	CHECK(spectrum_start(&s, 400000000, 400000000, 480000000, 100000, SPECTRUM_BINS, UINT32_MAX - 5));
	CHECK(spectrum_frequency(&s, 0) == 400000000);
	CHECK(!spectrum_sample(&s, -120, 3));
	CHECK(spectrum_sample(&s, -120, 4));
	CHECK(spectrum_start(&s, 480000000, 400000000, 480000000, 100000, SPECTRUM_BINS, 0));
	CHECK(spectrum_frequency(&s, SPECTRUM_BINS - 1) == 480000000);
	before = s;
	CHECK(!spectrum_start(&s, 0, 400000000, 400010000, 12500, SPECTRUM_BINS, 0));
	CHECK(memcmp(&before, &s, sizeof s) == 0);
	CHECK(!spectrum_start(&s, 0, 0, UINT32_MAX, UINT32_MAX, SPECTRUM_BINS, 0));
	CHECK(spectrum_start(&s, UINT32_MAX, UINT32_MAX - 20000000, UINT32_MAX, 100000, SPECTRUM_BINS, 0));
	CHECK(spectrum_frequency(&s, SPECTRUM_BINS - 1) == UINT32_MAX);

	// All three window sizes keep the spacing fixed and preserve an in-range listening frequency.
	CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, 12500, 129, 0));
	CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, 1, 0));
	static const unsigned counts[] = { 65, 97, 129 };
	for (unsigned n = 0; n < 3; n++) {
		unsigned bins = counts[n];
		CHECK(spectrum_adjust(&s, SPECTRUM_BANDWIDTH, (int32_t)bins, 0));
		CHECK(s.bins == bins && s.step_hz == 12500 && spectrum_receiver_hz(&s) == 446012500);
		CHECK(spectrum_frequency(&s, bins - 1) - spectrum_frequency(&s, 0) == (bins - 1) * 12500);
		for (now = 10; now <= 1600 && !s.sweeps; now += 10) spectrum_sample(&s, -120, now);
		CHECK(s.sweeps == 1 && s.index < bins);
	}
	before = s;
	CHECK(!spectrum_adjust(&s, SPECTRUM_BANDWIDTH, 66, now));
	CHECK(memcmp(&before, &s, sizeof s) == 0);

	// A weaker recent reading survives expiry of an older maximum; unvisited peaks also expire while listening.
	CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, 12500, 129, 0));
	CHECK(spectrum_adjust(&s, SPECTRUM_SQUELCH, -160, 0));
	CHECK(spectrum_sample(&s, -40, 10));
	CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, 1, 20));
	CHECK(spectrum_sample(&s, -55, 30));
	CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, -1, 1980));
	CHECK(spectrum_sample(&s, -70, 2000));
	CHECK(spectrum_sample(&s, -100, 4000));
	CHECK(spectrum_level_dbm(spectrum_peak_level(&s, 64)) == -40);
	CHECK(spectrum_sample(&s, -100, 5000));
	CHECK(spectrum_level_dbm(spectrum_peak_level(&s, 64)) == -70);
	CHECK(spectrum_peak_level(&s, 65) == SPECTRUM_EMPTY && spectrum_strongest(&s) == 64);
	CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, 1, 6980));
	CHECK(spectrum_sample(&s, -95, 7000));
	CHECK(spectrum_level_dbm(spectrum_peak_level(&s, 64)) == -100 && spectrum_strongest(&s) == 65);
	CHECK(spectrum_sample(&s, -105, 20000));
	CHECK(spectrum_peak_level(&s, 64) == SPECTRUM_EMPTY);
	CHECK(spectrum_level_dbm(spectrum_peak_level(&s, 65)) == -105);

	CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, 12500, 129, UINT32_MAX - 500));
	CHECK(spectrum_sample(&s, -40, UINT32_MAX - 490));
	CHECK(spectrum_sample(&s, -70, 1499));
	CHECK(spectrum_level_dbm(spectrum_peak_level(&s, 64)) == -40);
	CHECK(spectrum_sample(&s, -100, 4499));
	CHECK(spectrum_level_dbm(spectrum_peak_level(&s, 64)) == -70);

	// Dwell controls scan bins; listening still checks squelch at the short radio tick.
	CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, 12500, 65, 0));
	CHECK(spectrum_adjust(&s, SPECTRUM_DWELL, 100, 0));
	CHECK(spectrum_sample(&s, -120, 10) && s.due_ms == 110);
	CHECK(!spectrum_sample(&s, -60, 109) && s.index == 0);
	CHECK(spectrum_sample(&s, -60, 110) && s.index == 1 && !s.listening && s.due_ms == 210);
	CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, 1, 120));
	CHECK(spectrum_sample(&s, -60, 130) && s.audio_open && s.due_ms == 140);
	CHECK(!spectrum_adjust(&s, SPECTRUM_DWELL, 9, 130));
	CHECK(!spectrum_adjust(&s, SPECTRUM_DWELL, 1001, 130));
	CHECK(spectrum_adjust(&s, SPECTRUM_AUTO_PEAK, 0, 130) && s.audio_open);
	CHECK(spectrum_adjust(&s, SPECTRUM_SCALE, -40, 130) && s.audio_open);
	CHECK(!spectrum_adjust(&s, SPECTRUM_SCALE, -80, 130));
	CHECK(!spectrum_adjust(&s, SPECTRUM_AUTO_PEAK, SPECTRUM_AUTO_COUNT, 130));
	CHECK(!spectrum_adjust(&s, SPECTRUM_AUTO_PEAK, -1, 130));
	CHECK(spectrum_adjust(&s, SPECTRUM_AUTO_PEAK, SPECTRUM_AUTO_IMMEDIATE, 130) && s.audio_open);
	CHECK(spectrum_adjust(&s, SPECTRUM_SCALE, -20, 130) && s.audio_open);
	CHECK(!spectrum_adjust(&s, SPECTRUM_SCALE, -10, 130));
	uint32_t frequency = spectrum_frequency(&s, s.tuner);
	CHECK(spectrum_adjust(&s, SPECTRUM_FILTER, 1, 130));
	CHECK(s.narrow && spectrum_frequency(&s, s.tuner) == frequency && s.scale_db == -20 &&
	      s.auto_peak == SPECTRUM_AUTO_IMMEDIATE);
	CHECK(s.level[0] == SPECTRUM_EMPTY && spectrum_peak_level(&s, s.tuner) == SPECTRUM_EMPTY);
	CHECK(!spectrum_adjust(&s, SPECTRUM_FILTER, 2, 130));
	CHECK(spectrum_adjust(&s, SPECTRUM_SQUELCH, -120, 130));
	CHECK(spectrum_adjust(&s, SPECTRUM_STEP, 6250, 130));
	CHECK(s.dwell_ms == 100 && s.auto_peak == SPECTRUM_AUTO_IMMEDIATE && s.scale_db == -20 && s.squelch_dbm == -120 && s.narrow);
	CHECK(spectrum_recenter(&s, 145062500, 136000000, 174000000, 140));
	CHECK(s.center_hz == 145062500 && s.bins == 65 && s.step_hz == 6250 && s.dwell_ms == 100 &&
	      s.auto_peak == SPECTRUM_AUTO_IMMEDIATE && s.scale_db == -20 && s.squelch_dbm == -120 && s.narrow);
	CHECK(spectrum_adjust(&s, SPECTRUM_BANDWIDTH, 97, 140) && s.auto_peak == SPECTRUM_AUTO_IMMEDIATE);
	CHECK(spectrum_adjust(&s, SPECTRUM_CENTER, 146520000, 140) && s.auto_peak == SPECTRUM_AUTO_IMMEDIATE);
	CHECK(spectrum_adjust(&s, SPECTRUM_FILTER, 0, 140) && !s.narrow);
	before = s;
	CHECK(!spectrum_recenter(&s, 800000000, 136000000, 174000000, 150));
	CHECK(memcmp(&before, &s, sizeof s) == 0);

	// Auto off plots strong off-tuner signals without moving the manual tuner or opening their audio.
	CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, 12500, 65, 0));
	CHECK(spectrum_adjust(&s, SPECTRUM_AUTO_PEAK, 0, 0));
	for (now = 10; now < 2000 && !s.sweeps; now += 10) {
		CHECK(spectrum_sample(&s, s.listening || s.index == s.tuner ? -120 : -50, now));
		CHECK(s.tuner == 32 && !s.audio_open);
	}
	CHECK(s.sweeps == 1 && spectrum_level_dbm(spectrum_peak_level(&s, 0)) == -50);
	CHECK(spectrum_adjust(&s, SPECTRUM_TUNER, 1, now));
	CHECK(spectrum_sample(&s, -70, now + 10) && s.audio_open && s.tuner == 33);

	// A cached peak that disappeared before confirmation never opens the speaker.
	CHECK(spectrum_start(&s, 446000000, 400000000, 480000000, 12500, 65, 0));
	CHECK(spectrum_sample(&s, -120, 10));
	for (unsigned i = 0; i < s.bins; i++)
		CHECK(spectrum_sample(&s, i == 5 ? -80 : -120, 20 + 10 * i));
	CHECK(s.tuner == 5 && s.listening && !s.audio_open);
	CHECK(spectrum_sample(&s, -120, 670) && !s.audio_open && !s.listening);
	printf("spectrumTest: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
