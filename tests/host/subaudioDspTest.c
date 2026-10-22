// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Software CTCSS / DCS detection and the tone generators (drivers/rf/subaudio): synthetic signals with a DC offset,
// noise, neighboring tones, a phase reversal, dropouts, and the generator fed back into the detector
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "rf/subaudio/subaudio.h"
#include "rf/subaudio/subaudio_rx.h"
#include "rf/subaudio/subaudio_tx.h"
#include "radio/tones.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define FS	2613u		// the MD-380's ADC rate per channel (hardware-md380.md 6.4)
#define PI	3.14159265358979

static uint32_t rng = 12345;

// Uniform noise in -1..1
static double noise(void)
{
	rng = rng * 1664525u + 1013904223u;
	return ((double)(rng >> 8) / 8388608.0) - 1.0;
}

static int16_t counts(double v)
{
	v += 2048.0;
	return (int16_t)(v < 0 ? 0 : v > 4095 ? 4095 : lrint(v));
}

// Feed seconds of a CTCSS tone (amp counts) in noise (counts peak); returns the time the match first held, or -1
static double run_tone(subaudio_rx_t *d, double hz, double amp, double nz, double seconds, double *t0)
{
	double first = -1;
	unsigned n = (unsigned)(seconds * FS);
	for (unsigned i = 0; i < n; i++) {
		double t = *t0 + (double)i / FS;
		subaudio_rx_feed(d, counts(amp * sin(2 * PI * hz * t) + nz * noise()));
		if (first < 0 && subaudio_rx_match(d)) {
			first = (double)i / FS;
		}
	}
	*t0 += seconds;
	return first;
}

static void test_ctcss_all(void)
{
	for (unsigned i = 0; i < TONES_CTCSS_COUNT; i++) {
		subaudio_rx_t d;
		subaudio_rx_init(&d, FS, 0);
		subaudio_rx_set(&d, SUBAUDIO_CTCSS, tones_ctcss[i], false);
		double t = 0, hz = tones_ctcss[i] / 10.0;
		double first = run_tone(&d, hz, 150, 150, 1.5, &t);	// amplitude 150 against noise up to +-150
		CHECK(first >= 0 && first < 0.7);
		CHECK(subaudio_rx_match(&d));
	}
}

// No other standard tone opens the detector, in particular the neighbors
static void test_ctcss_rejects_others(void)
{
	for (unsigned i = 0; i < TONES_CTCSS_COUNT; i++) {
		for (unsigned j = 0; j < TONES_CTCSS_COUNT; j++) {
			if (i == j) {
				continue;
			}
			if (abs((int)i - (int)j) > 3 && (i % 7) != 0) {
				continue;	// the far ones on a sample of tones; every near pair is tried
			}
			subaudio_rx_t d;
			subaudio_rx_init(&d, FS, 0);
			subaudio_rx_set(&d, SUBAUDIO_CTCSS, tones_ctcss[i], false);
			double t = 0;
			double first = run_tone(&d, tones_ctcss[j] / 10.0, 150, 60, 2.0, &t);
			if (first >= 0) {
				printf("tone %u opened on %u\n", tones_ctcss[i], tones_ctcss[j]);
			}
			CHECK(first < 0);
		}
	}
}

// Transmitters are a little off frequency
static void test_ctcss_offset(void)
{
	static const double off[] = { -1.0, -0.5, 0.5, 1.0 };
	for (unsigned k = 0; k < 4; k++) {
		for (unsigned i = 0; i < TONES_CTCSS_COUNT; i += 5) {
			subaudio_rx_t d;
			subaudio_rx_init(&d, FS, 0);
			subaudio_rx_set(&d, SUBAUDIO_CTCSS, tones_ctcss[i], false);
			double t = 0;
			double first = run_tone(&d, tones_ctcss[i] / 10.0 + off[k], 150, 60, 1.5, &t);
			if (first < 0) {
				printf("tone %u not heard at %+.1f Hz\n", tones_ctcss[i], off[k]);
			}
			CHECK(first >= 0);
		}
	}
}

static void test_ctcss_noise_only(void)
{
	for (unsigned i = 0; i < TONES_CTCSS_COUNT; i += 3) {
		subaudio_rx_t d;
		subaudio_rx_init(&d, FS, 0);
		subaudio_rx_set(&d, SUBAUDIO_CTCSS, tones_ctcss[i], false);
		double t = 0;
		CHECK(run_tone(&d, 100, 0, 400, 60.0, &t) < 0);
		// A loud tone elsewhere in the band that is not on the list, plus noise
		CHECK(run_tone(&d, 40.0, 300, 100, 5.0, &t) < 0);
	}
}

// Below the amplitude floor nothing is heard; just above it the tone is
static void test_ctcss_level(void)
{
	subaudio_rx_t d;
	subaudio_rx_init(&d, FS, 0);
	subaudio_rx_set(&d, SUBAUDIO_CTCSS, 1000, false);
	double t = 0;
	CHECK(run_tone(&d, 100.0, 4, 0, 3.0, &t) < 0);
	CHECK(run_tone(&d, 100.0, 16, 2, 3.0, &t) >= 0);
}

// The tone ends: the detector closes within 0.6 s and opens again with the next one
static void test_ctcss_drop_and_phase_reversal(void)
{
	subaudio_rx_t d;
	subaudio_rx_init(&d, FS, 0);
	subaudio_rx_set(&d, SUBAUDIO_CTCSS, 1318, false);
	double t = 0;
	CHECK(run_tone(&d, 131.8, 150, 40, 1.0, &t) >= 0);
	// A 150 ms phase reversal (the reverse burst) is ridden out
	bool dropped = false;
	for (unsigned i = 0; i < (unsigned)(0.15 * FS); i++) {
		subaudio_rx_feed(&d, counts(-150 * sin(2 * PI * 131.8 * (t + (double)i / FS)) + 40 * noise()));
		dropped |= !subaudio_rx_match(&d);
	}
	t += 0.15;
	CHECK(!dropped);
	// Then the tone is gone
	unsigned n = (unsigned)(1.0 * FS), closed_at = 0;
	for (unsigned i = 0; i < n; i++) {
		subaudio_rx_feed(&d, counts(40 * noise()));
		if (!closed_at && !subaudio_rx_match(&d)) {
			closed_at = i;
		}
	}
	CHECK(closed_at > 0 && closed_at < (unsigned)(0.6 * FS));
	CHECK(run_tone(&d, 131.8, 150, 40, 1.0, &t) >= 0);
}

// A short fade does not close it
static void test_ctcss_fade(void)
{
	subaudio_rx_t d;
	subaudio_rx_init(&d, FS, 0);
	subaudio_rx_set(&d, SUBAUDIO_CTCSS, 1000, false);
	double t = 0;
	CHECK(run_tone(&d, 100.0, 150, 40, 1.0, &t) >= 0);
	bool dropped = false;
	for (unsigned i = 0; i < (unsigned)(0.12 * FS); i++) {
		subaudio_rx_feed(&d, counts(40 * noise()));
		dropped |= !subaudio_rx_match(&d);
	}
	CHECK(!dropped);
}

// --- DCS ---

// A DCS word as a sample stream: bits at baud x skew, smoothed like the radio's filter, noise, a DC offset
typedef struct {
	double phase;	// in bits
	uint32_t word;
	double level, amp, nz, skew;
} dcs_gen_t;

static void dcs_gen_init(dcs_gen_t *g, uint16_t code, bool inverted, double amp, double nz, double skew, double start)
{
	uint32_t w = subaudio_dcs_word(code);
	g->word = inverted ? ~w & 0x7FFFFFu : w;
	g->phase = start;
	g->level = 0;
	g->amp = amp;
	g->nz = nz;
	g->skew = skew;
}

static int16_t dcs_gen_next(dcs_gen_t *g)
{
	unsigned bit = (unsigned)g->phase % 23u;
	double target = (g->word >> bit) & 1u ? g->amp : -g->amp;
	g->level += (target - g->level) * 0.25;	// a corner near 150 Hz at this rate
	g->phase += 134.4 * g->skew / FS;
	return counts(g->level + g->nz * noise());
}

static double run_dcs(subaudio_rx_t *d, dcs_gen_t *g, double seconds)
{
	double first = -1;
	unsigned n = (unsigned)(seconds * FS);
	for (unsigned i = 0; i < n; i++) {
		subaudio_rx_feed(d, dcs_gen_next(g));
		if (first < 0 && subaudio_rx_match(d)) {
			first = (double)i / FS;
		}
	}
	return first;
}

static void test_dcs_codes(void)
{
	for (unsigned i = 0; i < TONES_DCS_COUNT; i++) {
		for (int inv = 0; inv < 2; inv++) {
			subaudio_rx_t d;
			dcs_gen_t g;
			subaudio_rx_init(&d, FS, 0);
			subaudio_rx_set(&d, inv ? SUBAUDIO_DCS_INV : SUBAUDIO_DCS, tones_dcs[i], false);
			dcs_gen_init(&g, tones_dcs[i], inv, 200, 40, 1.0, (double)(i * 5 % 23) + 0.37);
			double first = run_dcs(&d, &g, 2.0);
			CHECK(first >= 0 && first < 1.2);
			CHECK(subaudio_rx_match(&d));
		}
	}
}

static void test_dcs_rejects(void)
{
	for (unsigned i = 0; i < TONES_DCS_COUNT; i += 3) {
		subaudio_rx_t d;
		dcs_gen_t g;
		// The wrong code
		subaudio_rx_init(&d, FS, 0);
		subaudio_rx_set(&d, SUBAUDIO_DCS, tones_dcs[i], false);
		dcs_gen_init(&g, tones_dcs[(i + 1) % TONES_DCS_COUNT], false, 200, 40, 1.0, 0);
		CHECK(run_dcs(&d, &g, 3.0) < 0);
		// The right code but the other polarity
		subaudio_rx_set(&d, SUBAUDIO_DCS, tones_dcs[i], false);
		dcs_gen_init(&g, tones_dcs[i], true, 200, 40, 1.0, 0);
		double first = run_dcs(&d, &g, 3.0);
		// An inverted word can look like another code's word (D023N = D047I): only for those pairs
		CHECK(first < 0 || tones_dcs[i] == 023 || tones_dcs[i] == 047);
		// Noise only
		subaudio_rx_set(&d, SUBAUDIO_DCS, tones_dcs[i], false);
		dcs_gen_init(&g, tones_dcs[i], false, 0, 300, 1.0, 0);
		CHECK(run_dcs(&d, &g, 20.0) < 0);
	}
}

// The path inverts: DCS_INV and the invert knob together are the normal word again
static void test_dcs_invert_knob(void)
{
	subaudio_rx_t d;
	dcs_gen_t g;
	subaudio_rx_init(&d, FS, 0);
	subaudio_rx_set(&d, SUBAUDIO_DCS, 0155, true);
	dcs_gen_init(&g, 0155, true, 200, 30, 1.0, 3.3);
	CHECK(run_dcs(&d, &g, 2.0) >= 0);
	subaudio_rx_set(&d, SUBAUDIO_DCS_INV, 0155, true);
	dcs_gen_init(&g, 0155, false, 200, 30, 1.0, 3.3);
	CHECK(run_dcs(&d, &g, 2.0) >= 0);
}

// The sender's bit rate is a little off, and the signal has a DC step and noise
static void test_dcs_rate_error_and_noise(void)
{
	static const double skew[] = { 0.99, 0.995, 1.005, 1.01 };
	for (unsigned k = 0; k < 4; k++) {
		subaudio_rx_t d;
		dcs_gen_t g;
		subaudio_rx_init(&d, FS, 0);
		subaudio_rx_set(&d, SUBAUDIO_DCS, 0223, false);
		dcs_gen_init(&g, 0223, false, 200, 80, skew[k], 1.9);
		CHECK(run_dcs(&d, &g, 3.0) >= 0);
		CHECK(subaudio_rx_match(&d));
	}
}

static void test_dcs_drop(void)
{
	subaudio_rx_t d;
	dcs_gen_t g;
	subaudio_rx_init(&d, FS, 0);
	subaudio_rx_set(&d, SUBAUDIO_DCS, 0114, false);
	dcs_gen_init(&g, 0114, false, 200, 30, 1.0, 0);
	CHECK(run_dcs(&d, &g, 1.5) >= 0);
	dcs_gen_init(&g, 0114, false, 0, 30, 1.0, 0);
	unsigned n = (unsigned)(1.5 * FS), closed = 0;
	for (unsigned i = 0; i < n; i++) {
		subaudio_rx_feed(&d, dcs_gen_next(&g));
		if (!closed && !subaudio_rx_match(&d)) {
			closed = i;
		}
	}
	CHECK(closed > 0 && closed < (unsigned)(0.9 * FS));
}

static void test_none_and_reset(void)
{
	subaudio_rx_t d;
	subaudio_rx_init(&d, FS, 0);
	subaudio_rx_set(&d, SUBAUDIO_NONE, 0, false);
	CHECK(subaudio_rx_match(&d));
	subaudio_rx_set(&d, SUBAUDIO_CTCSS, 1000, false);
	CHECK(!subaudio_rx_match(&d));	// set() starts from closed
	double t = 0;
	run_tone(&d, 100.0, 150, 0, 1.0, &t);
	CHECK(subaudio_rx_match(&d));
	subaudio_rx_set(&d, SUBAUDIO_CTCSS, 1000, false);
	CHECK(!subaudio_rx_match(&d));
}

// --- Generators ---

#define TXRATE	4100u
#define FULL	1023u

static void test_tx_ctcss_frequency_and_shape(void)
{
	for (unsigned i = 0; i < TONES_CTCSS_COUNT; i += 7) {
		subaudio_tx_t g;
		subaudio_tx_init(&g, TXRATE, FULL, 50);
		subaudio_tx_start(&g, SUBAUDIO_CTCSS, tones_ctcss[i], false);
		double hz = tones_ctcss[i] / 10.0, worst = 0;
		int min = 9999, max = -1;
		unsigned n = 2 * TXRATE, up = 0;
		int prev = 0;
		for (unsigned k = 0; k < n; k++) {
			int v = subaudio_tx_next(&g);
			if (k > TXRATE / 20) {	// past the ramp
				double ideal = FULL / 2.0 + 0.5 * (FULL / 2.0) * sin(2 * PI * hz * k / TXRATE);
				double e = fabs(v - ideal);
				if (e > worst) {
					worst = e;
				}
				if (v < min) {
					min = v;
				}
				if (v > max) {
					max = v;
				}
				up += prev < (int)(FULL / 2) && v >= (int)(FULL / 2);
			}
			prev = v;
		}
		CHECK(worst < 3.0);	// counts of 1023: a little over 0.5 % of the swing is rounding
		CHECK(max - min > 505 && max - min < 517);	// 50 % of half range, both ways
		double secs = (double)(n - TXRATE / 20) / TXRATE;
		CHECK(fabs(up / secs - hz) < 1.0);
	}
}

static void test_tx_ramp(void)
{
	subaudio_tx_t g;
	subaudio_tx_init(&g, TXRATE, FULL, 100);
	subaudio_tx_start(&g, SUBAUDIO_CTCSS, 2541, false);
	int first_peak = 0;
	for (unsigned k = 0; k < TXRATE / 100; k++) {	// first 10 ms
		int d = abs((int)subaudio_tx_next(&g) - (int)(FULL / 2));
		if (d > first_peak) {
			first_peak = d;
		}
	}
	CHECK(first_peak < 300);	// well under the full swing of 511
	subaudio_tx_init(&g, TXRATE, FULL, 100);
	subaudio_tx_start(&g, SUBAUDIO_NONE, 0, false);
	CHECK(subaudio_tx_next(&g) == FULL / 2);
}

static void test_tx_dcs_bits(void)
{
	subaudio_tx_t g;
	for (int inv = 0; inv < 2; inv++) {
		subaudio_tx_init(&g, TXRATE, FULL, 100);
		subaudio_tx_start(&g, inv ? SUBAUDIO_DCS_INV : SUBAUDIO_DCS, 0171 + 1, false);
		uint32_t want = subaudio_dcs_word(0172);
		if (inv) {
			want = ~want & 0x7FFFFFu;
		}
		// Sample at the middle of each bit, three words, skipping the first (the ramp)
		uint32_t got[3] = { 0 };
		unsigned bad = 0;
		unsigned total = 3 * 23;
		unsigned k = 0;
		for (unsigned b = 0; b < total; b++) {
			unsigned target = (unsigned)((b + 0.5) * TXRATE / 134.4);
			int v = 0;
			for (; k <= target; k++) {
				v = subaudio_tx_next(&g);
			}
			bool one = v > (int)(FULL / 2);
			got[b / 23] |= (uint32_t)one << (b % 23);
		}
		for (int w = 1; w < 3; w++) {
			bad += got[w] != want;
		}
		CHECK(bad == 0);
	}
}

// The generator into the detector, sampled at the detector's rate
static void test_loopback(void)
{
	for (unsigned i = 0; i < TONES_PICKER_COUNT; i += 3) {
		subtone_t st = tones_picker(i);
		if (st.type == TONE_NONE) {
			continue;
		}
		subaudio_kind_t kind = st.type == TONE_CTCSS ? SUBAUDIO_CTCSS : st.type == TONE_DCS ? SUBAUDIO_DCS : SUBAUDIO_DCS_INV;
		subaudio_tx_t g;
		subaudio_rx_t d;
		subaudio_tx_init(&g, TXRATE, FULL, 40);
		subaudio_tx_start(&g, kind, st.value, false);
		subaudio_rx_init(&d, FS, 0);
		subaudio_rx_set(&d, kind, st.value, false);
		// The pin's RC filter and the ADC's offset are modelled by a first-order low-pass and the mid-scale offset
		double lp = FULL / 2.0, acc = 0, tnext = 0;
		unsigned n = 3 * TXRATE, heard = 0;
		for (unsigned k = 0; k < n; k++) {
			lp += (subaudio_tx_next(&g) - lp) * 0.3;
			if (k / (double)TXRATE >= tnext) {
				acc = lp - FULL / 2.0;
				subaudio_rx_feed(&d, counts(acc * 0.4 + 30 * noise()));
				tnext += 1.0 / FS;
			}
			heard |= subaudio_rx_match(&d);
		}
		CHECK(heard);
		CHECK(subaudio_rx_match(&d));
	}
}

int main(void)
{
	test_ctcss_all();
	test_ctcss_rejects_others();
	test_ctcss_offset();
	test_ctcss_noise_only();
	test_ctcss_level();
	test_ctcss_drop_and_phase_reversal();
	test_ctcss_fade();
	test_dcs_codes();
	test_dcs_rejects();
	test_dcs_invert_knob();
	test_dcs_rate_error_and_noise();
	test_dcs_drop();
	test_none_and_reset();
	test_tx_ctcss_frequency_and_shape();
	test_tx_ramp();
	test_tx_dcs_bits();
	test_loopback();
	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("subaudio DSP: all checks passed\n");
	return 0;
}
