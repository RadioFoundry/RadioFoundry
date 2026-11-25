// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Sandro Kalatozishvili
#include <stdio.h>
#include <string.h>
#include "radio/spectrum.h"

static unsigned failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
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
	for (unsigned i = 0; i < SPECTRUM_BINS; i++) spectrum_adjust(&s, SPECTRUM_TUNER, -1, 1360);
	CHECK(s.tuner == 0 && !spectrum_adjust(&s, SPECTRUM_TUNER, -1, 1360));
	for (unsigned i = 0; i < SPECTRUM_BINS; i++) spectrum_adjust(&s, SPECTRUM_TUNER, 1, 1360);
	CHECK(s.tuner == SPECTRUM_BINS - 1 && !spectrum_adjust(&s, SPECTRUM_TUNER, 1, 1360));
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
